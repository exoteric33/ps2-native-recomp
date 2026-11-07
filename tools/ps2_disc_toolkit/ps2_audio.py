"""
Generic PlayStation 2 SPU2 ADPCM (.VAG) Audio Decoder & WAV Encoder.

Decodes standard Sony PS2 SDK .VAG files ('VAGp' header + 16-byte SPU2 ADPCM blocks)
into standard 16-bit signed PCM .WAV files using only the Python standard library.
"""

import io
import struct
import wave

# Official Sony SPU / SPU2 ADPCM IIR prediction filter coefficients (scaled by 64)
_SPU2_FILTER_COEF = (
    (0, 0),
    (60, 0),
    (115, -52),
    (98, -55),
    (122, -60),
)


def decode_spu2_adpcm_blocks(adpcm_bytes):
    """
    Decode raw 16-byte PlayStation SPU2 ADPCM blocks into 16-bit signed little-endian PCM bytes.
    Each 16-byte block expands to 28 16-bit PCM samples.
    """
    num_blocks = len(adpcm_bytes) // 16
    pcm_samples = []
    hist1 = 0
    hist2 = 0

    for b in range(num_blocks):
        blk = adpcm_bytes[b * 16:(b + 1) * 16]
        if blk == b'\x00' * 16 and b > 0:
            break
        shift_filter = blk[0]
        flags = blk[1]
        if flags == 7:
            break
        shift = shift_filter & 0x0F
        if shift > 12:
            shift = 9
        filt = (shift_filter >> 4) & 0x07
        if filt >= len(_SPU2_FILTER_COEF):
            filt = 0
        k0, k1 = _SPU2_FILTER_COEF[filt]

        for i in range(14):
            byte_val = blk[2 + i]
            for nibble in (byte_val & 0x0F, (byte_val >> 4) & 0x0F):
                # Sign-extend 4-bit nibble to 16-bit and shift right by 'shift'
                s = nibble << 12
                if s & 0x8000:
                    s -= 0x10000
                sample = (s >> shift) + ((hist1 * k0 + hist2 * k1 + 32) >> 6)
                if sample > 32767:
                    sample = 32767
                elif sample < -32768:
                    sample = -32768
                hist2 = hist1
                hist1 = sample
                pcm_samples.append(sample)

        if flags & 1:  # Loop end / stream end flag
            break

    return struct.pack(f'<{len(pcm_samples)}h', *pcm_samples)


def decode_vag_to_wav_bytes(data):
    """
    Decode a Sony PS2 .VAG ('VAGp') audio buffer into a complete 16-bit PCM .WAV byte stream.
    Returns (sample_rate, num_samples, wav_bytes).
    """
    if len(data) < 48 or data[:4] != b'VAGp':
        raise ValueError('Not a valid Sony VAG audio file (missing VAGp magic)')

    _version, _reserved, data_size, sample_rate = struct.unpack_from('>IIII', data, 4)
    if sample_rate == 0 or sample_rate > 96000:
        sample_rate = 22050
    adpcm_end = min(len(data), 48 + data_size) if data_size > 0 else len(data)
    pcm_bytes = decode_spu2_adpcm_blocks(data[48:adpcm_end])

    buf = io.BytesIO()
    with wave.open(buf, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)

    return sample_rate, len(pcm_bytes) // 2, buf.getvalue()
