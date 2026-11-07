"""
Generic PlayStation 2 Audio Codec Suite:
- Sony SPU2 ADPCM (.VAG 'VAGp' and .ADS 'SShd'/'SSbd') Decoder & Encoder
- CRI Middleware ADX (.ADX '\\x80\\x00...(c)CRI') Decoder & Encoder

Provides bidirectional conversion between PS2 native audio formats (.vag, .adx, .ads)
and standard 16-bit signed PCM .WAV files.
"""

from __future__ import annotations

import io
import math
import shutil
import struct
import subprocess
import wave

# Official Sony SPU / SPU2 ADPCM IIR prediction filter coefficients (scaled by 64)
_SPU2_FILTER_COEF = (
    (0, 0),
    (60, 0),
    (115, -52),
    (98, -55),
    (122, -60),
)


def is_vag_bytes(data: bytes) -> bool:
    """Return True if data starts with a Sony PS2 'VAGp' header."""
    return len(data) >= 48 and data[:4] == b"VAGp"


def is_sshd_bytes(data: bytes) -> bool:
    """Return True if data is a Sony PS2 ADS ('SShd' + 'SSbd') audio stream."""
    return len(data) >= 40 and data[:4] == b"SShd" and b"SSbd" in data[:128]


def is_adx_bytes(data: bytes) -> bool:
    """Return True if data is a CRI Middleware ADX ('\\x80\\x00...(c)CRI') audio stream."""
    if len(data) < 20 or data[0] != 0x80 or data[1] != 0x00:
        return False
    copyright_offset = struct.unpack_from(">H", data, 2)[0]
    if copyright_offset < 6 or copyright_offset + 4 > len(data):
        return False
    return data[copyright_offset - 2 : copyright_offset + 4] == b"(c)CRI"


def decode_spu2_adpcm_blocks(adpcm_bytes: bytes) -> bytes:
    """
    Decode raw 16-byte PlayStation SPU2 ADPCM blocks into 16-bit signed little-endian PCM bytes.
    Each 16-byte block expands to 28 16-bit PCM samples.
    """
    num_blocks = len(adpcm_bytes) // 16
    pcm_samples: list[int] = []
    hist1 = 0
    hist2 = 0

    for b in range(num_blocks):
        blk = adpcm_bytes[b * 16 : (b + 1) * 16]
        if blk == b"\x00" * 16 and b > 0:
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

    return struct.pack(f"<{len(pcm_samples)}h", *pcm_samples)


