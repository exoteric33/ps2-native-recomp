// chdunpack: turns a .chd disc image (CHD version 5, MAME's compressed hunks) into a plain disc image, several threads at a time.
//
//     chdunpack [--jobs N] in.chd out.iso
//
// A native port of scripts/chd.py (which stays the reference and the fallback): the same file rules, the same refusals, the same
// result byte for byte. A DVD comes out as it is and is checked against the SHA-1 the file carries; a CD gives its first data track as
// 2048-byte sectors. The image is written as out.iso.part and renamed when it is complete, so a stopped unpack never leaves a half image
// that looks finished. Progress lines ("Unpacking the .chd disc image: 35 %") are the ones scripts/chd.py prints, which the app reads;
// errors come as "chd.py: <sentence>" on stderr for the same reason (the app turns that line into plain words).
//
// The rules come from the CHD v5 format as MAME's chd.h and codecs define it; libchdr (BSD-3-Clause) was read to learn them, nothing
// is copied. The decoders are in codecs.cpp, flac.cpp and zstd.cpp.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "codecs.hpp"

namespace fs = std::filesystem;

namespace {

constexpr size_t SECTOR = 2048, CD_RAW = 2352, CD_SUBCODE = 96, CD_FRAME = CD_RAW + CD_SUBCODE, CD_PAD = 4;
enum : uint8_t { T_NONE = 4, T_SELF = 5, T_PARENT = 6 };

fs::path to_path(const std::string &u8) { return fs::path(std::u8string(u8.begin(), u8.end())); }

FILE *open_file(const fs::path &p, const char *mode) {
#ifdef _WIN32
    return _wfopen(p.c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

int seek64(FILE *f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

struct File {
    FILE *f = nullptr;
    explicit File(const fs::path &p) {
        f = open_file(p, "rb");
        if (!f) throw ChdError("the file could not be opened");
    }
    ~File() {
        if (f) std::fclose(f);
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    size_t read_at(uint64_t off, void *dst, size_t n) {
        if (seek64(f, off) != 0) return 0;
        return std::fread(dst, 1, n, f);
    }
};

struct Track {
    uint64_t number, frames, pregap, start;
    std::string kind, pgtype;
    bool is_data() const { return kind != "AUDIO"; }
};

uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

bool all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

struct Chd {
    fs::path path;
    uint8_t codecs[4][4];
    uint64_t logical, map_offset, meta_offset, hunks;
    uint32_t hunk_bytes, unit_bytes;
    uint8_t raw_sha1[20];
    bool compressed;
    std::vector<Track> tracks;
    bool cd = false;
    size_t frames_per_hunk = 0;
    std::vector<uint8_t> types;
    std::vector<uint32_t> lengths;
    std::vector<uint64_t> offsets;

    static bool known_codec(const uint8_t *c) {
        static const char *names[] = {"zlib", "lzma", "huff", "flac", "zstd", "cdzl", "cdlz", "cdfl", "cdzs"};
        for (const char *n : names)
            if (std::memcmp(c, n, 4) == 0) return true;
        return false;
    }
    bool codec_is(int t, const char *name) const { return std::memcmp(codecs[t], name, 4) == 0; }

    explicit Chd(const fs::path &p) : path(p) {
        File file(p);
        uint8_t head[124];
        const size_t got = file.read_at(0, head, 124);
        if (got < 16 || std::memcmp(head, "MComprHD", 8) != 0) throw ChdError("this is not a CHD file");
        const uint64_t length = be(head + 8, 4), version = be(head + 12, 4);
        if (version != 5)
            throw ChdError("this CHD file is version " + std::to_string(version) + "; only version 5 is supported (convert it with chdman, or use an .iso)");
        if (length != 124 || got < 124) throw ChdError("the CHD header is damaged");
        for (int i = 0; i < 4; ++i) std::memcpy(codecs[i], head + 16 + 4 * i, 4);
        logical = be(head + 32, 8);
        map_offset = be(head + 40, 8);
        meta_offset = be(head + 48, 8);
        hunk_bytes = (uint32_t)be(head + 56, 4);
        unit_bytes = (uint32_t)be(head + 60, 4);
        std::memcpy(raw_sha1, head + 64, 20);
        if (!all_zero(head + 104, 20)) throw ChdError("this CHD file needs a parent CHD file (not supported)");
        if (hunk_bytes == 0 || unit_bytes == 0 || hunk_bytes % unit_bytes) throw ChdError("the CHD header is damaged");
        hunks = (logical + hunk_bytes - 1) / hunk_bytes;
        compressed = !all_zero(codecs[0], 4);
        for (int i = 0; i < 4; ++i) {
            if (!all_zero(codecs[i], 4) && !known_codec(codecs[i])) {
                std::string name(reinterpret_cast<const char *>(codecs[i]), 4);
                for (char &ch : name)
                    if (ch < 32 || ch > 126) ch = '?';
                throw ChdError("this CHD file uses the codec \"" + name + "\", which is not supported");
            }
        }
        read_tracks(file);
        read_map(file);
        cd = !tracks.empty();
        if (cd) {
            if (unit_bytes != CD_FRAME) throw ChdError("the CD layout of this CHD file is not supported");
            frames_per_hunk = hunk_bytes / CD_FRAME;
        }
    }

    void read_tracks(File &file) {
        uint64_t off = meta_offset, start = 0;
        for (int seen = 0; off && seen < 100000; ++seen) {
            uint8_t h[16];
            if (file.read_at(off, h, 16) < 16) break;
            const uint64_t len = be(h + 5, 3), nxt = be(h + 8, 8);
            std::vector<uint8_t> data(len);
            if (len && file.read_at(off + 16, data.data(), len) != len) data.resize(0);  // short data: the fields below fail
            if (std::memcmp(h, "CHT2", 4) == 0 || std::memcmp(h, "CHTR", 4) == 0) {
                std::string text(data.begin(), data.end());
                while (!text.empty() && (text.back() == '\0' || text.back() == '.' || text.back() == ' ' || text.back() == '\n')) text.pop_back();
                std::map<std::string, std::string> fields;
                size_t i = 0;
                while (i < text.size()) {
                    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
                    size_t j = i;
                    while (j < text.size() && !std::isspace((unsigned char)text[j])) ++j;
                    const std::string word = text.substr(i, j - i);
                    const size_t colon = word.find(':');
                    if (colon != std::string::npos) fields[word.substr(0, colon)] = word.substr(colon + 1);
                    i = j;
                }
                auto num = [&](const char *key, bool optional) -> uint64_t {
                    auto it = fields.find(key);
                    if (it == fields.end() || it->second.empty()) {
                        if (optional) return 0;
                        throw ChdError("the track list of this CHD file is damaged");
                    }
                    char *endp = nullptr;
                    const unsigned long long v = std::strtoull(it->second.c_str(), &endp, 10);
                    if (endp == it->second.c_str() || *endp) throw ChdError("the track list of this CHD file is damaged");
                    return v;
                };
                Track t;
                t.frames = num("FRAMES", false);
                t.number = num("TRACK", false);
                if (!fields.count("TYPE")) throw ChdError("the track list of this CHD file is damaged");
                t.kind = fields["TYPE"];
                t.pregap = num("PREGAP", true);
                t.pgtype = fields.count("PGTYPE") ? fields["PGTYPE"] : "";
                t.start = start;
                tracks.push_back(t);
                start += (t.frames + CD_PAD - 1) / CD_PAD * CD_PAD;
            } else if (std::memcmp(h, "CHCD", 4) == 0 || std::memcmp(h, "CHGT", 4) == 0 || std::memcmp(h, "CHGD", 4) == 0) {
                throw ChdError("this CHD file has a track list of an old or GD-ROM kind that is not supported");
            }
            off = nxt;
        }
    }

    void read_map(File &file) {
        const size_t n = (size_t)hunks;
        types.assign(n, 0);
        lengths.assign(n, 0);
        offsets.assign(n, 0);
        if (!compressed) {
            std::vector<uint8_t> raw(4 * n);
            if (n && file.read_at(map_offset, raw.data(), raw.size()) < raw.size()) throw ChdError("the hunk map is cut off");
            for (size_t i = 0; i < n; ++i) {
                types[i] = T_NONE;
                lengths[i] = hunk_bytes;
                offsets[i] = be(raw.data() + 4 * i, 4) * hunk_bytes;
            }
            return;
        }
        uint8_t mh[16];
        if (file.read_at(map_offset, mh, 16) < 16) throw ChdError("the hunk map is cut off");
        const uint64_t mapbytes = be(mh, 4), first = be(mh + 4, 6);
        const int length_bits = mh[12], self_bits = mh[13], parent_bits = mh[14];
        (void)parent_bits;
        std::vector<uint8_t> blob(mapbytes);
        if (mapbytes && file.read_at(map_offset + 16, blob.data(), mapbytes) < mapbytes) throw ChdError("the hunk map is cut off");
        BitsMsb bits(blob.data(), blob.size());
        HuffTree tree(16, 8);
        tree.import_rle(bits);
        uint8_t lastcomp = 0;
        size_t rep = 0;
        for (size_t h = 0; h < n; ++h) {
            if (rep) {
                types[h] = lastcomp;
                --rep;
                continue;
            }
            const int v = tree.decode(bits);
            if (v == 7) {
                types[h] = lastcomp;
                rep = 2 + (size_t)tree.decode(bits);
            } else if (v == 8) {
                types[h] = lastcomp;
                rep = 2 + 16 + ((size_t)tree.decode(bits) << 4);
                rep += (size_t)tree.decode(bits);
            } else {
                types[h] = lastcomp = (uint8_t)v;
            }
        }
        uint64_t cur = first, last_self = 0;
        for (size_t h = 0; h < n; ++h) {
            const uint8_t t = types[h];
            if (t <= 3) {
                const uint32_t ln = bits.read(length_bits);
                lengths[h] = ln;
                offsets[h] = cur;
                cur += ln;
                bits.pos += 16;
            } else if (t == T_NONE) {
                lengths[h] = hunk_bytes;
                offsets[h] = cur;
                cur += hunk_bytes;
                bits.pos += 16;
            } else if (t == T_SELF) {
                last_self = bits.read(self_bits);
                offsets[h] = last_self;
            } else if (t == T_PARENT) {
                throw ChdError("this CHD file needs a parent CHD file (not supported)");
            } else if (t == 9 || t == 10) {
                if (t == 10) ++last_self;
                types[h] = T_SELF;
                offsets[h] = last_self;
            } else {
                throw ChdError("this CHD file needs a parent CHD file (not supported)");
            }
        }
        if (bits.overflow()) throw ChdError("the hunk map is damaged");
    }

    size_t out_hunk_bytes() const { return cd ? frames_per_hunk * CD_RAW : hunk_bytes; }
};

// What one worker thread owns: its own file handle and scratch memory.
class Hunker {
public:
    explicit Hunker(const Chd &c) : c_(c), file_(c.path) {}

    // Decodes hunks first..last-1 into dst (out_hunk_bytes each).
    void span(size_t first, size_t last, uint8_t *dst) {
        load_region(first, last);
        const size_t hs = c_.out_hunk_bytes();
        for (size_t h = first; h < last; ++h) {
            try {
                decode(h, dst + (h - first) * hs, dst, first, 0);
            } catch (const ChdError &e) {
                if (!std::strcmp(e.what(), "the CHD file is cut off")) throw;
                throw ChdError("hunk " + std::to_string(h) + " is damaged (" + e.what() + ")");
            }
        }
    }

private:
    const Chd &c_;
    File file_;
    std::vector<uint8_t> region_, comp_, tmp_;
    uint64_t reg_lo_ = 0, reg_hi_ = 0;

    void load_region(size_t first, size_t last) {
        uint64_t lo = UINT64_MAX, hi = 0;
        for (size_t h = first; h < last; ++h) {
            const uint8_t t = c_.types[h];
            if (t > T_NONE || (t == T_NONE && c_.offsets[h] == 0)) continue;
            lo = std::min(lo, c_.offsets[h]);
            hi = std::max(hi, c_.offsets[h] + c_.lengths[h]);
        }
        reg_lo_ = reg_hi_ = 0;
        if (lo == UINT64_MAX || hi - lo > (64u << 20)) return;
        region_.resize(hi - lo);
        const size_t got = file_.read_at(lo, region_.data(), region_.size());
        reg_lo_ = lo;
        reg_hi_ = lo + got;
    }

    const uint8_t *fetch(uint64_t off, size_t len, std::vector<uint8_t> &scratch) {
        if (off >= reg_lo_ && off + len <= reg_hi_) return region_.data() + (off - reg_lo_);
        scratch.resize(len);
        if (len && file_.read_at(off, scratch.data(), len) < len) throw ChdError("the CHD file is cut off");
        return scratch.data();
    }

    size_t codec_decode(const uint8_t *tag, const uint8_t *in, size_t n, uint8_t *out, size_t size) {
        if (!std::memcmp(tag, "zlib", 4)) return inflate_raw(in, n, out, size);
        if (!std::memcmp(tag, "lzma", 4)) return lzma_raw(in, n, out, size);
        if (!std::memcmp(tag, "zstd", 4)) return zstd_decode(in, n, out, size);
        if (!std::memcmp(tag, "huff", 4)) {
            BitsMsb bits(in, n);
            HuffTree hf(256, 16);
            hf.import_huffman(bits);
            hf.decode_bytes(in, n, bits.pos, out, size);
            return size;
        }
        if (!std::memcmp(tag, "flac", 4)) {
            if (n == 0 || (in[0] != 'L' && in[0] != 'B')) throw ChdError("bad FLAC hunk");
            return flac_decode(in + 1, n - 1, size / 4, in[0] == 'B', out, size);
        }
        throw ChdError("codec not supported");
    }

    // A CD hunk as frames * 2352 sector bytes (the sub-channel part is left out).
    void cd_decode(const uint8_t *tag, const uint8_t *raw, size_t n, uint8_t *out) {
        const size_t frames = c_.frames_per_hunk, size = frames * CD_RAW;
        if (!std::memcmp(tag, "cdfl", 4)) {
            // no flag bytes: FLAC frames of the whole sector data (sync included, big-endian samples), then the sub-channel
            flac_decode(raw, n, size / 4, true, out, size);
            return;
        }
        const size_t eccb = (frames + 7) / 8, cl = c_.hunk_bytes < 65536 ? 2 : 3;
        if (n < eccb + cl) throw ChdError("a CD hunk is cut off");
        const size_t baselen = (size_t)be(raw + eccb, (int)cl);
        const size_t avail = n - eccb - cl;
        const uint8_t *base = raw + eccb + cl;
        const size_t blen = std::min(baselen, avail);
        const char *name = !std::memcmp(tag, "cdzl", 4) ? "zlib" : !std::memcmp(tag, "cdlz", 4) ? "lzma" : "zstd";
        const size_t got = codec_decode(reinterpret_cast<const uint8_t *>(name), base, blen, out, size);
        if (got != size) throw ChdError("a CD hunk came out with the wrong size");
        static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
        for (size_t f = 0; f < frames; ++f)  // the sync pattern of data frames is not stored, only a flag
            if (raw[f >> 3] & (1u << (f & 7))) std::memcpy(out + f * CD_RAW, sync, 12);
    }

    void cut_frames(const uint8_t *src, uint8_t *out) {
        for (size_t f = 0; f < c_.frames_per_hunk; ++f) std::memcpy(out + f * CD_RAW, src + f * CD_FRAME, CD_RAW);
    }

    // out gets out_hunk_bytes of hunk h. span_base/span_first: the hunks of this span already decoded (a shared hunk usually is one).
    void decode(size_t h, uint8_t *out, const uint8_t *span_base, size_t span_first, int depth) {
        const size_t hs = c_.out_hunk_bytes();
        uint8_t t = c_.types[h];
        if (t == T_SELF) {
            size_t target = (size_t)c_.offsets[h];
            for (int hops = 0;; ++hops) {
                if (target >= c_.hunks || hops > 64) throw ChdError("a shared hunk points nowhere");
                if (c_.types[target] != T_SELF) break;
                target = (size_t)c_.offsets[target];
            }
            if (target >= span_first && target < h) std::memcpy(out, span_base + (target - span_first) * hs, hs);
            else if (depth > 2) throw ChdError("shared hunks nest too deep");
            else decode(target, out, span_base, span_first, depth + 1);
            return;
        }
        if (t == T_NONE) {
            if (c_.offsets[h] == 0) {
                std::memset(out, 0, hs);
                return;
            }
            const uint8_t *src = fetch(c_.offsets[h], c_.hunk_bytes, comp_);
            if (c_.cd) cut_frames(src, out);
            else std::memcpy(out, src, hs);
            return;
        }
        if (t > 3) throw ChdError("this CHD file needs a parent CHD file (not supported)");
        const uint8_t *tag = c_.codecs[t];
        const size_t len = c_.lengths[h];
        const uint8_t *raw = fetch(c_.offsets[h], len, comp_);
        if (!std::memcmp(tag, "cdzl", 4) || !std::memcmp(tag, "cdlz", 4) || !std::memcmp(tag, "cdfl", 4) || !std::memcmp(tag, "cdzs", 4)) {
            if (!c_.cd) throw ChdError("codec not supported");
            cd_decode(tag, raw, len, out);
            return;
        }
        if (!c_.cd) {
            const size_t got = codec_decode(tag, raw, len, out, c_.hunk_bytes);
            if (got != c_.hunk_bytes) throw ChdError("a hunk came out with the wrong size");
        } else {  // a CD stored with a plain codec: frames of 2448 bytes
            tmp_.resize(c_.hunk_bytes);
            const size_t got = codec_decode(tag, raw, len, tmp_.data(), c_.hunk_bytes);
            if (got != c_.hunk_bytes) throw ChdError("a hunk came out with the wrong size");
            cut_frames(tmp_.data(), out);
        }
    }
};

// The 2048 bytes of a data sector, cut out of the stored form.
const uint8_t *user_data(const uint8_t *sector, const std::string &kind) {
    if (kind == "MODE1" || kind == "MODE2_FORM1") return sector;
    if (kind == "MODE2" || kind == "MODE2_FORM_MIX") return sector + 8;
    return sector[15] == 1 ? sector + 16 : sector + 24;
}

int job_count() {
    for (const char *name : {"PS2_CHD_JOBS", "PS2_JOBS"}) {
        if (const char *v = std::getenv(name)) {
            char *endp = nullptr;
            const long n = std::strtol(v, &endp, 10);
            if (endp != v && !*endp && n >= 1) return (int)std::min<long>(n, 64);
        }
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return (int)std::max(1u, std::min(hw ? hw : 1u, 8u));
}

struct Progress {
    int last = -1;
    void report(uint64_t done, uint64_t total) {
        const int pct = total ? (int)(done * 100 / total) : 100;
        if (pct >= last + 5 || (pct == 100 && last != 100)) {
            last = pct;
            std::printf("Unpacking the .chd disc image: %d %%\n", pct);
            std::fflush(stdout);
        }
    }
};

uint64_t convert(const fs::path &in_path, const fs::path &out_path, int jobs) {
    Chd chd(in_path);
    fs::path part = out_path;
    part += ".part";
    // what is to be written, and which hunks it needs
    size_t first_h = 0, last_h = (size_t)chd.hunks;
    uint64_t total = chd.logical;
    const Track *tr = nullptr;
    if (chd.cd) {
        for (const Track &t : chd.tracks)
            if (t.is_data()) {
                tr = &t;
                break;
            }
        if (!tr) throw ChdError("this CHD file holds an audio CD, no game disc");
        if (tr->pregap && !tr->pgtype.empty() && tr->pgtype[0] == 'V') throw ChdError("this CD image keeps its pregap in the file, which is not supported");
        const size_t fph = chd.frames_per_hunk;
        first_h = (size_t)(tr->start / fph);
        last_h = (size_t)((tr->start + tr->frames - 1) / fph + 1);
        total = tr->frames * SECTOR;
        if (tr->frames == 0 || last_h > chd.hunks) throw ChdError("the track list of this CHD file is damaged");
    }
    const size_t hs = chd.out_hunk_bytes();
    const size_t per_span = std::max<size_t>(1, ((size_t)2 << 20) / hs);
    const size_t nspans = last_h > first_h ? (last_h - first_h + per_span - 1) / per_span : 0;
    jobs = (int)std::max<size_t>(1, std::min<size_t>((size_t)jobs, std::max<size_t>(nspans, 1)));

    FILE *out = open_file(part, "wb");
    if (!out) throw ChdError("the disc image could not be created (" + std::string(std::strerror(errno)) + ")");
    auto cleanup = [&](bool keep_open) {
        if (!keep_open && out) {
            std::fclose(out);
            out = nullptr;
        }
        std::error_code ec;
        fs::remove(part, ec);
    };

    std::mutex mu;
    std::condition_variable cv_done, cv_space;
    std::map<size_t, std::vector<uint8_t>> done;
    size_t next_claim = 0, next_write = 0;
    bool abort = false;
    std::string error;
    const size_t window = (size_t)jobs * 3 + 1;

    auto worker = [&]() {
        try {
            Hunker hunker(chd);
            std::vector<uint8_t> raw;
            for (;;) {
                size_t i;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv_space.wait(lk, [&] { return abort || next_claim >= nspans || next_claim < next_write + window; });
                    if (abort || next_claim >= nspans) return;
                    i = next_claim++;
                }
                const size_t a = first_h + i * per_span, b = std::min(last_h, a + per_span);
                raw.resize((b - a) * hs);
                hunker.span(a, b, raw.data());
                std::vector<uint8_t> res;
                if (!chd.cd) {
                    const uint64_t end = std::min<uint64_t>((uint64_t)b * chd.hunk_bytes, chd.logical);
                    raw.resize((size_t)(end - (uint64_t)a * chd.hunk_bytes));
                    res.swap(raw);
                } else {
                    const size_t fph = chd.frames_per_hunk;
                    res.reserve((b - a) * fph * SECTOR);
                    for (size_t h = a; h < b; ++h)
                        for (size_t k = 0; k < fph; ++k) {
                            const uint64_t frame = (uint64_t)h * fph + k;
                            if (frame >= tr->start && frame < tr->start + tr->frames) {
                                const uint8_t *u = user_data(raw.data() + (h - a) * hs + k * CD_RAW, tr->kind);
                                res.insert(res.end(), u, u + SECTOR);
                            }
                        }
                }
                std::lock_guard<std::mutex> lk(mu);
                done.emplace(i, std::move(res));
                cv_done.notify_all();
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lk(mu);
            if (!abort) error = e.what();
            abort = true;
            cv_done.notify_all();
            cv_space.notify_all();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < jobs; ++i) threads.emplace_back(worker);

    Sha1 sha;
    Progress progress;
    uint64_t written = 0;
    std::string write_error;
    for (size_t i = 0; i < nspans && write_error.empty(); ++i) {
        std::vector<uint8_t> buf;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv_done.wait(lk, [&] { return abort || done.count(i); });
            if (abort) break;
            buf = std::move(done[i]);
            done.erase(i);
            next_write = i + 1;
            cv_space.notify_all();
        }
        std::future<void> hashing;
        if (!chd.cd) hashing = std::async(std::launch::async, [&] { sha.update(buf.data(), buf.size()); });
        if (!buf.empty() && std::fwrite(buf.data(), 1, buf.size(), out) != buf.size()) write_error = std::strerror(errno);
        if (hashing.valid()) hashing.get();
        written += buf.size();
        progress.report(written, total);
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!write_error.empty()) abort = true;
        cv_space.notify_all();
    }
    for (std::thread &t : threads) t.join();

    if (!write_error.empty()) {
        cleanup(false);
        throw ChdError("the disc image could not be written (" + write_error + ")");
    }
    if (abort) {
        cleanup(false);
        throw ChdError(error.empty() ? "unpacking failed" : error);
    }
    if (std::fclose(out) != 0) {
        out = nullptr;
        cleanup(true);
        throw ChdError(std::string("the disc image could not be written (") + std::strerror(errno) + ")");
    }
    out = nullptr;
    if (!chd.cd) {
        uint8_t digest[20];
        sha.finish(digest);
        if (std::memcmp(digest, chd.raw_sha1, 20) != 0 && !all_zero(chd.raw_sha1, 20)) {
            cleanup(true);
            throw ChdError("the data of this CHD file does not match its checksum (damaged file)");
        }
    }
    std::error_code ec;
    fs::rename(part, out_path, ec);
    if (ec) {
        fs::remove(out_path, ec);
        fs::rename(part, out_path, ec);
        if (ec) {
            cleanup(true);
            throw ChdError("the disc image could not be renamed (" + ec.message() + ")");
        }
    }
    return written;
}

}  // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // argv is in the ANSI code page and loses every character outside it: take the UTF-16 command line and use it as UTF-8
    int wide_argc = 0;
    if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc)) {
        for (int i = 0; i < wide_argc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s((size_t)std::max(n - 1, 0), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), n, nullptr, nullptr);
            args.push_back(s);
        }
        LocalFree(wide);
    }
#endif
    if (args.empty())
        for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
    int jobs = job_count();
    std::vector<std::string> files;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--jobs" && i + 1 < args.size()) jobs = std::max(1, std::atoi(args[++i].c_str()));
        else files.push_back(args[i]);
    }
    if (files.size() != 2) {
        std::fprintf(stderr, "usage: chdunpack [--jobs N] in.chd out.iso\n");
        return 2;
    }
    try {
        const uint64_t n = convert(to_path(files[0]), to_path(files[1]), jobs);
        std::printf("wrote %llu bytes\n", (unsigned long long)n);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "chd.py: %s\n", e.what());
        return 1;
    }
}
