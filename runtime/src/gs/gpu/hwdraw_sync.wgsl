
// ---- Hardware drawing trial: appended to gs.wgsl, so it shares its memory layout code (addr, read_frame, write_frame).
// hw_load copies a rectangle of a frame buffer from graphics memory (swizzled) into the render target texture, hw_store copies it back.
// Parameters: prm(0) = word offset of a setup whose frame format is the target's, prm(1), prm(2) = x, y of the rectangle, prm(3), prm(4) = size.

@group(1) @binding(0) var color_out: texture_storage_2d<rgba8unorm, write>;
@group(1) @binding(0) var color_in: texture_2d<f32>;
@group(1) @binding(0) var depth_in: texture_depth_2d;

@compute @workgroup_size(8, 8)
fn hw_load(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= prm(3u) || gid.y >= prm(4u)) {
        return;
    }
    let x = prm(1u) + gid.x;
    let y = prm(2u) + gid.y;
    var s: Setup;
    s.fpsm = data[prm(0u) + 3u];
    let c = read_frame(s, addr(fmt_at(prm(0u) + 30u), x, y), 0u);
    textureStore(color_out, vec2<i32>(i32(x), i32(y)), vec4<f32>(vec4<u32>(c & 0xFFu, (c >> 8u) & 0xFFu, (c >> 16u) & 0xFFu, c >> 24u)) / 255.0);
}

@compute @workgroup_size(8, 8)
fn hw_store(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= prm(3u) || gid.y >= prm(4u)) {
        return;
    }
    let x = prm(1u) + gid.x;
    let y = prm(2u) + gid.y;
    var s: Setup;
    s.fpsm = data[prm(0u) + 3u];
    let t = textureLoad(color_in, vec2<i32>(i32(x), i32(y)), 0);
    let o = vec4<u32>(clamp(t * 255.0 + vec4<f32>(0.5), vec4<f32>(0.0), vec4<f32>(255.0)));
    write_frame(s, addr(fmt_at(prm(0u) + 30u), x, y), 0u, o.x | (o.y << 8u) | (o.z << 16u) | (o.w << 24u), 0u);
}

// hw_store_z writes the render target's depth back into the depth buffer's memory (24-bit depth in the low bits of each word, the top byte stays).
@compute @workgroup_size(8, 8)
fn hw_store_z(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= prm(3u) || gid.y >= prm(4u)) {
        return;
    }
    let x = prm(1u) + gid.x;
    let y = prm(2u) + gid.y;
    let d = textureLoad(depth_in, vec2<i32>(i32(x), i32(y)), 0);
    store(addr(fmt_at(prm(0u) + 35u), x, y), 0u, u32(clamp(d * 16777216.0, 0.0, 16777215.0)));
}

// hw_display: the display kernel of gs.wgsl, reading a target's picture instead of graphics memory. Same parameters as `display` (prm 0 to 8);
// only for 32-bit and 24-bit pictures without the second read circuit. The scale is 1 on this path.
@compute @workgroup_size(8, 8)
fn hw_display(@builtin(global_invocation_id) gid: vec3<u32>) {
    let w = prm(5u);
    let h = prm(6u);
    if (gid.x >= w || gid.y >= h) {
        return;
    }
    var sy = gid.y;
    if (prm(7u) != 0u) {
        sy = gid.y / 2u;
    }
    let t = textureLoad(color_in, vec2<i32>(i32(prm(3u) + gid.x), i32(prm(4u) + sy)), 0);
    let o = vec4<u32>(clamp(t * 255.0 + vec4<f32>(0.5), vec4<f32>(0.0), vec4<f32>(255.0)));
    scratch[prm(8u) + gid.y * w + gid.x] = o.x | (o.y << 8u) | (o.z << 16u) | 0xFF000000u;
}
