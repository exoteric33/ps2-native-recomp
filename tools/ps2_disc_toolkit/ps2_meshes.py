"""
Generic PlayStation 2 3D Geometry Decoder & Wavefront .OBJ Exporter.

Supports:
  1. Recursive chunk-based .FMOD 3D models ('FMOD' magic: File Root 0x01, Main Mesh 0x02,
     SubMesh Object 0x04, Material 0x09, Texture Index 0x0A, Triangle Strips 0x30000/0x40000,
     Vertices 0x50000, Normals 0x60000, UVs 0x70000, Vertex Colors 0x80000).
  2. Direct PS2 VU1 / VIF1 UNPACK geometry packets (.vif / raw DMA display lists), honoring
     the Graphics Synthesizer ADC bit (0x8000) to break triangle strips without degenerate
     connecting faces.
  3. Automatic bounding-box computation and spatial role classification
     ('macro_terrain', 'cliff_or_vertical_prop', 'unit_prop') for 3D scene editors.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field


@dataclass
class Ps2SubMesh:
    index: int
    name: str = ""
    vertices: list[tuple[float, float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    uvs: list[tuple[float, float]] = field(default_factory=list)
    colors: list[tuple[float, float, float, float]] = field(default_factory=list)
    faces: list[tuple[int, int, int]] = field(default_factory=list)
    material_ids: list[int] = field(default_factory=list)
    texture_indices: list[int] = field(default_factory=list)

    def compute_bounds(self) -> dict:
        if not self.vertices:
            return {
                "min": [0.0, 0.0, 0.0],
                "max": [0.0, 0.0, 0.0],
                "size": [0.0, 0.0, 0.0],
                "role": "empty",
            }
        xs = [v[0] for v in self.vertices if math.isfinite(v[0])]
        ys = [v[1] for v in self.vertices if math.isfinite(v[1])]
        zs = [v[2] for v in self.vertices if math.isfinite(v[2])]
        if not xs or not ys or not zs:
            return {
                "min": [0.0, 0.0, 0.0],
                "max": [0.0, 0.0, 0.0],
                "size": [0.0, 0.0, 0.0],
                "role": "empty",
            }
        min_x, max_x = min(xs), max(xs)
        min_y, max_y = min(ys), max(ys)
        min_z, max_z = min(zs), max(zs)
        dx = max_x - min_x
        dy = max_y - min_y
        dz = max_z - min_z

        if dx > 50.0 or dz > 50.0:
            role = "macro_terrain"
        elif dy > max(dx, dz) * 1.3 and dy > 4.0:
            role = "cliff_or_vertical_prop"
        else:
            role = "unit_prop"

        return {
            "min": [round(min_x, 3), round(min_y, 3), round(min_z, 3)],
            "max": [round(max_x, 3), round(max_y, 3), round(max_z, 3)],
            "size": [round(dx, 3), round(dy, 3), round(dz, 3)],
            "role": role,
        }


def _read_fmod_tristrip_chunk(buf: bytes, sub: Ps2SubMesh) -> None:
    """
    Decode an FMOD primitive chunk (0x00030000 or 0x00040000) containing triangle strips.
    Alternates winding order on odd/even steps and skips degenerate triangles.
    """
    if len(buf) < 16:
        return
    _btype, _strip_count, bsize = struct.unpack_from("<IiI", buf, 0)
    limit = min(len(buf), bsize)
    pos = 12

    # Note: some FMOD tristrip chunks store a 4-byte material_id right at offset 12 if the
    # first word's high bits (0xF0000000) are zero and the next word has strip flags, or
    # directly start with the strip length word (raw_len & 0x0FFFFFFF). We probe safely:
    if pos + 8 <= limit:
        w0, w1 = struct.unpack_from("<II", buf, pos)
        n0 = w0 & 0x0FFFFFFF
        n1 = w1 & 0x0FFFFFFF
        # If w0 looks like a material index (< 256) and w1 has a strip flag or valid strip length
        if (w0 >> 28) == 0 and (w1 >> 28) != 0 and 3 <= n1 <= 4096 and pos + 4 + 4 + n1 * 4 <= limit:
            sub.material_ids.append(w0)
            pos += 4

    while pos + 4 <= limit:
        raw_len = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        n_verts = raw_len & 0x0FFFFFFF
        if n_verts < 3 or pos + n_verts * 4 > limit:
            break
        strip_indices = [
            struct.unpack_from("<I", buf, pos + k * 4)[0] & 0xFFFF
            for k in range(n_verts)
        ]
        pos += n_verts * 4
        for k in range(2, len(strip_indices)):
            i0, i1, i2 = strip_indices[k - 2], strip_indices[k - 1], strip_indices[k]
            if i0 != i1 and i1 != i2 and i0 != i2:
                if (k & 1) == 0:
                    sub.faces.append((i0, i1, i2))
                else:
                    sub.faces.append((i1, i0, i2))


def is_fmod_bytes(buf: bytes) -> bool:
    """
    Return True if `buf` is a valid PS2 .FMOD 3D model chunk tree.
    Handles both explicit 'FMOD' magic headers and standard root chunk headers
    (block_type == 0x00000001 or 0x00000002, child_count in 1..256, block_size == len(buf)).
    """
    n = len(buf)
    if n < 24:
        return False
    if buf[:4] == b"FMOD":
        return True
    btype, count, bsize = struct.unpack_from("<IiI", buf, 0)
    if btype not in (1, 2) or not (1 <= count <= 256):
        return False
    if bsize < 24 or bsize > n or (n - bsize) > 32:
        return False
    # Verify that the immediate child blocks inside the root chunk form a valid chain
    pos = 12
    seen = 0
    has_mesh_child = False
    while pos + 12 <= bsize and seen < count + 4:
        c_type, c_cnt, c_sz = struct.unpack_from("<IiI", buf, pos)
        if c_sz < 12 or pos + c_sz > bsize or c_cnt < 0 or c_cnt > 65536:
            return False
        if c_type in (0x00000002, 0x00000004, 0x00020000, 0x00000009, 0x0000000A):
            has_mesh_child = True
        pos += c_sz
        seen += 1
    return pos == bsize and has_mesh_child


def decode_fmod_to_meshes(fmod_bytes: bytes) -> list[Ps2SubMesh]:
    """
    Walk the recursive chunk tree of a PS2 .FMOD model buffer and return all valid Ps2SubMesh objects.
    Accepts either a file starting with 'FMOD' header or a raw root chunk (block_type == 1 or 2).
    """
    if len(fmod_bytes) < 16:
        return []

    submeshes: list[Ps2SubMesh] = []
    global_tex_indices: list[int] = []

    def walk_blocks(buf: bytes, current_sub: Ps2SubMesh | None = None) -> None:
        pos = 0
        n = len(buf)
        if n >= 16 and buf[:4] == b"FMOD":
            for cand in (4, 8, 12, 16):
                if cand + 12 <= n:
                    bt, _cnt, bsz = struct.unpack_from("<IiI", buf, cand)
                    if bt in (1, 2, 4) and 12 <= bsz <= n - cand:
                        pos = cand
                        break

        while pos + 12 <= n:
            btype, count, bsize = struct.unpack_from("<IiI", buf, pos)
            if bsize < 12 or pos + bsize > n:
                break
            payload = buf[pos + 12 : pos + bsize]
            plen = len(payload)

            # SubMesh / Object block (0x00000004)
            if btype == 0x00000004:
                new_sub = Ps2SubMesh(index=len(submeshes), name=f"part{len(submeshes):02d}")
                submeshes.append(new_sub)
                walk_blocks(payload, new_sub)

            # Parent containers (File Root 0x01, Main Object 0x02, Group 0x03, Primitive Container 0x05)
            elif btype in (0x00000001, 0x00000002, 0x00000003, 0x00000005):
                walk_blocks(payload, current_sub)

            elif btype == 0x0000000A:
                for k in range(min(max(0, count), plen // 4)):
                    t_idx = struct.unpack_from("<I", payload, k * 4)[0]
                    global_tex_indices.append(t_idx)
                    if current_sub is not None:
                        current_sub.texture_indices.append(t_idx)

            elif current_sub is not None:
                if btype in (0x00030000, 0x00040000):
                    _read_fmod_tristrip_chunk(buf[pos : pos + bsize], current_sub)

                elif btype == 0x00050000:
                    # Either float32x3 vertices (12B stride) or uint32 material ID list (4B stride)
                    if count > 0 and plen == count * 12 and not current_sub.vertices:
                        for k in range(count):
                            vx, vy, vz = struct.unpack_from("<fff", payload, k * 12)
                            current_sub.vertices.append((vx, vy, vz))
                    elif count > 0 and plen == count * 4:
                        for k in range(count):
                            current_sub.material_ids.append(struct.unpack_from("<I", payload, k * 4)[0])

                elif btype == 0x00060000:
                    # Either float32x3 normals (12B stride) or per-strip material map (4B stride)
                    if count > 0 and plen == count * 12 and not current_sub.normals:
                        for k in range(count):
                            nx, ny, nz = struct.unpack_from("<fff", payload, k * 12)
                            current_sub.normals.append((nx, ny, nz))

                elif btype == 0x00070000:
                    # PS2 FMOD uses 0x00070000 for float32x3 Vertices (12B stride);
                    # Frontier FMOD uses 0x00070000 for float32x2 UVs (8B stride)
                    if count > 0 and plen == count * 12 and not current_sub.vertices:
                        for k in range(count):
                            vx, vy, vz = struct.unpack_from("<fff", payload, k * 12)
                            current_sub.vertices.append((vx, vy, vz))
                    elif count > 0 and plen == count * 8 and not current_sub.uvs:
                        for k in range(count):
                            u, v = struct.unpack_from("<ff", payload, k * 8)
                            current_sub.uvs.append((u, 1.0 - v))

                elif btype == 0x00080000:
                    # PS2 FMOD uses 0x00080000 for float32x3 Normals (12B stride);
                    # Frontier FMOD uses 0x00080000 for Vertex Colors (16B or 4B stride)
                    if count > 0 and plen == count * 12 and not current_sub.normals:
                        for k in range(count):
                            nx, ny, nz = struct.unpack_from("<fff", payload, k * 12)
                            current_sub.normals.append((nx, ny, nz))
                    elif count > 0 and plen == count * 16 and not current_sub.colors:
                        for k in range(count):
                            r, g, b, a = struct.unpack_from("<ffff", payload, k * 16)
                            scale = 1.0 / 255.0 if max(r, g, b) > 2.0 else 1.0
                            current_sub.colors.append((
                                max(0.0, min(1.0, r * scale)),
                                max(0.0, min(1.0, g * scale)),
                                max(0.0, min(1.0, b * scale)),
                                max(0.0, min(1.0, a * scale)),
                            ))
                    elif count > 0 and plen == count * 4 and not current_sub.colors:
                        for k in range(count):
                            r, g, b, a = struct.unpack_from("<BBBB", payload, k * 4)
                            current_sub.colors.append((r / 255.0, g / 255.0, b / 255.0, min(1.0, a / 128.0)))

                elif btype == 0x000A0000:
                    # PS2 FMOD float32x2 UVs (8B stride)
                    if count > 0 and plen == count * 8 and not current_sub.uvs:
                        for k in range(count):
                            u, v = struct.unpack_from("<ff", payload, k * 8)
                            current_sub.uvs.append((u, 1.0 - v))

                elif btype == 0x000B0000:
                    # PS2 FMOD float32x4 RGBA Vertex Colors (16B stride)
                    if count > 0 and plen == count * 16 and not current_sub.colors:
                        for k in range(count):
                            r, g, b, a = struct.unpack_from("<ffff", payload, k * 16)
                            scale = 1.0 / 255.0 if max(r, g, b) > 2.0 else 1.0
                            current_sub.colors.append((
                                max(0.0, min(1.0, r * scale)),
                                max(0.0, min(1.0, g * scale)),
                                max(0.0, min(1.0, b * scale)),
                                max(0.0, min(1.0, a * scale)),
                            ))

            pos += bsize

    walk_blocks(fmod_bytes)
    valid = [s for s in submeshes if len(s.vertices) >= 3 and len(s.faces) >= 1]
    for s in valid:
        if not s.texture_indices and global_tex_indices:
            s.texture_indices = list(global_tex_indices)
    return valid


def decode_vif_to_meshes(vif_bytes: bytes) -> list[Ps2SubMesh]:
    """
    Scan a PS2 VIF1 / VU1 geometry packet stream for UNPACK V4-32 (0x6C) / V3-32 (0x68) /
    V2-16 (0x65) vertex/UV batches and reconstruct triangle strips honoring the GS ADC
    bit (bit 15 = 0x8000 in the W/flag word or kick flag).
    """
    if len(vif_bytes) < 32:
        return []

    sub = Ps2SubMesh(index=0, name="vif_mesh")
    pos = 0
    n = len(vif_bytes)

    batch_verts: list[tuple[float, float, float, bool]] = []
    batch_uvs: list[tuple[float, float]] = []
    batch_nrms: list[tuple[float, float, float]] = []

    def flush_batch() -> None:
        if len(batch_verts) < 3:
            batch_verts.clear()
            batch_uvs.clear()
            batch_nrms.clear()
            return
        base_idx = len(sub.vertices)
        for vx, vy, vz, _adc in batch_verts:
            sub.vertices.append((vx, vy, vz))
        if len(batch_uvs) == len(batch_verts):
            sub.uvs.extend(batch_uvs)
        if len(batch_nrms) == len(batch_verts):
            sub.normals.extend(batch_nrms)

        for k in range(2, len(batch_verts)):
            adc = batch_verts[k][3]
            if adc:
                continue
            i0, i1, i2 = base_idx + k - 2, base_idx + k - 1, base_idx + k
            if (k & 1) == 0:
                sub.faces.append((i0, i1, i2))
            else:
                sub.faces.append((i1, i0, i2))

        batch_verts.clear()
        batch_uvs.clear()
        batch_nrms.clear()

    while pos + 4 <= n:
        imm, num, cmd = struct.unpack_from("<HBB", vif_bytes, pos)
        op = cmd & 0x7F
        # MSCAL / MSCNT (0x14, 0x17) triggers VU1 execution of the current batch
        if op in (0x14, 0x15, 0x17):
            flush_batch()
            pos += 4
            continue

        # UNPACK V4-32 (0x6C)
        if (op & 0x6F) == 0x6C and 3 <= num <= 128 and pos + 4 + num * 16 <= n:
            payload = vif_bytes[pos + 4 : pos + 4 + num * 16]
            # Check if coordinates are plausible 3D floats
            plausible = True
            cand_v: list[tuple[float, float, float, bool]] = []
            for k in range(num):
                vx, vy, vz, raw_w = struct.unpack_from("<fffI", payload, k * 16)
                if not (math.isfinite(vx) and math.isfinite(vy) and math.isfinite(vz)):
                    plausible = False
                    break
                if max(abs(vx), abs(vy), abs(vz)) > 1e6:
                    plausible = False
                    break
                adc = bool(raw_w & 0x8000) or (raw_w == 0x8000)
                cand_v.append((vx, vy, vz, adc))
            if plausible:
                if batch_verts:
                    flush_batch()
                batch_verts = cand_v
                pos += 4 + num * 16
                continue

        # UNPACK V3-32 (0x68)
        if (op & 0x6F) == 0x68 and 3 <= num <= 128 and pos + 4 + num * 12 <= n:
            payload = vif_bytes[pos + 4 : pos + 4 + num * 12]
            plausible = True
            cand_3: list[tuple[float, float, float]] = []
            for k in range(num):
                x, y, z = struct.unpack_from("<fff", payload, k * 12)
                if not (math.isfinite(x) and math.isfinite(y) and math.isfinite(z)) or max(abs(x), abs(y), abs(z)) > 1e6:
                    plausible = False
                    break
                cand_3.append((x, y, z))
            if plausible:
                if not batch_verts:
                    batch_verts = [(x, y, z, False) for x, y, z in cand_3]
                elif len(cand_3) == len(batch_verts) and not batch_nrms:
                    batch_nrms = cand_3
                pos += 4 + num * 12
                continue

        # UNPACK V2-16 (0x65) fixed-point UVs (1/4096.0f)
        if (op & 0x6F) == 0x65 and 3 <= num <= 128 and pos + 4 + num * 4 <= n:
            if batch_verts and num == len(batch_verts) and not batch_uvs:
                payload = vif_bytes[pos + 4 : pos + 4 + num * 4]
                for k in range(num):
                    su, sv = struct.unpack_from("<hh", payload, k * 4)
                    batch_uvs.append((su / 4096.0, 1.0 - (sv / 4096.0)))
                pos += 4 + num * 4
                continue

        pos += 4

    flush_batch()
    return [sub] if len(sub.vertices) >= 3 and len(sub.faces) >= 1 else []


def export_meshes_to_obj(
    meshes: list[Ps2SubMesh],
    header_comment: str = "PS2 Disc Toolkit - Extracted 3D Mesh",
    mtllib_name: str | None = None,
) -> str:
    """
    Serialize one or more Ps2SubMesh objects into a standard Wavefront .OBJ file string.
    Preserves UVs, normals, and per-vertex RGB shading when present.
    """
    lines = [f"# {header_comment}"]
    if mtllib_name:
        lines.append(f"mtllib {mtllib_name}")

    v_offset = 0
    vt_offset = 0
    vn_offset = 0

    for sub in meshes:
        n_verts = len(sub.vertices)
        if n_verts < 3 or not sub.faces:
            continue
        obj_name = sub.name or f"part{sub.index:02d}"
        lines.append(f"o {obj_name}")

        has_colors = len(sub.colors) == n_verts
        has_uv = len(sub.uvs) == n_verts
        has_nrm = len(sub.normals) == n_verts

        for idx, (vx, vy, vz) in enumerate(sub.vertices):
            if has_colors:
                r, g, b, _a = sub.colors[idx]
                lines.append(f"v {vx:.4f} {vy:.4f} {vz:.4f} {r:.4f} {g:.4f} {b:.4f}")
            else:
                lines.append(f"v {vx:.4f} {vy:.4f} {vz:.4f}")

        if has_uv:
            for u, v in sub.uvs:
                lines.append(f"vt {u:.4f} {v:.4f}")

        if has_nrm:
            for nx, ny, nz in sub.normals:
                lines.append(f"vn {nx:.4f} {ny:.4f} {nz:.4f}")

        for i0, i1, i2 in sub.faces:
            if 0 <= i0 < n_verts and 0 <= i1 < n_verts and 0 <= i2 < n_verts:
                v0, v1, v2 = v_offset + i0 + 1, v_offset + i1 + 1, v_offset + i2 + 1
                if has_uv and has_nrm:
                    t0, t1, t2 = vt_offset + i0 + 1, vt_offset + i1 + 1, vt_offset + i2 + 1
                    n0, n1, n2 = vn_offset + i0 + 1, vn_offset + i1 + 1, vn_offset + i2 + 1
                    lines.append(f"f {v0}/{t0}/{n0} {v1}/{t1}/{n1} {v2}/{t2}/{n2}")
                elif has_uv:
                    t0, t1, t2 = vt_offset + i0 + 1, vt_offset + i1 + 1, vt_offset + i2 + 1
                    lines.append(f"f {v0}/{t0} {v1}/{t1} {v2}/{t2}")
                elif has_nrm:
                    n0, n1, n2 = vn_offset + i0 + 1, vn_offset + i1 + 1, vn_offset + i2 + 1
                    lines.append(f"f {v0}//{n0} {v1}//{n1} {v2}//{n2}")
                else:
                    lines.append(f"f {v0} {v1} {v2}")

        v_offset += n_verts
        if has_uv:
            vt_offset += n_verts
        if has_nrm:
            vn_offset += n_verts

    lines.append("")
    return "\n".join(lines)


def export_mesh_to_obj(sub: Ps2SubMesh, header_comment: str = "PS2 Disc Toolkit - Extracted SubMesh") -> str:
    """Export a single Ps2SubMesh to Wavefront .OBJ format."""
    return export_meshes_to_obj([sub], header_comment=header_comment)


def parse_obj_to_submeshes(obj_text: str) -> list[Ps2SubMesh]:
    """
    Parse a Wavefront .OBJ string back into a list of Ps2SubMesh objects.
    Handles multiple `o`/`g` submeshes, arbitrary polygon fan-triangulation,
    negative or positive 1-based indices, UVs, normals, and optional per-vertex RGB colors.
    """
    global_v: list[tuple[float, float, float]] = []
    global_vc: list[tuple[float, float, float, float] | None] = []
    global_vt: list[tuple[float, float]] = []
    global_vn: list[tuple[float, float, float]] = []

    raw_groups: list[tuple[str, list[list[tuple[int, int, int]]]]] = []
    current_name = "part00"
    current_faces: list[list[tuple[int, int, int]]] = []

    for raw_line in obj_text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        cmd = parts[0]
        if cmd in ("o", "g") and len(parts) >= 2:
            if current_faces:
                raw_groups.append((current_name, current_faces))
                current_faces = []
            current_name = parts[1]
        elif cmd == "v" and len(parts) >= 4:
            vx, vy, vz = float(parts[1]), float(parts[2]), float(parts[3])
            global_v.append((vx, vy, vz))
            if len(parts) >= 7:
                r, g, b = float(parts[4]), float(parts[5]), float(parts[6])
                a = float(parts[7]) if len(parts) >= 8 else 1.0
                global_vc.append((r, g, b, a))
            else:
                global_vc.append(None)
        elif cmd == "vt" and len(parts) >= 3:
            global_vt.append((float(parts[1]), float(parts[2])))
        elif cmd == "vn" and len(parts) >= 4:
            global_vn.append((float(parts[1]), float(parts[2]), float(parts[3])))
        elif cmd == "f" and len(parts) >= 4:
            poly: list[tuple[int, int, int]] = []
            for corner in parts[1:]:
                 segs = corner.split("/")
                 vi = int(segs[0])
                 vti = int(segs[1]) if len(segs) > 1 and segs[1] else 0
                 vni = int(segs[2]) if len(segs) > 2 and segs[2] else 0
                 if vi < 0:
                     vi = len(global_v) + 1 + vi
                 if vti < 0:
                     vti = len(global_vt) + 1 + vti
                 if vni < 0:
                     vni = len(global_vn) + 1 + vni
                 poly.append((vi - 1, vti - 1, vni - 1))
            for k in range(2, len(poly)):
                current_faces.append([poly[0], poly[k - 1], poly[k]])

    if current_faces:
        raw_groups.append((current_name, current_faces))

    submeshes: list[Ps2SubMesh] = []
    for idx, (g_name, tris) in enumerate(raw_groups):
        sub = Ps2SubMesh(index=idx, name=g_name)
        vert_map: dict[tuple[int, int, int], int] = {}
        has_any_color = False

        for tri in tris:
            tri_indices: list[int] = []
            for vi, vti, vni in tri:
                key = (vi, vti, vni)
                new_idx = vert_map.get(key)
                if new_idx is None:
                    new_idx = len(sub.vertices)
                    vert_map[key] = new_idx
                    vx, vy, vz = global_v[vi] if 0 <= vi < len(global_v) else (0.0, 0.0, 0.0)
                    sub.vertices.append((vx, vy, vz))
                    if 0 <= vti < len(global_vt):
                        sub.uvs.append(global_vt[vti])
                    else:
                        sub.uvs.append((0.0, 0.0))
                    if 0 <= vni < len(global_vn):
                        sub.normals.append(global_vn[vni])
                    else:
                        sub.normals.append((0.0, 1.0, 0.0))
                    vc = global_vc[vi] if 0 <= vi < len(global_vc) else None
                    if vc is not None:
                        has_any_color = True
                        sub.colors.append(vc)
                    else:
                        sub.colors.append((1.0, 1.0, 1.0, 1.0))
                tri_indices.append(new_idx)
            if (
                tri_indices[0] != tri_indices[1]
                and tri_indices[1] != tri_indices[2]
                and tri_indices[0] != tri_indices[2]
            ):
                sub.faces.append((tri_indices[0], tri_indices[1], tri_indices[2]))

        if not has_any_color:
            sub.colors.clear()
        if sub.vertices and sub.faces:
            submeshes.append(sub)

    return submeshes


def _extract_fmod_skinning_records(
    vert_payload: bytes,
    vert_count: int,
    skin_payload: bytes,
    skin_count: int,
) -> list[tuple[tuple[float, float, float], bytes]]:
    """
    Extract (position_xyz, raw_skin_record_bytes) pairs from an original FMOD submesh's
    0x00070000 vertex chunk and 0x000C0000 variable-length bone weight chunk.
    """
    if vert_count <= 0 or vert_count != skin_count or len(vert_payload) < vert_count * 12:
        return []
    records: list[tuple[tuple[float, float, float], bytes]] = []
    s_pos = 0
    for i in range(vert_count):
        if s_pos + 4 > len(skin_payload):
            return []
        num_bones = struct.unpack_from("<I", skin_payload, s_pos)[0]
        rec_len = 4 + num_bones * 8
        if num_bones > 16 or s_pos + rec_len > len(skin_payload):
            return []
        vx, vy, vz = struct.unpack_from("<fff", vert_payload, i * 12)
        records.append(((vx, vy, vz), skin_payload[s_pos : s_pos + rec_len]))
        s_pos += rec_len
    return records


def _rebuild_fmod_submesh_block(orig_sub_block: bytes, new_sub: Ps2SubMesh) -> bytes:
    """
    Rebuild a single 0x00000004 SubMesh block using the geometry from `new_sub`,
    while preserving materials (0x00050000), metadata (0x000F0000 / 0x00110000)
    and transferring nearest-vertex skeletal skinning weights (0x000C0000).
    """
    btype, orig_child_count, bsize = struct.unpack_from("<IiI", orig_sub_block, 0)
    if btype != 0x00000004 or bsize < 12:
        return orig_sub_block

    # First pass: inspect original children to extract reference vertices, skinning, and strip type
    pos = 12
    orig_vert_payload = b""
    orig_vert_count = 0
    orig_skin_payload = b""
    orig_skin_count = 0
    orig_default_mat_idx = 0
    orig_strip_btype = 0x00040000
    children_meta: list[tuple[int, int, bytes]] = []

    while pos + 12 <= bsize:
        ct, cc, cs = struct.unpack_from("<IiI", orig_sub_block, pos)
        if cs < 12 or pos + cs > bsize:
            break
        cpayload = orig_sub_block[pos + 12 : pos + cs]
        children_meta.append((ct, cc, cpayload))
        if ct == 0x00000005 and len(cpayload) >= 12:
            st = struct.unpack_from("<I", cpayload, 0)[0]
            if st in (0x00030000, 0x00040000):
                orig_strip_btype = st
        elif ct == 0x00060000 and len(cpayload) >= 4:
            orig_default_mat_idx = struct.unpack_from("<I", cpayload, 0)[0]
        elif ct == 0x00070000 and len(cpayload) == cc * 12:
            orig_vert_payload = cpayload
            orig_vert_count = cc
        elif ct == 0x000C0000:
            orig_skin_payload = cpayload
            orig_skin_count = cc
        pos += cs

    skin_refs = _extract_fmod_skinning_records(
        orig_vert_payload, orig_vert_count, orig_skin_payload, orig_skin_count
    )

    n_verts = len(new_sub.vertices)
    n_faces = len(new_sub.faces)

    # Second pass: rebuild each child chunk in its original order
    new_children = bytearray()
    for ct, cc, cpayload in children_meta:
        if ct == 0x00000005:
            # Pack each triangle as a 3-index strip inside a single strip chunk
            strip_words: list[int] = []
            for i0, i1, i2 in new_sub.faces:
                strip_words.extend((3, i0, i1, i2))
            strip_body = struct.pack(f"<{len(strip_words)}I", *strip_words)
            strip_chunk = struct.pack("<IiI", orig_strip_btype, n_faces, 12 + len(strip_body)) + strip_body
            new_children.extend(struct.pack("<IiI", 0x00000005, 1, 12 + len(strip_chunk)))
            new_children.extend(strip_chunk)

        elif ct == 0x00060000:
            # Per-strip material table (1 uint32 per triangle strip)
            mat_body = struct.pack(f"<{n_faces}I", *([orig_default_mat_idx] * n_faces))
            new_children.extend(struct.pack("<IiI", 0x00060000, n_faces, 12 + len(mat_body)))
            new_children.extend(mat_body)

        elif ct == 0x00070000 and len(cpayload) == cc * 12:
            # Float32x3 vertices
            v_floats: list[float] = []
            for vx, vy, vz in new_sub.vertices:
                v_floats.extend((vx, vy, vz))
            v_body = struct.pack(f"<{len(v_floats)}f", *v_floats)
            new_children.extend(struct.pack("<IiI", 0x00070000, n_verts, 12 + len(v_body)))
            new_children.extend(v_body)

        elif ct == 0x00080000 and len(cpayload) == cc * 12:
            # Float32x3 normals
            n_floats: list[float] = []
            for idx in range(n_verts):
                nx, ny, nz = new_sub.normals[idx] if idx < len(new_sub.normals) else (0.0, 1.0, 0.0)
                n_floats.extend((nx, ny, nz))
            n_body = struct.pack(f"<{len(n_floats)}f", *n_floats)
            new_children.extend(struct.pack("<IiI", 0x00080000, n_verts, 12 + len(n_body)))
            new_children.extend(n_body)

        elif ct == 0x000A0000 and len(cpayload) == cc * 8:
            # Float32x2 UVs (flip V back to PS2 1.0 - v convention)
            uv_floats: list[float] = []
            for idx in range(n_verts):
                u, v = new_sub.uvs[idx] if idx < len(new_sub.uvs) else (0.0, 0.0)
                uv_floats.extend((u, 1.0 - v))
            uv_body = struct.pack(f"<{len(uv_floats)}f", *uv_floats)
            new_children.extend(struct.pack("<IiI", 0x000A0000, n_verts, 12 + len(uv_body)))
            new_children.extend(uv_body)

        elif ct == 0x000B0000 and len(cpayload) == cc * 16:
            # Float32x4 RGBA vertex colors (0..255.0f range)
            c_floats: list[float] = []
            for idx in range(n_verts):
                if idx < len(new_sub.colors):
                    r, g, b, a = new_sub.colors[idx]
                    c_floats.extend((r * 255.0, g * 255.0, b * 255.0, a * 255.0))
                else:
                    c_floats.extend((255.0, 255.0, 255.0, 255.0))
            c_body = struct.pack(f"<{len(c_floats)}f", *c_floats)
            new_children.extend(struct.pack("<IiI", 0x000B0000, n_verts, 12 + len(c_body)))
            new_children.extend(c_body)

        elif ct == 0x000C0000 and skin_refs:
            # Transfer nearest-vertex skeletal skinning weights for each new vertex
            skin_body = bytearray()
            for vx, vy, vz in new_sub.vertices:
                best_rec = skin_refs[0][1]
                best_dist = float("inf")
                for (rx, ry, rz), rec_bytes in skin_refs:
                    d = (vx - rx) * (vx - rx) + (vy - ry) * (vy - ry) + (vz - rz) * (vz - rz)
                    if d < best_dist:
                        best_dist = d
                        best_rec = rec_bytes
                skin_body.extend(best_rec)
            new_children.extend(struct.pack("<IiI", 0x000C0000, n_verts, 12 + len(skin_body)))
            new_children.extend(skin_body)

        else:
            # Preserve material IDs (0x00050000), bounding boxes (0x000F0000, 0x00110000), etc.
            new_children.extend(struct.pack("<IiI", ct, cc, 12 + len(cpayload)))
            new_children.extend(cpayload)

    return struct.pack("<IiI", 0x00000004, orig_child_count, 12 + len(new_children)) + bytes(new_children)


def encode_obj_to_fmod(obj_text: str, reference_fmod: bytes) -> bytes:
    """
    Encode a modified Wavefront .OBJ mesh back into a PS2 .FMOD binary buffer,
    using `reference_fmod` as template for materials, texture indices, and
    nearest-vertex skeletal skinning weights (0x000C0000).
    """
    new_submeshes = parse_obj_to_submeshes(obj_text)
    if not new_submeshes or len(reference_fmod) < 24:
        return reference_fmod

    sub_cursor = 0

    def rebuild_container(buf: bytes) -> bytes:
        nonlocal sub_cursor
        pos = 0
        n = len(buf)
        out = bytearray()
        while pos + 12 <= n:
            btype, count, bsize = struct.unpack_from("<IiI", buf, pos)
            if bsize < 12 or pos + bsize > n:
                out.extend(buf[pos:])
                break
            raw_block = buf[pos : pos + bsize]
            if btype == 0x00000004:
                if sub_cursor < len(new_submeshes):
                    out.extend(_rebuild_fmod_submesh_block(raw_block, new_submeshes[sub_cursor]))
                elif new_submeshes:
                    # If the modder merged submeshes into fewer objects in Blender, collapse remaining into degenerate 1-tri stub
                    stub = Ps2SubMesh(
                        index=sub_cursor,
                        vertices=[(0.0, 0.0, 0.0)] * 3,
                        normals=[(0.0, 1.0, 0.0)] * 3,
                        uvs=[(0.0, 0.0)] * 3,
                        faces=[(0, 1, 2)],
                    )
                    out.extend(_rebuild_fmod_submesh_block(raw_block, stub))
                else:
                    out.extend(raw_block)
                sub_cursor += 1
            elif btype in (0x00000001, 0x00000002, 0x00000003):
                rebuilt_payload = rebuild_container(buf[pos + 12 : pos + bsize])
                out.extend(struct.pack("<IiI", btype, count, 12 + len(rebuilt_payload)))
                out.extend(rebuilt_payload)
            else:
                out.extend(raw_block)
            pos += bsize
        return bytes(out)

    if reference_fmod[:4] == b"FMOD":
        for cand in (4, 8, 12, 16):
            if cand + 12 <= len(reference_fmod):
                bt, _cnt, bsz = struct.unpack_from("<IiI", reference_fmod, cand)
                if bt in (1, 2, 4) and 12 <= bsz <= len(reference_fmod) - cand:
                    return reference_fmod[:cand] + rebuild_container(reference_fmod[cand:])

    return rebuild_container(reference_fmod)