def encode_spu2_adpcm_blocks(
    pcm_samples: list[int] | tuple[int, ...],
    loop_start_sample: int | None = None,
    loop_end_sample: int | None = None,
) -> bytes:
    """
    Encode 16-bit signed mono PCM samples into 16-byte Sony SPU2 ADPCM blocks.
    Selects the optimal predictor filter (0..4) and shift factor (0..12) per 28-sample block.
    """
    samples = list(pcm_samples)
    if not samples:
        samples = [0] * 28
    rem = len(samples) % 28
    if rem != 0:
        samples.extend([0] * (28 - rem))

    num_blocks = len(samples) // 28
    out = bytearray()
    hist1 = 0
    hist2 = 0

    for b in range(num_blocks):
        chunk = samples[b * 28 : (b + 1) * 28]
        best_err = float("inf")
        best_filt = 0
        best_shift = 0
        best_nibbles: list[int] = [0] * 28
        best_h1 = hist1
        best_h2 = hist2

        for filt, (k0, k1) in enumerate(_SPU2_FILTER_COEF):
            # Estimate peak residual to pick candidate shift quickly
            h1_est, h2_est = hist1, hist2
            max_res = 0
            for target in chunk:
                pred = (h1_est * k0 + h2_est * k1 + 32) >> 6
                res = abs(target - pred)
                if res > max_res:
                    max_res = res
                h2_est = h1_est
                h1_est = target

            # Determine ideal shift in 0..12 where (max_res << shift) fits in ~28672
            base_shift = 12
            while base_shift > 0 and (max_res << base_shift) > 28672:
                base_shift -= 1

            for shift in (max(0, base_shift - 1), base_shift, min(12, base_shift + 1)):
                h1, h2 = hist1, hist2
                err = 0
                nibbles: list[int] = []
                for target in chunk:
                    pred = (h1 * k0 + h2 * k1 + 32) >> 6
                    diff = target - pred
                    # Quantize diff: s = nibble << 12; s >> shift ~= diff  => nibble ~= (diff << shift) >> 12
                    q = int(round((diff * (1 << shift)) / 4096.0))
                    if q > 7:
                        q = 7
                    elif q < -8:
                        q = -8
                    nib = q & 0x0F
                    s = q << 12
                    recon = (s >> shift) + pred
                    if recon > 32767:
                        recon = 32767
                    elif recon < -32768:
                        recon = -32768
                    d = target - recon
                    err += d * d
                    h2 = h1
                    h1 = recon
                    nibbles.append(nib)
                    if err >= best_err:
                        break
                if err < best_err:
                    best_err = err
                    best_filt = filt
                    best_shift = shift
                    best_nibbles = nibbles
                    best_h1 = h1
                    best_h2 = h2

        hist1, hist2 = best_h1, best_h2
        flags = 0
        if loop_start_sample is not None:
            if b * 28 <= loop_start_sample < (b + 1) * 28:
                flags |= 0x04  # Loop start
            if loop_end_sample is not None and (b + 1) * 28 >= loop_end_sample:
                flags |= 0x03  # Loop end + repeat
            else:
                flags |= 0x02  # Loop region
        elif b == num_blocks - 1:
            flags = 0x01  # End of non-looping sample

        blk = bytearray(16)
        blk[0] = ((best_filt & 0x07) << 4) | (best_shift & 0x0F)
        blk[1] = flags
        for i in range(14):
            lo = best_nibbles[i * 2] & 0x0F
            hi = best_nibbles[i * 2 + 1] & 0x0F
            blk[2 + i] = lo | (hi << 4)
        out.extend(blk)

    # Append 16-byte SPU2 end marker block if non-looping
    if loop_start_sample is None:
        end_blk = bytearray(16)
        end_blk[1] = 0x07
        out.extend(end_blk)
    return bytes(out)


def decode_vag_to_wav_bytes(data: bytes) -> tuple[int, int, bytes]:
    """
    Decode a Sony PS2 .VAG ('VAGp') audio buffer into a complete 16-bit PCM .WAV byte stream.
    Returns (sample_rate, num_samples, wav_bytes).
    """
    if not is_vag_bytes(data):
        raise ValueError("Not a valid Sony VAG audio file (missing VAGp magic)")

    _version, _reserved, data_size, sample_rate = struct.unpack_from(">IIII", data, 4)
    if sample_rate == 0 or sample_rate > 96000:
        sample_rate = 22050
    adpcm_end = min(len(data), 48 + data_size) if data_size > 0 else len(data)
    pcm_bytes = decode_spu2_adpcm_blocks(data[48:adpcm_end])

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)

    return sample_rate, len(pcm_bytes) // 2, buf.getvalue()


