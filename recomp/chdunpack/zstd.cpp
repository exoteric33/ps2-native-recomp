// Zstandard (RFC 8878) for chdunpack: a port of _zstd_pure() and its helpers in scripts/chd.py (the reference). One CHD hunk is one
// frame, but several frames in a row are decoded too. Checksums are not checked (the CHD carries its own).
#include <algorithm>
#include <cstring>
#include <memory>

#include "codecs.hpp"

namespace {

const uint32_t kLlBase[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t kLlBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint32_t kMlBase[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                              35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t kMlBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const int8_t kLlDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const int8_t kMlDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                               1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const int8_t kOfDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

[[noreturn]] void bad(const char *what) { throw ChdError(std::string("Zstandard: ") + what); }

inline int bit_length(uint32_t v) { return v ? 32 - __builtin_clz(v) : 0; }

// A bitstream read backwards: the last byte holds a 1 bit that marks the end; bits come out most significant first.
struct Back {
    const uint8_t *data;
    ptrdiff_t lo, idx;
    int cnt;
    uint64_t acc;
    uint64_t deficit = 0;  // bits asked for beyond the start (they read as zero)
    Back(const uint8_t *d, ptrdiff_t lo_, ptrdiff_t hi) : data(d), lo(lo_) {
        if (hi <= lo || d[hi - 1] == 0) bad("bad bitstream");
        const uint32_t last = d[hi - 1];
        idx = hi - 2;
        cnt = bit_length(last) - 1;
        acc = last & ((1u << cnt) - 1);
    }
    uint64_t read(int n) {
        if (n == 0) return 0;
        if (cnt < n) {
            while (cnt < 56 && idx >= lo) {
                acc = (acc << 8) | data[idx--];
                cnt += 8;
            }
            if (cnt < n) {
                const int miss = n - cnt;
                deficit += (uint64_t)miss;
                const uint64_t v = (acc << miss) & ((1ull << n) - 1);
                acc = 0;
                cnt = 0;
                return v;
            }
        }
        cnt -= n;
        const uint64_t v = acc >> cnt;
        acc &= (1ull << cnt) - 1;
        return v;
    }
    int64_t bits_left() const { return cnt + 8 * (idx - lo + 1); }
};

struct Fse {
    int al = 0;
    std::vector<uint8_t> sym, nbits;
    std::vector<uint32_t> base;
};
using FsePtr = std::shared_ptr<const Fse>;

FsePtr fse_table(int al, const int8_t *probs, size_t np) {
    auto t = std::make_shared<Fse>();
    const size_t size = (size_t)1 << al;
    t->al = al;
    t->sym.assign(size, 0);
    t->nbits.assign(size, 0);
    t->base.assign(size, 0);
    std::vector<uint32_t> nxt(np);
    size_t high = size - 1;
    for (size_t s = 0; s < np; ++s) {
        if (probs[s] == -1) {
            t->sym[high--] = (uint8_t)s;
            nxt[s] = 1;
        } else {
            nxt[s] = (uint32_t)probs[s];
        }
    }
    const size_t step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
    size_t pos = 0;
    for (size_t s = 0; s < np; ++s) {
        for (int k = 0; k < (probs[s] > 0 ? probs[s] : 0); ++k) {
            t->sym[pos] = (uint8_t)s;
            pos = (pos + step) & mask;
            while (pos > high) pos = (pos + step) & mask;
        }
    }
    if (pos != 0) bad("bad FSE table");
    for (size_t i = 0; i < size; ++i) {
        const uint32_t n = nxt[t->sym[i]]++;
        const int nb = al - (bit_length(n) - 1);
        t->nbits[i] = (uint8_t)nb;
        t->base[i] = (n << nb) - (uint32_t)size;
    }
    return t;
}

// Reads an FSE table description (forward, least significant bit first). Returns the next byte position.
size_t fse_counts(const uint8_t *data, size_t dlen, size_t pos, int max_symbol, int max_al, int &al_out, std::vector<int8_t> &probs) {
    const size_t window = std::min<size_t>(64, dlen - std::min(pos, dlen));
    auto bits_at = [&](size_t bp, int nb) -> uint32_t {  // up to 16 bits
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const size_t b = bp / 8 + (size_t)i;
            v |= (uint32_t)(b < window ? data[pos + b] : 0) << (8 * i);
        }
        return (v >> (bp & 7)) & ((1u << nb) - 1);
    };
    const int al = (int)bits_at(0, 4) + 5;
    if (al > max_al) bad("FSE accuracy too large");
    size_t bp = 4;
    int remaining = (1 << al) + 1;
    int threshold = 1 << al;
    int nb = al + 1;
    probs.clear();
    bool prev0 = false;
    while (remaining > 1 && (int)probs.size() <= max_symbol) {
        if (prev0) {
            for (;;) {
                const int r = (int)bits_at(bp, 2);
                bp += 2;
                for (int i = 0; i < r; ++i) probs.push_back(0);
                if (r != 3) break;
            }
            prev0 = false;
            if ((int)probs.size() > max_symbol) break;
        }
        const int mx = (2 * threshold - 1) - remaining;
        const int v = (int)bits_at(bp, nb);
        int count;
        if ((v & (threshold - 1)) < mx) {
            count = v & (threshold - 1);
            bp += (size_t)(nb - 1);
        } else {
            count = v;
            if (count >= threshold) count -= mx;
            bp += (size_t)nb;
        }
        count -= 1;
        remaining -= count < 0 ? -count : count;
        probs.push_back((int8_t)count);
        prev0 = count == 0;
        while (remaining < threshold) {
            --nb;
            threshold >>= 1;
        }
    }
    if (remaining != 1 || bp > window * 8) bad("bad FSE table description");
    al_out = al;
    return pos + ((bp + 7) >> 3);
}

struct HufTable {
    std::vector<uint16_t> table;  // (symbol << 4) | bits
    int mb = 0;
};

// Reads a Huffman tree description at data[pos..]; returns the next byte position.
size_t huffman_weights(const uint8_t *data, size_t dlen, size_t pos, HufTable &out) {
    if (pos >= dlen) bad("data ends too early");
    const int hb = data[pos++];
    std::vector<uint8_t> weights;
    if (hb >= 128) {
        const int n = hb - 127;
        if (pos + (size_t)((n + 1) >> 1) > dlen) bad("data ends too early");
        for (int i = 0; i < n; ++i) {
            const uint8_t b = data[pos + (size_t)(i >> 1)];
            weights.push_back((i % 2 == 0) ? (uint8_t)(b >> 4) : (uint8_t)(b & 15));
        }
        pos += (size_t)((n + 1) >> 1);
    } else {
        const size_t stop = pos + (size_t)hb;
        if (stop > dlen) bad("data ends too early");
        int al;
        std::vector<int8_t> probs;
        const size_t bpos = fse_counts(data, stop, pos, 255, 6, al, probs);
        FsePtr t = fse_table(al, probs.data(), probs.size());
        Back back(data, (ptrdiff_t)bpos, (ptrdiff_t)stop);
        uint32_t s1 = (uint32_t)back.read(al), s2 = (uint32_t)back.read(al);
        for (;;) {
            weights.push_back(t->sym[s1]);
            s1 = t->base[s1] + (uint32_t)back.read(t->nbits[s1]);
            if (s1 >= t->sym.size()) bad("bad Huffman weights");
            if (back.deficit) {
                weights.push_back(t->sym[s2]);
                break;
            }
            weights.push_back(t->sym[s2]);
            s2 = t->base[s2] + (uint32_t)back.read(t->nbits[s2]);
            if (s2 >= t->sym.size()) bad("bad Huffman weights");
            if (back.deficit) {
                weights.push_back(t->sym[s1]);
                break;
            }
            if (weights.size() > 255) bad("bad Huffman weights");
        }
        pos = stop;
    }
    uint32_t total = 0;
    for (uint8_t w : weights) {
        if (w > 12) bad("bad Huffman weights");
        if (w) total += 1u << (w - 1);
    }
    if (total == 0) bad("bad Huffman weights");
    const int mb = bit_length(total);
    const uint32_t rest = (1u << mb) - total;
    if (rest & (rest - 1)) bad("bad Huffman weights");
    weights.push_back((uint8_t)bit_length(rest));
    if (mb > 11 || weights.size() > 256) bad("bad Huffman tree");
    out.mb = mb;
    out.table.assign((size_t)1 << mb, 0);
    size_t at = 0;
    for (int w = 1; w <= mb; ++w) {
        const size_t cnt = (size_t)1 << (w - 1);
        const int e_nb = mb + 1 - w;
        for (size_t s = 0; s < weights.size(); ++s) {
            if (weights[s] == w) {
                if (at + cnt > out.table.size()) bad("bad Huffman tree");
                for (size_t k = 0; k < cnt; ++k) out.table[at + k] = (uint16_t)((s << 4) | (size_t)e_nb);
                at += cnt;
            }
        }
    }
    return pos;
}

// Decodes n literals from one backward Huffman bitstream.
void huff_stream(const uint8_t *data, ptrdiff_t lo, ptrdiff_t hi, const HufTable &h, uint8_t *out, size_t n) {
    if (hi <= lo || data[hi - 1] == 0) bad("bad literals stream");
    const uint32_t last = data[hi - 1];
    int cnt = bit_length(last) - 1;
    uint64_t acc = last & ((1u << cnt) - 1);
    ptrdiff_t idx = hi - 2;
    const int mb = h.mb;
    const uint64_t mask = ((uint64_t)1 << mb) - 1;
    int shortfall = 0;
    for (size_t i = 0; i < n; ++i) {
        if (cnt < mb) {
            while (cnt < 56 && idx >= lo) {
                acc = (acc << 8) | data[idx--];
                cnt += 8;
            }
            if (cnt < mb) {
                acc <<= mb - cnt;
                shortfall += mb - cnt;
                cnt = mb;
            }
        }
        const uint16_t e = h.table[(acc >> (cnt - mb)) & mask];
        cnt -= e & 15;
        out[i] = (uint8_t)(e >> 4);
    }
    if (cnt != shortfall || idx >= lo) bad("literals stream is damaged");
}

struct State {
    HufTable huff;
    bool have_huff = false;
    FsePtr ll, of, ml;
    uint64_t rep[3] = {1, 4, 8};
};

struct Out {
    uint8_t *p;
    size_t n = 0, cap, frame_start = 0;
    void append(const uint8_t *s, size_t len) {
        if (len > cap - n) throw ChdError("a hunk came out with the wrong size");
        std::memcpy(p + n, s, len);
        n += len;
    }
};

inline uint8_t at(const uint8_t *d, size_t dlen, size_t i) {
    if (i >= dlen) bad("data ends too early");
    return d[i];
}

void block(const uint8_t *d, size_t dlen, size_t pos, size_t end, Out &out, State &st) {
    thread_local std::vector<uint8_t> lit_buf;
    const uint8_t *lits;
    size_t nlits;
    const uint32_t b0 = at(d, dlen, pos);
    const int ltype = (int)(b0 & 3), sf = (int)((b0 >> 2) & 3);
    if (ltype < 2) {
        size_t size;
        if (sf == 0 || sf == 2) {
            size = b0 >> 3;
            pos += 1;
        } else if (sf == 1) {
            size = (b0 >> 4) | ((size_t)at(d, dlen, pos + 1) << 4);
            pos += 2;
        } else {
            size = (b0 >> 4) | ((size_t)at(d, dlen, pos + 1) << 4) | ((size_t)at(d, dlen, pos + 2) << 12);
            pos += 3;
        }
        if (ltype == 0) {
            if (pos + size > dlen) bad("data ends too early");
            lits = d + pos;
            pos += size;
        } else {
            lit_buf.assign(size, at(d, dlen, pos));
            lits = lit_buf.data();
            pos += 1;
        }
        nlits = size;
    } else {
        size_t size, csize, pos0;
        if (sf == 0 || sf == 1) {
            const uint64_t v = b0 | ((uint64_t)at(d, dlen, pos + 1) << 8) | ((uint64_t)at(d, dlen, pos + 2) << 16);
            size = (v >> 4) & 0x3FF;
            csize = (v >> 14) & 0x3FF;
            pos0 = pos + 3;
        } else if (sf == 2) {
            const uint64_t v = b0 | ((uint64_t)at(d, dlen, pos + 1) << 8) | ((uint64_t)at(d, dlen, pos + 2) << 16) | ((uint64_t)at(d, dlen, pos + 3) << 24);
            size = (v >> 4) & 0x3FFF;
            csize = (v >> 18) & 0x3FFF;
            pos0 = pos + 4;
        } else {
            const uint64_t v = b0 | ((uint64_t)at(d, dlen, pos + 1) << 8) | ((uint64_t)at(d, dlen, pos + 2) << 16) | ((uint64_t)at(d, dlen, pos + 3) << 24) |
                               ((uint64_t)at(d, dlen, pos + 4) << 32);
            size = (v >> 4) & 0x3FFFF;
            csize = (v >> 22) & 0x3FFFF;
            pos0 = pos + 5;
        }
        const size_t lend = pos0 + csize;
        if (lend > dlen) bad("data ends too early");
        size_t p = pos0;
        if (ltype == 2) {
            p = huffman_weights(d, lend, p, st.huff);
            st.have_huff = true;
        } else if (!st.have_huff) {
            bad("literals without a Huffman table");
        }
        lit_buf.resize(size);
        if (sf == 0) {
            huff_stream(d, (ptrdiff_t)p, (ptrdiff_t)lend, st.huff, lit_buf.data(), size);
        } else {
            if (p + 6 > lend) bad("data ends too early");
            const size_t s1 = d[p] | ((size_t)d[p + 1] << 8), s2 = d[p + 2] | ((size_t)d[p + 3] << 8), s3 = d[p + 4] | ((size_t)d[p + 5] << 8);
            p += 6;
            const size_t seg = (size + 3) >> 2;
            if (s1 + s2 + s3 > lend - p || size < 3 * seg) bad("bad literals streams");
            const size_t sz[4] = {s1, s2, s3, lend - p - s1 - s2 - s3};
            const size_t cnts[4] = {seg, seg, seg, size - 3 * seg};
            size_t a = p, o = 0;
            for (int i = 0; i < 4; ++i) {
                huff_stream(d, (ptrdiff_t)a, (ptrdiff_t)(a + sz[i]), st.huff, lit_buf.data() + o, cnts[i]);
                a += sz[i];
                o += cnts[i];
            }
        }
        lits = lit_buf.data();
        nlits = size;
        pos = lend;
    }
    if (pos >= end) {
        out.append(lits, nlits);
        return;
    }
    size_t nseq = at(d, dlen, pos++);
    if (nseq == 0) {
        out.append(lits, nlits);
        return;
    }
    if (nseq >= 128) {
        if (nseq == 255) {
            nseq = (size_t)at(d, dlen, pos) + ((size_t)at(d, dlen, pos + 1) << 8) + 0x7F00;
            pos += 2;
        } else {
            nseq = ((nseq - 128) << 8) + at(d, dlen, pos);
            pos += 1;
        }
    }
    const uint32_t modes = at(d, dlen, pos++);
    static const FsePtr def_ll = fse_table(6, kLlDefault, 36), def_of = fse_table(5, kOfDefault, 29), def_ml = fse_table(6, kMlDefault, 53);
    struct Spec {
        FsePtr *slot;
        uint32_t mode;
        const FsePtr *def;
        int maxsym, maxal;
    } specs[3] = {{&st.ll, modes >> 6, &def_ll, 35, 9}, {&st.of, (modes >> 4) & 3, &def_of, 31, 8}, {&st.ml, (modes >> 2) & 3, &def_ml, 52, 9}};
    for (Spec &s : specs) {
        if (s.mode == 0) {
            *s.slot = *s.def;
        } else if (s.mode == 1) {
            auto t = std::make_shared<Fse>();
            t->al = 0;
            t->sym.assign(1, at(d, dlen, pos));
            t->nbits.assign(1, 0);
            t->base.assign(1, 0);
            *s.slot = t;
            pos += 1;
        } else if (s.mode == 2) {
            int al;
            std::vector<int8_t> probs;
            pos = fse_counts(d, end, pos, s.maxsym, s.maxal, al, probs);
            *s.slot = fse_table(al, probs.data(), probs.size());
        } else if (!*s.slot) {
            bad("repeat table without a table");
        }
    }
    const Fse &ll = *st.ll, &of = *st.of, &ml = *st.ml;
    if (pos > end) bad("data ends too early");
    Back back(d, (ptrdiff_t)pos, (ptrdiff_t)end);
    uint32_t lls = (uint32_t)back.read(ll.al), ofs = (uint32_t)back.read(of.al), mls = (uint32_t)back.read(ml.al);
    uint64_t rep0 = st.rep[0], rep1 = st.rep[1], rep2 = st.rep[2];
    size_t lp = 0;
    for (size_t i = 0; i < nseq; ++i) {
        const unsigned oc = of.sym[ofs], mc = ml.sym[mls], lc = ll.sym[lls];
        if (mc >= 53 || lc >= 36 || oc >= 32) bad("bad sequence code");
        const uint64_t ofv = ((uint64_t)1 << oc) + back.read((int)oc);
        const size_t mlen = kMlBase[mc] + (size_t)back.read(kMlBits[mc]);
        const size_t llen = kLlBase[lc] + (size_t)back.read(kLlBits[lc]);
        uint64_t off;
        if (ofv > 3) {
            off = ofv - 3;
            rep2 = rep1;
            rep1 = rep0;
            rep0 = off;
        } else {
            const int idx = (int)ofv - 1 + (llen == 0 ? 1 : 0);
            if (idx == 0) {
                off = rep0;
            } else if (idx == 1) {
                off = rep1;
                rep1 = rep0;
                rep0 = off;
            } else if (idx == 2) {
                off = rep2;
                rep2 = rep1;
                rep1 = rep0;
                rep0 = off;
            } else {
                off = rep0 - 1;
                rep2 = rep1;
                rep1 = rep0;
                rep0 = off;
            }
        }
        if (llen) {
            if (llen > nlits - lp) bad("literals run out");
            out.append(lits + lp, llen);
            lp += llen;
        }
        if (off == 0 || off > out.n - out.frame_start) bad("bad match offset");
        if (mlen > out.cap - out.n) throw ChdError("a hunk came out with the wrong size");
        const uint8_t *src = out.p + out.n - off;
        uint8_t *dst = out.p + out.n;
        if (off >= mlen) std::memcpy(dst, src, mlen);
        else
            for (size_t k = 0; k < mlen; ++k) dst[k] = src[k];
        out.n += mlen;
        if (i + 1 < nseq) {
            lls = ll.base[lls] + (uint32_t)back.read(ll.nbits[lls]);
            mls = ml.base[mls] + (uint32_t)back.read(ml.nbits[mls]);
            ofs = of.base[ofs] + (uint32_t)back.read(of.nbits[ofs]);
            if (lls >= ll.sym.size() || mls >= ml.sym.size() || ofs >= of.sym.size()) bad("bad state");
        }
    }
    if (back.deficit || back.bits_left() != 0) bad("sequences are damaged");
    out.append(lits + lp, nlits - lp);
    st.rep[0] = rep0;
    st.rep[1] = rep1;
    st.rep[2] = rep2;
}

}  // namespace

size_t zstd_decode(const uint8_t *d, size_t n, uint8_t *out_p, size_t cap) {
    Out out{out_p, 0, cap, 0};
    size_t pos = 0;
    auto u32 = [&](size_t i) -> uint32_t { return (uint32_t)at(d, n, i) | ((uint32_t)at(d, n, i + 1) << 8) | ((uint32_t)at(d, n, i + 2) << 16) | ((uint32_t)at(d, n, i + 3) << 24); };
    while (pos < n) {
        const uint32_t magic = u32(pos);
        if (magic >= 0x184D2A50u && magic <= 0x184D2A5Fu) {
            pos += 8 + (size_t)u32(pos + 4);
            continue;
        }
        if (magic != 0xFD2FB528u) bad("frame expected");
        pos += 4;
        const uint32_t fhd = at(d, n, pos++);
        const bool single = (fhd >> 5) & 1, checksum = (fhd >> 2) & 1;
        const int did = (int)(fhd & 3), fcs = (int)(fhd >> 6);
        if (!single) pos += 1;
        static const int dsz[4] = {0, 1, 2, 4};
        pos += (size_t)dsz[did];
        static const int fsz[4] = {1, 2, 4, 8};
        pos += (size_t)(fcs == 0 ? (single ? 1 : 0) : fsz[fcs]);
        State st;
        out.frame_start = out.n;
        for (;;) {
            const uint32_t h = at(d, n, pos) | ((uint32_t)at(d, n, pos + 1) << 8) | ((uint32_t)at(d, n, pos + 2) << 16);
            pos += 3;
            const bool last = h & 1;
            const int btype = (int)((h >> 1) & 3);
            const size_t bsize = h >> 3;
            if (btype == 0) {
                if (pos + bsize > n) bad("data ends too early");
                out.append(d + pos, bsize);
                pos += bsize;
            } else if (btype == 1) {
                const uint8_t b = at(d, n, pos);
                if (bsize > out.cap - out.n) throw ChdError("a hunk came out with the wrong size");
                std::memset(out.p + out.n, b, bsize);
                out.n += bsize;
                pos += 1;
            } else if (btype == 2) {
                if (pos + bsize > n) bad("data ends too early");
                block(d, n, pos, pos + bsize, out, st);
                pos += bsize;
            } else {
                bad("bad block");
            }
            if (last) break;
        }
        if (checksum) pos += 4;
    }
    return out.n;
}
