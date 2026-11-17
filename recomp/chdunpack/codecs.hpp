// The decoders of chdunpack: every compression `chdman` writes for a disc, written for this project (nothing is copied).
// Each decoder gets the compressed bytes (in, n) and writes into out (capacity cap); it returns how many bytes it produced and throws
// ChdError for data that is damaged. None of them reads outside (in, n): a read past the end gives zero bits and is reported as an error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct ChdError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Raw deflate (RFC 1951), the "zlib" codec. Follows the same layout as Mark Adler's public description in RFC 1951; own code.
size_t inflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

// LZMA without a header (the "lzma" codec): lc=3, lp=0, pb=2 as chdman writes it; decodes until `cap` bytes are out.
// Follows Igor Pavlov's public LZMA specification (lzma-specification.txt, public domain); own code.
size_t lzma_raw(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

// Zstandard (RFC 8878), one or more frames, the "zstd" codec.
size_t zstd_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

// FLAC frames (RFC 9639) without the file header: 16-bit stereo; writes `samples` samples per channel as interleaved 16-bit values
// in the byte order asked for. Returns the bytes written (samples * 4).
size_t flac_decode(const uint8_t *in, size_t n, size_t samples, bool big_endian, uint8_t *out, size_t cap);

// CHD's own Huffman code (map and the "huff" codec): canonical codes, most significant bit first.
class BitsMsb {
public:
    BitsMsb(const uint8_t *p, size_t n) : p_(p), n_(n) {}
    uint32_t peek(int bits) const;  // up to 32 bits; zeros past the end
    uint32_t read(int bits) {
        const uint32_t v = peek(bits);
        pos += (size_t)bits;
        return v;
    }
    bool overflow() const { return pos > n_ * 8; }
    size_t pos = 0;  // bit position

private:
    const uint8_t *p_;
    size_t n_;
};

class HuffTree {
public:
    HuffTree(int numcodes, int maxbits);
    void import_rle(BitsMsb &bits);      // the hunk map's tree
    void import_huffman(BitsMsb &bits);  // the "huff" codec's tree
    int decode(BitsMsb &bits) const;
    // The "huff" codec: size symbols starting at bit position bitpos of (in, n).
    void decode_bytes(const uint8_t *in, size_t n, size_t bitpos, uint8_t *out, size_t size) const;
    std::vector<uint8_t> lengths;

private:
    void assign();
    int numcodes_, maxbits_, look_ = 0;
    std::vector<uint16_t> table_;      // (symbol << 5) | length for codes of at most look_ bits, 0 for the rest
    std::vector<uint16_t> slow_syms_;  // symbols of the longer codes ordered by (length, symbol)
    uint32_t slow_first_[33] = {}, slow_count_[33] = {}, slow_off_[33] = {};
};

// SHA-1, to check the data against the checksum the CHD file carries.
class Sha1 {
public:
    Sha1();
    void update(const uint8_t *data, size_t n);
    void finish(uint8_t digest[20]);

private:
    void block(const uint8_t *p);
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t total_ = 0;
};
