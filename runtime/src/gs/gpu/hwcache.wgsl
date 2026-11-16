// Alpha range of decoded textures: the smallest and largest alpha of each entry of the cache, so that the draw layer can cancel an
// alpha test that cannot fail or cannot pass. One workgroup row per entry; `decoded` holds one 32-bit word per texel with the alpha in the top byte, in the units
// the decode pass writes (0x80 is the chip's 1.0).
struct Entry {
    offset: u32,
    words: u32,
};

@group(0) @binding(0) var<storage, read> decoded: array<u32>;
@group(0) @binding(1) var<storage, read> table: array<Entry>;
@group(0) @binding(2) var<storage, read_write> ranges: array<atomic<u32>>;

@compute @workgroup_size(64)
fn alpha_range(@builtin(workgroup_id) wg: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
    let e = table[wg.y];
    var lo = 255u;
    var hi = 0u;
    var i = wg.x * 64u + lid.x;
    let stride = nwg.x * 64u;
    loop {
        if (i >= e.words) { break; }
        let a = decoded[e.offset + i] >> 24u;
        lo = min(lo, a);
        hi = max(hi, a);
        i += stride;
    }
    atomicMin(&ranges[wg.y * 2u], lo);
    atomicMax(&ranges[wg.y * 2u + 1u], hi);
}
