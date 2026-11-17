// FLAC frames (RFC 9639) without the file header, as the "flac" and "cdfl" codecs of CHD store them. A port of flac_decode()
// of scripts/chd.py; 16-bit stereo, which is what chdman always writes. CRCs are not checked (the CHD carries its own checksum).
#include <algorithm>
#include <cstring>

#include "codecs.hpp"

namespace {

struct Rd {
    const uint8_t *p;
    size_t total;  // bits
    size_t pos = 0;
    Rd(const uint8_t *p_, size_t n) : p(p_), total(n * 8) {}
    [[noreturn]] static void fail() { throw ChdError("FLAC data ends too early"); }
    uint32_t read(int n) {  // n <= 32
        if (n == 0) return 0;
        if (pos + (size_t)n > total) fail();
        const size_t byte = pos >> 3;
        const int off = (int)(pos & 7);
        const int need = (off + n + 7) >> 3;
        uint64_t v = 0;
        for (int i = 0; i < need; ++i) v = (v << 8) | p[byte + (size_t)i];
        v >>= (need * 8 - off - n);
        pos += (size_t)n;
        return (uint32_t)(v & ((1ull << n) - 1));
    }
    int32_t read_signed(int n) {
        const uint32_t v = read(n);
        return (v >> (n - 1)) ? (int32_t)((int64_t)v - ((int64_t)1 << n)) : (int32_t)v;
    }
    uint32_t unary() {
        uint32_t q = 0;
        for (;;) {
            if (pos >= total) fail();
            const int off = (int)(pos & 7);
            const uint32_t b = p[pos >> 3] & (0xFFu >> off);
            if (b) {
                const int lz = __builtin_clz(b) - 24;
                q += (uint32_t)(lz - off);
                pos += (size_t)(lz - off) + 1;
                return q;
            }
            q += (uint32_t)(8 - off);
            pos += (size_t)(8 - off);
        }
    }
};

const int kBlock[16] = {0, 192, 576, 1152, 2304, 4608, 0, 0, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};

void residual(Rd &r, int *out, int blocksize, int order) {
    const int method = (int)r.read(2);
    if (method > 1) throw ChdError("bad FLAC residual coding");
    const int pbits = method == 0 ? 4 : 5;
    const uint32_t esc = (1u << pbits) - 1;
    const int porder = (int)r.read(4);
    const int parts = 1 << porder;
    int at = 0;
    for (int p = 0; p < parts; ++p) {
        const int n = (blocksize >> porder) - (p == 0 ? order : 0);
        if (n < 0) throw ChdError("bad FLAC residual coding");
        const uint32_t k = r.read(pbits);
        if (k == esc) {
            const int raw = (int)r.read(5);
            for (int i = 0; i < n; ++i) out[at++] = raw ? r.read_signed(raw) : 0;
        } else {
            for (int i = 0; i < n; ++i) {
                const uint32_t q = r.unary();
                const uint32_t v = (q << k) | (k ? r.read((int)k) : 0);
                out[at++] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
            }
        }
    }
}

void subframe(Rd &r, int32_t *s, int blocksize, int bps, std::vector<int> &res) {
    if (r.read(1)) throw ChdError("bad FLAC subframe");
    const int kind = (int)r.read(6);
    int wasted = 0;
    if (r.read(1)) {
        wasted = 1;
        while (!r.read(1)) ++wasted;
    }
    const int b = bps - wasted;
    if (b < 1 || b > 32) throw ChdError("bad FLAC subframe");
    if (kind == 0) {
        const int32_t v = r.read_signed(b);
        for (int i = 0; i < blocksize; ++i) s[i] = v;
    } else if (kind == 1) {
        for (int i = 0; i < blocksize; ++i) s[i] = r.read_signed(b);
    } else if (kind >= 8 && kind <= 12) {
        const int order = kind - 8;
        if (order > blocksize) throw ChdError("bad FLAC subframe");
        for (int i = 0; i < order; ++i) s[i] = r.read_signed(b);
        res.resize((size_t)blocksize);
        residual(r, res.data(), blocksize, order);
        for (int i = order; i < blocksize; ++i) {
            const int64_t x = res[(size_t)(i - order)];
            int64_t pred = 0;
            if (order == 1) pred = s[i - 1];
            else if (order == 2) pred = 2 * (int64_t)s[i - 1] - s[i - 2];
            else if (order == 3) pred = 3 * (int64_t)s[i - 1] - 3 * (int64_t)s[i - 2] + s[i - 3];
            else if (order == 4) pred = 4 * (int64_t)s[i - 1] - 6 * (int64_t)s[i - 2] + 4 * (int64_t)s[i - 3] - s[i - 4];
            s[i] = (int32_t)(pred + x);
        }
    } else if (kind >= 32) {
        const int order = kind - 31;
        if (order > blocksize) throw ChdError("bad FLAC subframe");
        for (int i = 0; i < order; ++i) s[i] = r.read_signed(b);
        const int prec = (int)r.read(4) + 1;
        if (prec == 16) throw ChdError("bad FLAC predictor precision");
        const int shift = r.read_signed(5);
        int32_t coefs[32];
        for (int i = 0; i < order; ++i) coefs[i] = r.read_signed(prec);
        res.resize((size_t)blocksize);
        residual(r, res.data(), blocksize, order);
        for (int i = order; i < blocksize; ++i) {
            int64_t acc = 0;
            for (int j = 0; j < order; ++j) acc += (int64_t)coefs[j] * s[i - 1 - j];
            const int64_t v = shift >= 0 ? (acc >> shift) : (int64_t)((uint64_t)acc << -shift);
            s[i] = (int32_t)(v + res[(size_t)(i - order)]);
        }
    } else {
        throw ChdError("bad FLAC subframe type");
    }
    if (wasted)
        for (int i = 0; i < blocksize; ++i) s[i] = (int32_t)((uint32_t)s[i] << wasted);
}

}  // namespace