def encode_wav_to_vag_bytes(wav_data: bytes, reference_vag: bytes | None = None) -> bytes:
    """
    Encode a standard 16-bit PCM .WAV byte stream into a Sony PS2 .VAG ('VAGp') file.
    Preserves header metadata and loop flags from reference_vag if provided.
    """
    with wave.open(io.BytesIO(wav_data), "rb") as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        sample_rate = wf.getframerate()
        nframes = wf.getnframes()
        raw_frames = wf.readframes(nframes)

    if sampwidth != 2:
        raise ValueError(f"Unsupported WAV sample width {sampwidth * 8}-bit (expected 16-bit PCM)")

    total_shorts = len(raw_frames) // 2
    all_samples = struct.unpack(f"<{total_shorts}h", raw_frames[: total_shorts * 2])
    if channels > 1:
        # Downmix multi-channel WAV to mono for SPU2 .VAG
        mono_samples = [
            int(sum(all_samples[i * channels : (i + 1) * channels]) // channels)
            for i in range(total_shorts // channels)
        ]
    else:
        mono_samples = list(all_samples)

    loop_start: int | None = None
    loop_end: int | None = None
    vag_version = 3
    vag_name = b"PS2_RECOMP_VAG\x00\x00"

    if reference_vag is not None and is_vag_bytes(reference_vag):
        vag_version = struct.unpack_from(">I", reference_vag, 4)[0]
        vag_name = reference_vag[32:48]
        ref_adpcm = reference_vag[48:]
        for b in range(len(ref_adpcm) // 16):
            flags = ref_adpcm[b * 16 + 1]
            if (flags & 0x04) and loop_start is None:
                loop_start = b * 28
            if (flags & 0x03) == 0x03:
                loop_end = (b + 1) * 28

    adpcm_payload = encode_spu2_adpcm_blocks(mono_samples, loop_start, loop_end)
    header = bytearray(48)
    header[0:4] = b"VAGp"
    struct.pack_into(">IIII", header, 4, vag_version, 0, len(adpcm_payload), sample_rate)
    header[32:48] = vag_name[:16].ljust(16, b"\x00")
    return bytes(header) + adpcm_payload


def decode_sshd_to_wav_bytes(data: bytes) -> tuple[int, int, bytes]:
    """
    Decode a Sony PS2 .ADS ('SShd' + 'SSbd') audio stream into 16-bit PCM .WAV bytes.
    Supports both 16-bit PCM (codec 0/1) and interleaved SPU2 ADPCM (codec 0x10).
    """
    if not is_sshd_bytes(data):
        raise ValueError("Not a valid Sony SShd/SSbd audio stream")

    sshd_size = struct.unpack_from("<I", data, 4)[0]
    codec, sample_rate, channels, interleave = struct.unpack_from("<IIII", data, 8)
    channels = max(1, min(channels, 2))
    if sample_rate == 0 or sample_rate > 96000:
        sample_rate = 48000

    ssbd_pos = data.find(b"SSbd", 8 + sshd_size)
    if ssbd_pos < 0:
        ssbd_pos = data.find(b"SSbd")
    if ssbd_pos < 0 or ssbd_pos + 8 > len(data):
        raise ValueError("Missing SSbd chunk in SShd stream")

    body_size = struct.unpack_from("<I", data, ssbd_pos + 4)[0]
    body = data[ssbd_pos + 8 : min(len(data), ssbd_pos + 8 + body_size)]

    if codec in (0, 1):
        # Raw 16-bit little-endian PCM (possibly interleaved by `interleave` bytes per channel)
        if channels == 1 or interleave == 0 or interleave == 2:
            pcm_bytes = body
        else:
            ch_buffers = [bytearray() for _ in range(channels)]
            stride = interleave * channels
            for offset in range(0, len(body), stride):
                for ch in range(channels):
                    chunk = body[offset + ch * interleave : offset + (ch + 1) * interleave]
                    ch_buffers[ch].extend(chunk)
            n_samples = min(len(b) // 2 for b in ch_buffers)
            ch_samples = [struct.unpack_from(f"<{n_samples}h", b) for b in ch_buffers]
            interleaved: list[int] = []
            for i in range(n_samples):
                for ch in range(channels):
                    interleaved.append(ch_samples[ch][i])
            pcm_bytes = struct.pack(f"<{len(interleaved)}h", *interleaved)
    else:
        # SPU2 ADPCM interleaved per channel
        if channels == 1 or interleave == 0:
            pcm_bytes = decode_spu2_adpcm_blocks(body)
            channels = 1
        else:
            ch_adpcm = [bytearray() for _ in range(channels)]
            stride = interleave * channels
            for offset in range(0, len(body), stride):
                for ch in range(channels):
                    ch_adpcm[ch].extend(body[offset + ch * interleave : offset + (ch + 1) * interleave])
            ch_pcm = [decode_spu2_adpcm_blocks(bytes(b)) for b in ch_adpcm]
            n_samples = min(len(p) // 2 for p in ch_pcm)
            ch_samples = [struct.unpack_from(f"<{n_samples}h", p) for p in ch_pcm]
            interleaved = []
            for i in range(n_samples):
                for ch in range(channels):
                    interleaved.append(ch_samples[ch][i])
            pcm_bytes = struct.pack(f"<{len(interleaved)}h", *interleaved)

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)

    return sample_rate, len(pcm_bytes) // (2 * channels), buf.getvalue()


def _decode_adx_pure_python(data: bytes) -> tuple[int, int, int, bytes]:
    """
    Pure-Python fallback decoder for CRI Middleware ADX 4-bit ADPCM streams.
    Returns (sample_rate, channels, total_samples, wav_bytes).
    """
    copyright_offset = struct.unpack_from(">H", data, 2)[0]
    data_offset = copyright_offset + 4
    encoding_type = data[4]
    block_size = data[5]
    sample_bitdepth = data[6]
    channels = data[7]
    sample_rate, total_samples = struct.unpack_from(">II", data, 8)
    highpass_freq = struct.unpack_from(">H", data, 16)[0]

    if encoding_type not in (2, 3, 4) or block_size != 18 or sample_bitdepth != 4 or channels < 1:
        raise ValueError(f"Unsupported ADX header parameters: type={encoding_type}, block={block_size}, ch={channels}")

    # CRI ADX 2nd-order IIR filter coefficients (scaled by 4096)
    sqrt2 = math.sqrt(2.0)
    a = sqrt2 - math.cos(2.0 * math.pi * float(highpass_freq) / float(sample_rate))
    b = sqrt2 - 1.0
    c = (a - math.sqrt((a + b) * (a - b))) / b
    coef1 = int(math.floor(c * 2.0 * 4096.0))
    coef2 = int(math.floor(-(c * c) * 4096.0))

    samples_per_block = (block_size - 2) * 2  # 32 samples per 18-byte block
    total_frames = (total_samples + samples_per_block - 1) // samples_per_block
    hist1 = [0] * channels
    hist2 = [0] * channels
    ch_out: list[list[int]] = [[] for _ in range(channels)]

    pos = data_offset
    stride = block_size * channels
    for _ in range(total_frames):
        if pos + stride > len(data):
            break
        for ch in range(channels):
            blk = data[pos + ch * block_size : pos + (ch + 1) * block_size]
            scale = (struct.unpack_from(">H", blk, 0)[0] & 0x1FFF) + 1
            h1 = hist1[ch]
            h2 = hist2[ch]
            out_list = ch_out[ch]
            for i in range(2, block_size):
                byte_val = blk[i]
                for nib in ((byte_val >> 4) & 0x0F, byte_val & 0x0F):
                    if nib >= 8:
                        nib -= 16
                    pred = (coef1 * h1 + coef2 * h2) >> 12
                    s = nib * scale + pred
                    if s > 32767:
                        s = 32767
                    elif s < -32768:
                        s = -32768
                    h2 = h1
                    h1 = s
                    out_list.append(s)
            hist1[ch] = h1
            hist2[ch] = h2
        pos += stride

    actual_samples = min(total_samples, min(len(c_list) for c_list in ch_out) if ch_out else 0)
    if channels == 1:
        pcm_bytes = struct.pack(f"<{actual_samples}h", *ch_out[0][:actual_samples])
    else:
        interleaved: list[int] = []
        for i in range(actual_samples):
            for ch in range(channels):
                interleaved.append(ch_out[ch][i])
        pcm_bytes = struct.pack(f"<{len(interleaved)}h", *interleaved)

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return sample_rate, channels, actual_samples, buf.getvalue()


def decode_adx_to_wav_bytes(data: bytes) -> tuple[int, int, bytes]:
    """
    Decode a CRI Middleware .ADX ('\\x80\\x00...(c)CRI') stream into 16-bit signed PCM .WAV bytes.
    Uses native C-speed ffmpeg when available and falls back to the built-in pure-Python decoder.
    Returns (sample_rate, total_samples, wav_bytes).
    """
    if not is_adx_bytes(data):
        raise ValueError("Not a valid CRI ADX audio file (missing (c)CRI signature)")

    channels = max(1, data[7])
    sample_rate, total_samples = struct.unpack_from(">II", data, 8)

    ffmpeg_bin = shutil.which("ffmpeg")
    if ffmpeg_bin:
        proc = subprocess.run(
            [
                ffmpeg_bin,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "adx",
                "-i",
                "pipe:0",
                "-f",
                "wav",
                "-c:a",
                "pcm_s16le",
                "pipe:1",
            ],
            input=data,
            capture_output=True,
            check=False,
        )
        if proc.returncode == 0 and proc.stdout.startswith(b"RIFF"):
            if total_samples == 0:
                total_samples = max(0, (len(proc.stdout) - 44) // (2 * channels))
            return sample_rate, total_samples, proc.stdout

    sr, _ch, n_samples, wav_bytes = _decode_adx_pure_python(data)
    return sr, n_samples, wav_bytes


def _encode_adx_pure_python(wav_data: bytes) -> bytes:
    """Pure-Python CRI ADX 4-bit ADPCM encoder fallback."""
    with wave.open(io.BytesIO(wav_data), "rb") as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        sample_rate = wf.getframerate()
        nframes = wf.getnframes()
        raw_frames = wf.readframes(nframes)

    if sampwidth != 2:
        raise ValueError("ADX encoder requires 16-bit PCM WAV input")

    total_shorts = len(raw_frames) // 2
    all_samples = struct.unpack(f"<{total_shorts}h", raw_frames[: total_shorts * 2])
    total_samples = total_shorts // channels
    ch_samples: list[list[int]] = []
    for ch in range(channels):
        s_list = list(all_samples[ch::channels])
        rem = len(s_list) % 32
        if rem != 0:
            s_list.extend([0] * (32 - rem))
        ch_samples.append(s_list)

    highpass_freq = 500
    sqrt2 = math.sqrt(2.0)
    a = sqrt2 - math.cos(2.0 * math.pi * float(highpass_freq) / float(sample_rate))
    b = sqrt2 - 1.0
    c = (a - math.sqrt((a + b) * (a - b))) / b
    coef1 = int(math.floor(c * 2.0 * 4096.0))
    coef2 = int(math.floor(-(c * c) * 4096.0))

    header = bytearray(0x20)
    header[0] = 0x80
    header[1] = 0x00
    struct.pack_into(">H", header, 2, 0x1C)  # copyright offset -> data starts at 0x20
    header[4] = 3  # standard ADX encoding
    header[5] = 18  # 18 bytes per block
    header[6] = 4  # 4 bits per sample
    header[7] = channels
    struct.pack_into(">IIHH", header, 8, sample_rate, total_samples, highpass_freq, 0x0300)
    header[0x1A:0x20] = b"(c)CRI"

    out = bytearray(header)
    hist1 = [0] * channels
    hist2 = [0] * channels
    num_blocks = len(ch_samples[0]) // 32

    for blk_idx in range(num_blocks):
        for ch in range(channels):
            chunk = ch_samples[ch][blk_idx * 32 : (blk_idx + 1) * 32]
            h1, h2 = hist1[ch], hist2[ch]
            # Estimate peak residual for scale
            max_res = 0
            h1_e, h2_e = h1, h2
            for target in chunk:
                pred = (coef1 * h1_e + coef2 * h2_e) >> 12
                res = abs(target - pred)
                if res > max_res:
                    max_res = res
                h2_e, h1_e = h1_e, target
            scale = max(1, min(0x1FFF, (max_res + 6) // 7))
            blk = bytearray(18)
            struct.pack_into(">H", blk, 0, scale - 1)
            nibbles: list[int] = []
            for target in chunk:
                pred = (coef1 * h1 + coef2 * h2) >> 12
                diff = target - pred
                q = int(round(diff / float(scale)))
                if q > 7:
                    q = 7
                elif q < -8:
                    q = -8
                recon = pred + q * scale
                if recon > 32767:
                    recon = 32767
                elif recon < -32768:
                    recon = -32768
                h2, h1 = h1, recon
                nibbles.append(q & 0x0F)
            hist1[ch], hist2[ch] = h1, h2
            for i in range(16):
                blk[2 + i] = (nibbles[i * 2] << 4) | nibbles[i * 2 + 1]
            out.extend(blk)

    # Append CRI ADX footer block (0x8001)
    footer = bytearray(18 * channels)
    footer[0] = 0x80
    footer[1] = 0x01
    struct.pack_into(">H", footer, 2, 18 * channels - 4)
    out.extend(footer)
    return bytes(out)


def encode_wav_to_adx_bytes(wav_data: bytes, reference_adx: bytes | None = None) -> bytes:
    """
    Encode a 16-bit PCM .WAV byte stream into a CRI Middleware .ADX audio file.
    Uses ffmpeg ('adpcm_adx') when available and falls back to pure Python.
    """
    with wave.open(io.BytesIO(wav_data), "rb") as wf:
        nframes = wf.getnframes()

    ffmpeg_bin = shutil.which("ffmpeg")
    if ffmpeg_bin:
        proc = subprocess.run(
            [
                ffmpeg_bin,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "wav",
                "-i",
                "pipe:0",
                "-f",
                "adx",
                "-c:a",
                "adpcm_adx",
                "pipe:1",
            ],
            input=wav_data,
            capture_output=True,
            check=False,
        )
        if proc.returncode == 0 and is_adx_bytes(proc.stdout):
            adx_buf = bytearray(proc.stdout)
            if struct.unpack_from(">I", adx_buf, 12)[0] == 0 and nframes > 0:
                struct.pack_into(">I", adx_buf, 12, nframes)
            return bytes(adx_buf)

    return _encode_adx_pure_python(wav_data)


def is_spu2_bd_bank(data: bytes) -> bool:
    """
    Return True if `data` is a raw PlayStation 2 SPU2 ADPCM sound bank (.BD):
    starts with a 16-byte zero block followed by valid 16-byte SPU2 ADPCM blocks
    (shift <= 12, filter <= 4, flags <= 7).
    """
    if len(data) < 64 or (len(data) % 16) != 0:
        return False
    if data[:16] != b"\x00" * 16:
        return False
    non_zero = 0
    for off in range(16, min(len(data), 512), 16):
        blk = data[off : off + 16]
        if blk == b"\x00" * 16:
            continue
        shift = blk[0] & 0x0F
        filt = (blk[0] >> 4) & 0x0F
        flags = blk[1]
        if shift > 12 or filt > 4 or flags > 7:
            return False
        non_zero += 1
    return non_zero >= 2


def split_spu2_bd_bank_to_vag(
    bd_data: bytes, default_sample_rate: int = 22050
) -> list[tuple[int, int, bytes]]:
    """
    Split a raw SPU2 ADPCM .BD sound bank into individual .VAG ('VAGp') samples.
    Returns a list of (byte_offset, byte_length, vag_bytes).
    """
    samples: list[tuple[int, int, bytes]] = []
    cur = 0
    n = len(bd_data)
    idx = 0
    while cur + 16 <= n:
        while cur + 32 <= n and bd_data[cur : cur + 16] == b"\x00" * 16 and bd_data[cur + 16 : cur + 32] == b"\x00" * 16:
            cur += 16
        start = cur
        cur += 16
        while cur + 16 <= n:
            flags = bd_data[cur + 1]
            cur += 16
            if flags in (1, 3, 7):
                if cur + 16 <= n and bd_data[cur + 1] == 7:
                    cur += 16
                break
        length = cur - start
        if length >= 48:
            adpcm_slice = bd_data[start:cur]
            header = bytearray(48)
            header[0:4] = b"VAGp"
            struct.pack_into(">IIII", header, 4, 3, 0, len(adpcm_slice), default_sample_rate)
            name = f"sfx_{idx:04d}".encode("ascii")[:16].ljust(16, b"\x00")
            header[32:48] = name
            samples.append((start, length, bytes(header) + adpcm_slice))
            idx += 1
    return samples

