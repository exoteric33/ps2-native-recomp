"""
Generic PlayStation 2 Video Demuxer, Converter & Remuxer:
- CRI Middleware Sofdec (.SFD: MPEG-2 Video 0xE0 + CRI ADX Audio 0xC0)
- Sony PlayStation Stream (.PSS: MPEG-2 Video 0xE0 + Sony SShd/SSbd or VAG/PCM Audio 0xBD)

Converts .SFD and .PSS video streams into universally playable .MP4 files WITH synchronized
stereo sound, and supports remuxing modified .MP4 files back into .SFD / .PSS containers.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from .ps2_audio import (
    decode_adx_to_wav_bytes,
    decode_sshd_to_wav_bytes,
    decode_vag_to_wav_bytes,
    encode_wav_to_adx_bytes,
    is_adx_bytes,
    is_sshd_bytes,
    is_vag_bytes,
)


def is_ps2_mpeg_ps_bytes(data: bytes) -> bool:
    """Return True if data begins with an MPEG-1/MPEG-2 Program Stream pack header (0x000001BA)."""
    return len(data) >= 32 and data[:4] == b"\x00\x00\x01\xba"


def detect_ps2_video_subtype(data: bytes) -> str | None:
    """
    Detect whether an MPEG Program Stream buffer is a CRI Sofdec (.sfd) or Sony (.pss) video.
    Returns '.sfd', '.pss', or None if not an MPEG PS stream.
    """
    if not is_ps2_mpeg_ps_bytes(data):
        return None
    probe = data[: min(len(data), 131072)]
    if b"Sofdec" in probe or b"(c)CRI" in probe:
        return ".sfd"
    return ".pss"


def _parse_pes_payload_offset(data: bytes | memoryview, pos: int, pkt_end: int) -> int:
    """
    Given a PES packet starting at `pos` (00 00 01 <id> <len_hi> <len_lo>),
    return the byte offset where the elementary payload begins.
    Supports both MPEG-1 PES headers (used by CRI Sofdec .SFD) and MPEG-2 PES headers (used by .PSS).
    """
    p = pos + 6
    while p < pkt_end and data[p] == 0xFF:
        p += 1
    if p >= pkt_end:
        return pkt_end

    first = data[p]
    if (first & 0xC0) == 0x80:
        # MPEG-2 PES header
        if p + 3 > pkt_end:
            return pkt_end
        header_data_len = data[p + 2]
        return min(pkt_end, p + 3 + header_data_len)

    # MPEG-1 PES header (used in CRI Sofdec .SFD)
    if (first & 0xC0) == 0x40:
        p += 2
        if p >= pkt_end:
            return pkt_end
        first = data[p]
    if (first & 0xF0) == 0x20:
        p += 5
    elif (first & 0xF0) == 0x30:
        p += 10
    elif first == 0x0F:
        p += 1
    return min(pkt_end, p)


def demux_ps2_mpeg_ps(data: bytes) -> tuple[bytes, bytes | None, str]:
    """
    Demux a PS2 .SFD or .PSS MPEG Program Stream into:
      (m2v_video_bytes, wav_audio_bytes_or_None, audio_kind)
    where audio_kind is 'adx', 'sshd', 'vag', or 'none'.
    """
    m2v_chunks: list[bytes] = []
    c0_chunks: list[bytes] = []
    bd_chunks: list[bytes] = []

    n = len(data)
    pos = 0
    while pos + 6 <= n:
        # Find next start code prefix 0x000001
        if data[pos] != 0 or data[pos + 1] != 0 or data[pos + 2] != 1:
            next_sc = data.find(b"\x00\x00\x01", pos)
            if next_sc < 0:
                break
            pos = next_sc
            if pos + 6 > n:
                break

        stream_id = data[pos + 3]
        if stream_id == 0xB9:  # MPEG Program End Code
            break
        if stream_id == 0xBA:  # Pack Header
            if pos + 12 > n:
                break
            if (data[pos + 4] & 0xC0) == 0x40:
                # MPEG-2 Pack Header (14 bytes + stuffing)
                if pos + 14 > n:
                    break
                stuffing = data[pos + 13] & 0x07
                pos += 14 + stuffing
            else:
                # MPEG-1 Pack Header (12 bytes)
                pos += 12
            continue

        pkt_len = struct.unpack_from(">H", data, pos + 4)[0]
        pkt_end = min(n, pos + 6 + pkt_len)
        if pkt_len == 0:
            pos += 4
            continue

        if 0xE0 <= stream_id <= 0xEF:
            payload_start = _parse_pes_payload_offset(data, pos, pkt_end)
            if payload_start < pkt_end:
                m2v_chunks.append(data[payload_start:pkt_end])
        elif 0xC0 <= stream_id <= 0xDF:
            payload_start = _parse_pes_payload_offset(data, pos, pkt_end)
            if payload_start < pkt_end:
                c0_chunks.append(data[payload_start:pkt_end])
        elif stream_id == 0xBD:
            payload_start = _parse_pes_payload_offset(data, pos, pkt_end)
            if payload_start < pkt_end:
                bd_chunks.append(data[payload_start:pkt_end])

        pos = pkt_end

    m2v_bytes = b"".join(m2v_chunks)
    wav_bytes: bytes | None = None
    audio_kind = "none"

    if c0_chunks:
        c0_raw = b"".join(c0_chunks)
        # Locate ADX header if preceded by any padding
        adx_pos = 0
        if not is_adx_bytes(c0_raw):
            cri_pos = c0_raw.find(b"(c)CRI", 0, min(len(c0_raw), 4096))
            if cri_pos >= 6:
                # Scan backwards for 0x80 0x00
                for cand in range(max(0, cri_pos - 2048), cri_pos - 2):
                    if c0_raw[cand] == 0x80 and c0_raw[cand + 1] == 0x00:
                        if is_adx_bytes(c0_raw[cand:]):
                            adx_pos = cand
                            break
        if is_adx_bytes(c0_raw[adx_pos:]):
            try:
                _sr, _ns, wav_bytes = decode_adx_to_wav_bytes(c0_raw[adx_pos:])
                audio_kind = "adx"
            except Exception:
                wav_bytes = None

    if wav_bytes is None and bd_chunks:
        # Sony .PSS Private Stream 1 often has a 4-byte subheader per PES packet
        bd_raw = b"".join(bd_chunks)
        sshd_pos = bd_raw.find(b"SShd", 0, min(len(bd_raw), 256))
        if sshd_pos > 0:
            # Strip `sshd_pos`-byte subheader from each PES packet
            bd_clean = b"".join(chunk[sshd_pos:] for chunk in bd_chunks if len(chunk) > sshd_pos)
        else:
            bd_clean = bd_raw

        if is_sshd_bytes(bd_clean):
            try:
                _sr, _ns, wav_bytes = decode_sshd_to_wav_bytes(bd_clean)
                audio_kind = "sshd"
            except Exception:
                wav_bytes = None
        elif is_vag_bytes(bd_clean):
            try:
                _sr, _ns, wav_bytes = decode_vag_to_wav_bytes(bd_clean)
                audio_kind = "vag"
            except Exception:
                wav_bytes = None

    return m2v_bytes, wav_bytes, audio_kind


def convert_sfd_pss_to_mp4(data: bytes, output_mp4_path: Path) -> bool:
    """
    Demux a PS2 .SFD or .PSS video buffer, decode its CRI ADX or Sony SPU2 audio track,
    and mux both into a standard playable .MP4 file with synchronized stereo audio.
    """
    m2v_bytes, wav_bytes, _audio_kind = demux_ps2_mpeg_ps(data)
    if not m2v_bytes:
        return False

    output_mp4_path.parent.mkdir(parents=True, exist_ok=True)
    ffmpeg_bin = shutil.which("ffmpeg")
    if not ffmpeg_bin:
        # Fallback if ffmpeg is not installed: write .m2v and .wav side by side
        output_mp4_path.with_suffix(".m2v").write_bytes(m2v_bytes)
        if wav_bytes:
            output_mp4_path.with_suffix(".wav").write_bytes(wav_bytes)
        return False

    with tempfile.TemporaryDirectory(prefix="ps2_vid_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        m2v_path = tmp_path / "video.m2v"
        m2v_path.write_bytes(m2v_bytes)

        cmd = [
            ffmpeg_bin,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(m2v_path),
        ]
        if wav_bytes:
            wav_path = tmp_path / "audio.wav"
            wav_path.write_bytes(wav_bytes)
            cmd.extend(["-i", str(wav_path)])

        # Encode video to universally playable H.264 MP4 (or MPEG-4 fallback) + AAC stereo audio
        cmd.extend(
            [
                "-c:v",
                "libx264",
                "-preset",
                "veryfast",
                "-crf",
                "18",
                "-pix_fmt",
                "yuv420p",
            ]
        )
        if wav_bytes:
            cmd.extend(["-c:a", "aac", "-b:a", "192k", "-shortest"])
        cmd.extend(["-movflags", "+faststart", str(output_mp4_path)])

        proc = subprocess.run(cmd, capture_output=True, check=False)
        if proc.returncode != 0:
            # Fallback if libx264 is unavailable in a minimal ffmpeg build: use mpeg4 video codec
            cmd_fallback = [
                ffmpeg_bin,
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(m2v_path),
            ]
            if wav_bytes:
                cmd_fallback.extend(["-i", str(tmp_path / "audio.wav")])
            cmd_fallback.extend(["-c:v", "mpeg4", "-q:v", "3"])
            if wav_bytes:
                cmd_fallback.extend(["-c:a", "aac", "-b:a", "192k", "-shortest"])
            cmd_fallback.append(str(output_mp4_path))
            proc = subprocess.run(cmd_fallback, capture_output=True, check=False)

    return output_mp4_path.is_file() and output_mp4_path.stat().st_size > 0


def _make_mpeg1_pack_and_pes(stream_id: int, payload: bytes, scr_ticks: int) -> bytes:
    """Build a 2048-byte aligned MPEG-1 pack + PES packet (CRI Sofdec .SFD compatible)."""
    pack_hdr = bytearray(12)
    pack_hdr[0:4] = b"\x00\x00\x01\xba"
    # MPEG-1 SCR marker bits
    b4 = 0x21 | (((scr_ticks >> 30) & 0x07) << 1)
    b5 = (scr_ticks >> 22) & 0xFF
    b6 = (((scr_ticks >> 15) & 0x7F) << 1) | 1
    b7 = (scr_ticks >> 7) & 0xFF
    b8 = (((scr_ticks) & 0x7F) << 1) | 1
    pack_hdr[4:9] = bytes((b4, b5, b6, b7, b8))
    pack_hdr[9:12] = b"\x80\x1f\xff"  # mux rate marker

    pes_hdr_body = b"\x40\x20\x0f"  # STD buffer + no PTS/DTS marker
    pkt_len = len(pes_hdr_body) + len(payload)
    pes_hdr = struct.pack(">BBBBH", 0x00, 0x00, 0x01, stream_id, pkt_len) + pes_hdr_body
    return bytes(pack_hdr) + pes_hdr + payload


def encode_mp4_to_sfd_pss(
    mp4_path: Path,
    reference_video_bytes: bytes | None = None,
    target_ext: str = ".sfd",
) -> bytes:
    """
    Convert a modded .MP4 video file back into a PS2 .SFD (MPEG-2 Video + CRI ADX Audio)
    or .PSS stream using ffmpeg.
    """
    ffmpeg_bin = shutil.which("ffmpeg")
    if not ffmpeg_bin:
        raise RuntimeError("ffmpeg is required to encode .mp4 back into .sfd/.pss")

    with tempfile.TemporaryDirectory(prefix="ps2_vid_enc_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        m2v_path = tmp_path / "video.m2v"
        wav_path = tmp_path / "audio.wav"

        # 1. Extract/transcode MPEG-2 video elementary stream (.m2v)
        subprocess.run(
            [
                ffmpeg_bin,
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(mp4_path),
                "-an",
                "-c:v",
                "mpeg2video",
                "-b:v",
                "4500k",
                "-maxrate",
                "6000k",
                "-bufsize",
                "1835k",
                "-pix_fmt",
                "yuv420p",
                "-f",
                "mpeg2video",
                str(m2v_path),
            ],
            check=True,
        )

        # 2. Extract audio track to 48 kHz 16-bit stereo WAV (if present)
        audio_proc = subprocess.run(
            [
                ffmpeg_bin,
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(mp4_path),
                "-vn",
                "-ar",
                "48000",
                "-ac",
                "2",
                "-c:a",
                "pcm_s16le",
                str(wav_path),
            ],
            capture_output=True,
            check=False,
        )

        m2v_bytes = m2v_path.read_bytes()
        adx_bytes = b""
        if audio_proc.returncode == 0 and wav_path.is_file() and wav_path.stat().st_size > 44:
            adx_bytes = encode_wav_to_adx_bytes(wav_path.read_bytes())

    # Interleave video (0xE0) and ADX audio (0xC0) into MPEG Program Stream packs
    out = bytearray()
    if reference_video_bytes and len(reference_video_bytes) >= 0x2000 and b"Sofdec" in reference_video_bytes[:0x2000]:
        # Preserve original Sofdec system header pack (first 0x2000 bytes)
        out.extend(reference_video_bytes[:0x2000])

    v_pos = 0
    a_pos = 0
    v_chunk_size = 2000
    a_chunk_size = 2000
    scr = 0
    pack_idx = 0

    while v_pos < len(m2v_bytes) or a_pos < len(adx_bytes):
        if a_pos < len(adx_bytes) and (pack_idx % 5 == 0 or v_pos >= len(m2v_bytes)):
            chunk = adx_bytes[a_pos : a_pos + a_chunk_size]
            a_pos += len(chunk)
            out.extend(_make_mpeg1_pack_and_pes(0xC0, chunk, scr))
        else:
            chunk = m2v_bytes[v_pos : v_pos + v_chunk_size]
            v_pos += len(chunk)
            out.extend(_make_mpeg1_pack_and_pes(0xE0, chunk, scr))
        scr += 300
        pack_idx += 1

    out.extend(b"\x00\x00\x01\xb9")  # MPEG Program End code
    return bytes(out)