size_t flac_decode(const uint8_t *in, size_t n, size_t samples, bool big_endian, uint8_t *out, size_t cap) {
    if (samples * 4 > cap) throw ChdError("a hunk came out with the wrong size");
    Rd r(in, n);
    thread_local std::vector<int32_t> ch[2];
    thread_local std::vector<int> res;
    size_t done = 0;
    while (done < samples) {
        r.pos = (r.pos + 7) & ~(size_t)7;
        if (r.read(14) != 0x3FFE) throw ChdError("FLAC frame sync not found");
        r.read(1);
        r.read(1);
        const int bs_code = (int)r.read(4);
        const int sr_code = (int)r.read(4);
        const int ch_code = (int)r.read(4);
        const int ss_code = (int)r.read(3);
        r.read(1);
        const uint32_t first = r.read(8);
        const int extra = first < 0x80 ? 0 : first < 0xE0 ? 1 : first < 0xF0 ? 2 : first < 0xF8 ? 3 : first < 0xFC ? 4 : first < 0xFE ? 5 : 6;
        for (int i = 0; i < extra; ++i) r.read(8);
        int blocksize;
        if (bs_code == 6) blocksize = (int)r.read(8) + 1;
        else if (bs_code == 7) blocksize = (int)r.read(16) + 1;
        else if (kBlock[bs_code]) blocksize = kBlock[bs_code];
        else throw ChdError("FLAC block size code not supported");
        if (sr_code == 12) r.read(8);
        else if (sr_code == 13 || sr_code == 14) r.read(16);
        r.read(8);  // CRC-8 of the header
        int bps;
        switch (ss_code) {
            case 0: case 4: bps = 16; break;
            case 1: bps = 8; break;
            case 2: bps = 12; break;
            case 5: bps = 20; break;
            case 6: bps = 24; break;
            default: throw ChdError("FLAC sample size not supported");
        }
        int nch, mode;
        if (ch_code < 8) {
            nch = ch_code + 1;
            mode = 0;
        } else if (ch_code <= 10) {
            nch = 2;
            mode = ch_code - 7;  // 1 left/side, 2 right/side, 3 mid/side
        } else {
            throw ChdError("FLAC channel layout not supported");
        }
        if (nch != 2) throw ChdError("FLAC stream has the wrong number of channels");
        for (int c = 0; c < 2; ++c) {
            int b = bps;
            if ((mode == 1 && c == 1) || (mode == 2 && c == 0) || (mode == 3 && c == 1)) ++b;
            ch[c].resize((size_t)blocksize);
            subframe(r, ch[c].data(), blocksize, b, res);
        }
        int32_t *a = ch[0].data(), *bch = ch[1].data();
        if (mode == 1) {
            for (int i = 0; i < blocksize; ++i) bch[i] = a[i] - bch[i];
        } else if (mode == 2) {
            for (int i = 0; i < blocksize; ++i) a[i] = a[i] + bch[i];
        } else if (mode == 3) {
            for (int i = 0; i < blocksize; ++i) {
                const int32_t mm = (int32_t)(((uint32_t)a[i] << 1) | ((uint32_t)bch[i] & 1u));
                const int32_t ss = bch[i];
                a[i] = (mm + ss) >> 1;
                bch[i] = (mm - ss) >> 1;
            }
        }
        r.pos = (r.pos + 7) & ~(size_t)7;
        r.read(16);  // CRC-16
        const size_t take = std::min<size_t>((size_t)blocksize, samples - done);
        for (size_t i = 0; i < take; ++i) {
            for (int c = 0; c < 2; ++c) {
                const int32_t v = ch[c][i];
                if (v < -32768 || v > 32767) throw ChdError("FLAC sample out of range");
                const uint16_t u = (uint16_t)v;
                uint8_t *o = out + (done + i) * 4 + (size_t)c * 2;
                if (big_endian) {
                    o[0] = (uint8_t)(u >> 8);
                    o[1] = (uint8_t)u;
                } else {
                    o[0] = (uint8_t)u;
                    o[1] = (uint8_t)(u >> 8);
                }
            }
        }
        done += take;
    }
    return samples * 4;
}
