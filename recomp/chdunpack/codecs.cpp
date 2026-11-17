// Deflate, LZMA, CHD's Huffman code and SHA-1 for chdunpack. Written for this project from the public descriptions:
//   deflate: RFC 1951; LZMA: Igor Pavlov's lzma-specification (public domain); SHA-1: FIPS 180-4;
//   the CHD Huffman code: the CHD v5 format as MAME's chd.h and codecs describe it (libchdr, BSD-3-Clause, was read to learn
//   it; nothing is copied). The same rules as scripts/chd.py, which is the reference.
#include "codecs.hpp"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------------------------------------------------
// Deflate
// ---------------------------------------------------------------------------------------------------------------------
namespace {

struct InBits {
    const uint8_t *p;
    size_t n, pos = 0;  // pos counts bytes taken into acc (also zero bytes past the end)
    uint64_t acc = 0;
    int cnt = 0;
    InBits(const uint8_t *p_, size_t n_) : p(p_), n(n_) {}
    void refill() {
        while (cnt <= 56) {
            const uint64_t b = pos < n ? p[pos] : 0;
            ++pos;
            acc |= b << cnt;
            cnt += 8;
        }
        if (pos > n + 16) throw ChdError("deflate data ends too early");
    }
    uint32_t bits(int k) {  // k <= 16 and refill() was called not long ago
        const uint32_t v = (uint32_t)(acc & ((1u << k) - 1));
        acc >>= k;
        cnt -= k;
        return v;
    }
    size_t consumed_bits() const { return pos * 8 - (size_t)cnt; }
};

constexpr int kFast = 10;

struct Inf {
    uint16_t count[16];
    uint16_t symbol[288];
    uint16_t fast[1 << kFast];  // (length << 9) | symbol, 0 when the code is longer than kFast
};

void build(Inf &h, const uint8_t *len, int n) {
    std::memset(&h, 0, sizeof h);
    for (int i = 0; i < n; ++i) ++h.count[len[i]];
    h.count[0] = 0;
    int left = 1;
    for (int l = 1; l <= 15; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) throw ChdError("bad deflate code lengths");
    }
    uint16_t offs[17] = {};
    for (int l = 1; l <= 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
    for (int i = 0; i < n; ++i)
        if (len[i]) h.symbol[offs[len[i]]++] = (uint16_t)i;
    int idx = 0;
    uint32_t code = 0;
    for (int l = 1; l <= 15; ++l) {
        for (int k = 0; k < h.count[l]; ++k, ++code) {
            const int sym = h.symbol[idx++];
            if (l <= kFast) {
                uint32_t rev = 0;
                for (int b = 0; b < l; ++b) rev |= ((code >> b) & 1u) << (l - 1 - b);
                for (uint32_t j = rev; j < (1u << kFast); j += 1u << l) h.fast[j] = (uint16_t)((l << 9) | sym);
            }
        }
        code <<= 1;
    }
}

int decode_sym(InBits &in, const Inf &h) {
    const uint16_t e = h.fast[in.acc & ((1u << kFast) - 1)];
    if (e) {
        const int l = e >> 9;
        in.acc >>= l;
        in.cnt -= l;
        return e & 511;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; ++l) {
        code |= (int)((in.acc >> (l - 1)) & 1);
        const int count = h.count[l];
        if (code - count < first) {
            in.acc >>= l;
            in.cnt -= l;
            return h.symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw ChdError("bad deflate code");
}

const uint16_t kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

[[noreturn]] void wrong_size() { throw ChdError("a hunk came out with the wrong size"); }

}  // namespace

size_t inflate_raw(const uint8_t *in_p, size_t n, uint8_t *out, size_t cap) {
    InBits in(in_p, n);
    size_t o = 0;
    thread_local Inf lit, dist;
    static const struct Fixed {
        Inf lit, dist;
        Fixed() {
            uint8_t l[288], d[30];
            for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            for (int i = 0; i < 30; ++i) d[i] = 5;
            build(lit, l, 288);
            build(dist, d, 30);
        }
    } fixed;
    for (;;) {
        in.refill();
        const uint32_t last = in.bits(1);
        const uint32_t type = in.bits(2);
        if (type == 0) {
            in.bits(in.cnt & 7);
            in.refill();
            const uint32_t len = in.bits(16), nlen = in.bits(16);
            if (len != (~nlen & 0xFFFFu)) throw ChdError("bad deflate stored block");
            for (uint32_t i = 0; i < len; ++i) {
                if (in.cnt < 8) in.refill();
                if (o >= cap) wrong_size();
                out[o++] = (uint8_t)in.bits(8);
            }
        } else if (type == 1 || type == 2) {
            const Inf *lc = &fixed.lit, *dc = &fixed.dist;
            if (type == 2) {
                const int nlen = (int)in.bits(5) + 257, ndist = (int)in.bits(5) + 1, ncode = (int)in.bits(4) + 4;
                if (nlen > 286 || ndist > 30) throw ChdError("bad deflate block header");
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t lengths[320] = {};
                for (int i = 0; i < ncode; ++i) {
                    in.refill();
                    lengths[order[i]] = (uint8_t)in.bits(3);
                }
                Inf cl;
                build(cl, lengths, 19);
                uint8_t ll[320] = {};
                int idx = 0;
                while (idx < nlen + ndist) {
                    in.refill();
                    int sym = decode_sym(in, cl);
                    if (sym < 16) {
                        ll[idx++] = (uint8_t)sym;
                    } else {
                        int rep, val = 0;
                        if (sym == 16) {
                            if (idx == 0) throw ChdError("bad deflate lengths");
                            val = ll[idx - 1];
                            rep = 3 + (int)in.bits(2);
                        } else if (sym == 17) {
                            rep = 3 + (int)in.bits(3);
                        } else {
                            rep = 11 + (int)in.bits(7);
                        }
                        if (idx + rep > nlen + ndist) throw ChdError("bad deflate lengths");
                        while (rep--) ll[idx++] = (uint8_t)val;
                    }
                }
                if (ll[256] == 0) throw ChdError("bad deflate lengths");
                build(lit, ll, nlen);
                build(dist, ll + nlen, ndist);
                lc = &lit;
                dc = &dist;
            }
            for (;;) {
                in.refill();
                int sym = decode_sym(in, *lc);
                if (sym < 256) {
                    if (o >= cap) wrong_size();
                    out[o++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) throw ChdError("bad deflate length");
                    const size_t len = kLBase[sym] + in.bits(kLExt[sym]);
                    const int ds = decode_sym(in, *dc);
                    if (ds >= 30) throw ChdError("bad deflate distance");
                    const size_t d = kDBase[ds] + in.bits(kDExt[ds]);
                    if (d > o) throw ChdError("bad deflate distance");
                    if (o + len > cap) wrong_size();
                    for (size_t i = 0; i < len; ++i, ++o) out[o] = out[o - d];
                }
            }
        } else {
            throw ChdError("bad deflate block");
        }
        if (in.consumed_bits() > n * 8) throw ChdError("deflate data ends too early");
        if (last) break;
    }
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// LZMA (raw stream, lc=3 lp=0 pb=2). The dictionary is the output itself: a hunk is decoded in one piece.
// ---------------------------------------------------------------------------------------------------------------------
namespace {

struct LzmaLen {
    uint16_t choice, choice2, low[16][8], mid[16][8], high[256];
};

struct LzmaProbs {
    uint16_t is_match[12 << 4], is_rep[12], is_rep_g0[12], is_rep_g1[12], is_rep_g2[12], is_rep0_long[12 << 4];
    uint16_t pos_slot[4][64], pos_special[115], align[16];
    LzmaLen len, rep_len;
    uint16_t lit[0x300 << 3];
};

struct Rc {
    const uint8_t *p, *end;
    uint32_t range = 0xFFFFFFFFu, code = 0;
    size_t over = 0;  // bytes asked for beyond the end
    inline uint8_t next() {
        if (p < end) return *p++;
        ++over;
        return 0;
    }
    inline void norm() {
        if (range < (1u << 24)) {
            range <<= 8;
            code = (code << 8) | next();
        }
    }
    inline uint32_t bit(uint16_t &prob) {
        norm();
        const uint32_t bound = (range >> 11) * prob;
        if (code < bound) {
            range = bound;
            prob = (uint16_t)(prob + ((2048 - prob) >> 5));
            return 0;
        }
        range -= bound;
        code -= bound;
        prob = (uint16_t)(prob - (prob >> 5));
        return 1;
    }
    uint32_t direct(int n) {
        uint32_t res = 0;
        while (n--) {
            norm();
            range >>= 1;
            code -= range;
            const uint32_t t = 0u - (code >> 31);
            code += range & t;
            res = (res << 1) + (t + 1);
        }
        return res;
    }
    uint32_t tree(uint16_t *probs, int nbits) {
        uint32_t m = 1;
        for (int i = 0; i < nbits; ++i) m = (m << 1) + bit(probs[m]);
        return m - (1u << nbits);
    }
    uint32_t tree_rev(uint16_t *probs, int nbits) {
        uint32_t m = 1, sym = 0;
        for (int i = 0; i < nbits; ++i) {
            const uint32_t b = bit(probs[m]);
            m = (m << 1) + b;
            sym |= b << i;
        }
        return sym;
    }
    uint32_t length(LzmaLen &l, uint32_t pos_state) {
        if (!bit(l.choice)) return tree(l.low[pos_state], 3);
        if (!bit(l.choice2)) return 8 + tree(l.mid[pos_state], 3);
        return 16 + tree(l.high, 8);
    }
};

}  // namespace

size_t lzma_raw(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    constexpr int lc = 3, lp = 0, pb = 2;
    if (n < 5 || in[0] != 0) throw ChdError("bad LZMA data");
    thread_local LzmaProbs P;
    {
        uint16_t *q = reinterpret_cast<uint16_t *>(&P);
        for (size_t i = 0; i < sizeof(P) / sizeof(uint16_t); ++i) q[i] = 1024;
    }
    Rc rc{in + 5, in + n};
    rc.code = ((uint32_t)in[1] << 24) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 8) | in[4];
    uint32_t state = 0, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    size_t pos = 0;
    while (pos < cap) {
        const uint32_t pos_state = (uint32_t)pos & ((1u << pb) - 1);
        if (!rc.bit(P.is_match[(state << 4) | pos_state])) {
            const uint32_t prev = pos ? out[pos - 1] : 0;
            uint16_t *probs = P.lit + 0x300 * ((((uint32_t)pos & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)));
            uint32_t symbol = 1;
            if (state >= 7) {
                if (rep0 >= pos) throw ChdError("bad LZMA data");
                uint32_t match_byte = out[pos - rep0 - 1];
                do {
                    const uint32_t match_bit = (match_byte >> 7) & 1;
                    match_byte <<= 1;
                    const uint32_t b = rc.bit(probs[((1 + match_bit) << 8) + symbol]);
                    symbol = (symbol << 1) | b;
                    if (match_bit != b) break;
                } while (symbol < 0x100);
            }
            while (symbol < 0x100) symbol = (symbol << 1) | rc.bit(probs[symbol]);
            out[pos++] = (uint8_t)symbol;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        uint32_t len;
        if (rc.bit(P.is_rep[state])) {
            if (pos == 0) throw ChdError("bad LZMA data");
            if (!rc.bit(P.is_rep_g0[state])) {
                if (!rc.bit(P.is_rep0_long[(state << 4) | pos_state])) {
                    if (rep0 >= pos) throw ChdError("bad LZMA data");
                    state = state < 7 ? 9 : 11;
                    out[pos] = out[pos - rep0 - 1];
                    ++pos;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc.bit(P.is_rep_g1[state])) {
                    dist = rep1;
                } else {
                    if (!rc.bit(P.is_rep_g2[state])) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = rc.length(P.rep_len, pos_state);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = rc.length(P.len, pos_state);
            state = state < 7 ? 7 : 10;
            const uint32_t len_state = len < 4 ? len : 3;
            const uint32_t slot = rc.tree(P.pos_slot[len_state], 6);
            if (slot < 4) {
                rep0 = slot;
            } else {
                const int num_direct = (int)(slot >> 1) - 1;
                uint32_t dist = (2 | (slot & 1)) << num_direct;
                if (slot < 14) {
                    dist += rc.tree_rev(P.pos_special + dist - slot, num_direct);
                } else {
                    dist += rc.direct(num_direct - 4) << 4;
                    dist += rc.tree_rev(P.align, 4);
                }
                rep0 = dist;
            }
            if (rep0 == 0xFFFFFFFFu) break;  // end marker
        }
        len += 2;
        if (rep0 >= pos) throw ChdError("bad LZMA data");
        if (len > cap - pos) throw ChdError("a hunk came out with the wrong size");
        const size_t d = (size_t)rep0 + 1;
        if (d >= len) {
            std::memcpy(out + pos, out + pos - d, len);
            pos += len;
        } else {
            for (uint32_t i = 0; i < len; ++i, ++pos) out[pos] = out[pos - d];
        }
    }
    if (rc.over) throw ChdError("LZMA data ends too early");
    return pos;
}

// ---------------------------------------------------------------------------------------------------------------------
// CHD Huffman
// ---------------------------------------------------------------------------------------------------------------------
uint32_t BitsMsb::peek(int bits) const {
    const size_t byte = pos >> 3;
    const int off = (int)(pos & 7);
    uint64_t v = 0;
    for (size_t i = 0; i < 5; ++i) v = (v << 8) | (byte + i < n_ ? p_[byte + i] : 0);
    return (uint32_t)((v >> (40 - off - bits)) & ((1ull << bits) - 1));
}

HuffTree::HuffTree(int numcodes, int maxbits) : lengths((size_t)numcodes, 0), numcodes_(numcodes), maxbits_(maxbits) {}

void HuffTree::assign() {
    uint64_t hist[33] = {};
    for (uint8_t l : lengths) {
        if (l > maxbits_) throw ChdError("Huffman code too long");
        ++hist[l];
    }
    uint64_t start = 0;
    for (int l = 32; l >= 1; --l) {
        const uint64_t nxt = (start + hist[l]) >> 1;
        hist[l] = start;
        start = nxt;
    }
    uint64_t first[33];
    std::memcpy(first, hist, sizeof first);
    look_ = std::min(maxbits_, 10);
    table_.assign((size_t)1 << look_, 0);
    std::vector<uint32_t> per_len_count(33, 0);
    std::vector<std::pair<uint32_t, uint16_t>> slow;  // (code, symbol) of the long codes
    std::vector<uint64_t> codes(lengths.size());
    for (size_t sym = 0; sym < lengths.size(); ++sym) {
        const int l = lengths[sym];
        if (!l) continue;
        const uint64_t code = hist[l]++;
        codes[sym] = code;
        if (l <= look_) {
            const int shift = look_ - l;
            const uint64_t lo = code << shift;
            const uint64_t hi = std::min<uint64_t>(lo + ((uint64_t)1 << shift), table_.size());
            for (uint64_t j = lo; j < hi; ++j) table_[j] = (uint16_t)((sym << 5) | (size_t)l);
        } else {
            ++per_len_count[l];
        }
    }
    slow_syms_.clear();
    for (int l = look_ + 1; l <= maxbits_; ++l) {
        slow_first_[l] = (uint32_t)first[l];
        slow_count_[l] = per_len_count[l];
        slow_off_[l] = (uint32_t)slow_syms_.size();
        for (size_t sym = 0; sym < lengths.size(); ++sym)
            if (lengths[sym] == l) slow_syms_.push_back((uint16_t)sym);
    }
}

void HuffTree::import_rle(BitsMsb &bits) {
    const int numbits = maxbits_ >= 16 ? 5 : maxbits_ >= 8 ? 4 : 3;
    int cur = 0;
    while (cur < numcodes_) {
        int nb = (int)bits.read(numbits);
        if (nb != 1) {
            lengths[(size_t)cur++] = (uint8_t)nb;
        } else {
            nb = (int)bits.read(numbits);
            if (nb == 1) {
                lengths[(size_t)cur++] = 1;
            } else {
                const int rep = (int)bits.read(numbits) + 3;
                if (cur + rep > numcodes_) throw ChdError("bad Huffman run in the hunk map");
                for (int i = 0; i < rep; ++i) lengths[(size_t)cur++] = (uint8_t)nb;
            }
        }
    }
    assign();
}

void HuffTree::import_huffman(BitsMsb &bits) {
    HuffTree small(24, 6);
    small.lengths[0] = (uint8_t)bits.read(3);
    const int start = (int)bits.read(3) + 1;
    int count = 0;
    for (int i = 1; i < 24; ++i) {
        if (i < start || count == 7) {
            small.lengths[(size_t)i] = 0;
        } else {
            count = (int)bits.read(3);
            small.lengths[(size_t)i] = count == 7 ? 0 : (uint8_t)count;
        }
    }
    small.assign();
    int rlefull = 0;
    for (int t = numcodes_ - 9; t; t >>= 1) ++rlefull;
    int cur = 0, last = 0;
    while (cur < numcodes_) {
        const int value = small.decode(bits);
        if (value) {
            last = value - 1;
            lengths[(size_t)cur++] = (uint8_t)last;
        } else {
            int c = (int)bits.read(3) + 2;
            if (c == 9) c += (int)bits.read(rlefull);
            while (c && cur < numcodes_) {
                lengths[(size_t)cur++] = (uint8_t)last;
                --c;
            }
        }
    }
    assign();
}

int HuffTree::decode(BitsMsb &bits) const {
    const uint16_t e = table_[bits.peek(look_)];
    if (e) {
        bits.pos += e & 31;
        return e >> 5;
    }
    const uint32_t w = bits.peek(maxbits_);
    for (int l = look_ + 1; l <= maxbits_; ++l) {
        const uint32_t c = w >> (maxbits_ - l);
        if (slow_count_[l] && c >= slow_first_[l] && c < slow_first_[l] + slow_count_[l]) {
            bits.pos += (size_t)l;
            return slow_syms_[slow_off_[l] + (c - slow_first_[l])];
        }
    }
    throw ChdError("bad Huffman code");
}

void HuffTree::decode_bytes(const uint8_t *in, size_t n, size_t bitpos, uint8_t *out, size_t size) const {
    size_t bi = bitpos >> 3;
    const int off = (int)(bitpos & 7);
    auto byte = [&](size_t i) -> uint64_t { return i < n ? in[i] : 0; };
    int cnt = 8 - off;
    uint64_t acc = (byte(bi) & (0xFFu >> off)) << (64 - cnt);
    ++bi;
    for (size_t i = 0; i < size; ++i) {
        while (cnt <= 56) {
            acc |= byte(bi++) << (56 - cnt);
            cnt += 8;
        }
        const uint16_t e = table_[acc >> (64 - look_)];
        int len, sym;
        if (e) {
            len = e & 31;
            sym = e >> 5;
        } else {
            const uint32_t w = (uint32_t)(acc >> (64 - maxbits_));
            len = 0;
            sym = 0;
            for (int l = look_ + 1; l <= maxbits_; ++l) {
                const uint32_t c = w >> (maxbits_ - l);
                if (slow_count_[l] && c >= slow_first_[l] && c < slow_first_[l] + slow_count_[l]) {
                    len = l;
                    sym = slow_syms_[slow_off_[l] + (c - slow_first_[l])];
                    break;
                }
            }
            if (!len) throw ChdError("bad Huffman code");
        }
        acc <<= len;
        cnt -= len;
        out[i] = (uint8_t)sym;
    }
    if (bi * 8 - (size_t)cnt > n * 8) throw ChdError("Huffman data ends too early");
}

// ---------------------------------------------------------------------------------------------------------------------
// SHA-1
// ---------------------------------------------------------------------------------------------------------------------
Sha1::Sha1() : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u} {}

static inline uint32_t rol(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

void Sha1::block(const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const uint8_t *data, size_t n) {
    total_ += n;
    if (fill_) {
        const size_t take = std::min(n, 64 - fill_);
        std::memcpy(buf_ + fill_, data, take);
        fill_ += take;
        data += take;
        n -= take;
        if (fill_ == 64) {
            block(buf_);
            fill_ = 0;
        }
    }
    while (n >= 64) {
        block(data);
        data += 64;
        n -= 64;
    }
    if (n) {
        std::memcpy(buf_, data, n);
        fill_ = n;
    }
}

void Sha1::finish(uint8_t digest[20]) {
    const uint64_t bits = total_ * 8;
    uint8_t pad[72] = {0x80};
    const size_t padlen = (fill_ < 56 ? 56 : 120) - fill_;
    for (int i = 0; i < 8; ++i) pad[padlen + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    update(pad, padlen + 8);
    for (int i = 0; i < 5; ++i) {
        digest[4 * i] = (uint8_t)(h_[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(h_[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(h_[i] >> 8);
        digest[4 * i + 3] = (uint8_t)h_[i];
    }
}
