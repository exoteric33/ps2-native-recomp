// hwprobe: stand-alone measurements for the hardware drawing plan (rows 1 to 3 below).
// It does not use the runtime. It opens a wgpu-native device by itself and prints one "[probe]" line per measured answer.
//
//   ps2hwprobe [--backend vulkan|dx12] [--rows 1,2,3] [--reps N] [--shapes N] [--side PIXELS] [--order up|random]
//
// Row 1: dual-source blending, primitive index, stencil formats, a vertex shader that reads a storage buffer: is it there and is the result right.
// Row 2: what ending a render pass and copying a box of the target costs (the cost of "draws that read what they write").
// Row 3: depth written from the fragment shader against the card's own depth test, for a draw shaped like the trial's.
// All timings are wall clock around submit and queue-done (the Intel driver has no timestamps); best and median of N repetitions, run in turn.
#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

WGPUStringView sv(const char *s) { return WGPUStringView{s, WGPU_STRLEN}; }

WGPUInstance g_instance;
WGPUAdapter g_adapter;
WGPUDevice g_device;
WGPUQueue g_queue;
int g_errors = 0;
std::string g_last_error;
bool g_has_dual, g_has_prim, g_has_d32s8, g_has_ts;

void pump() { wgpuDevicePoll(g_device, true, nullptr); }
void wait_until(const bool &flag) {
    while (!flag) pump();
}
void on_error(WGPUDevice const *, WGPUErrorType, WGPUStringView message, void *, void *) {
    if (g_errors++ == 0) g_last_error.assign(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length);  // keep the first (the shader's own)
}
void wait_gpu() {
    bool done = false;
    WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
    cb.userdata1 = &done;
    wgpuQueueOnSubmittedWorkDone(g_queue, cb);
    wait_until(done);
}
void submit(WGPUCommandEncoder enc) {
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
}

WGPUBuffer make_buffer(std::uint64_t size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = size;
    d.usage = usage;
    return wgpuDeviceCreateBuffer(g_device, &d);
}
WGPUTexture make_texture(std::uint32_t w, std::uint32_t h, WGPUTextureFormat fmt, WGPUTextureUsage usage) {
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.size = {w, h, 1};
    d.format = fmt;
    d.usage = usage;
    return wgpuDeviceCreateTexture(g_device, &d);
}
WGPUShaderModule make_module(const std::string &code) {
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = WGPUStringView{code.data(), code.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &src.chain;
    return wgpuDeviceCreateShaderModule(g_device, &md);
}

// copy a w x h texture to the processor, 4 bytes per pixel (RGBA8 or R32), rows packed
std::vector<std::uint8_t> read_texture(WGPUTexture tex, std::uint32_t w, std::uint32_t h) {
    const std::uint32_t row = (w * 4 + 255) / 256 * 256;
    WGPUBuffer buf = make_buffer(static_cast<std::uint64_t>(row) * h, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = tex;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buf;
    dst.layout.bytesPerRow = row;
    dst.layout.rowsPerImage = h;
    const WGPUExtent3D ext = {w, h, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    submit(enc);
    bool done = false;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus, WGPUStringView, void *flag, void *) { *static_cast<bool *>(flag) = true; };
    cb.userdata1 = &done;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, static_cast<std::uint64_t>(row) * h, cb);
    wait_until(done);
    const auto *mapped = static_cast<const std::uint8_t *>(wgpuBufferGetConstMappedRange(buf, 0, static_cast<std::uint64_t>(row) * h));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) std::memcpy(out.data() + static_cast<std::size_t>(y) * w * 4, mapped + static_cast<std::size_t>(y) * row, w * 4);
    wgpuBufferUnmap(buf);
    wgpuBufferRelease(buf);
    return out;
}

// One render pass on `target` (and optional depth view): clear (or load), set a pipeline, draw `vertices`, end.
struct Pass {
    WGPUTextureView color = nullptr, depth = nullptr;
    bool clear = true;
    WGPUColor clear_color = {0, 0, 0, 0};
    bool clear_depth = true;
    bool has_stencil = false;
};
WGPURenderPassEncoder begin_pass(WGPUCommandEncoder enc, const Pass &p) {
    WGPURenderPassColorAttachment c = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    c.view = p.color;
    c.loadOp = p.clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
    c.storeOp = WGPUStoreOp_Store;
    c.clearValue = p.clear_color;
    WGPURenderPassDepthStencilAttachment d = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassDescriptor rd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rd.colorAttachmentCount = p.color ? 1 : 0;
    rd.colorAttachments = &c;
    if (p.depth) {
        d.view = p.depth;
        d.depthLoadOp = p.clear_depth ? WGPULoadOp_Clear : WGPULoadOp_Load;
        d.depthStoreOp = WGPUStoreOp_Store;
        d.depthClearValue = 0.0f;
        if (p.has_stencil) {
            d.stencilLoadOp = p.clear_depth ? WGPULoadOp_Clear : WGPULoadOp_Load;
            d.stencilStoreOp = WGPUStoreOp_Store;
            d.stencilClearValue = 0;
        }
        rd.depthStencilAttachment = &d;
    }
    return wgpuCommandEncoderBeginRenderPass(enc, &rd);
}

WGPUTextureView view_of(WGPUTexture t) { return wgpuTextureCreateView(t, nullptr); }

// ---------- opening ----------
bool open_device(const std::string &backend) {
    WGPUInstanceExtras extras = {};
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    extras.backends = backend == "vulkan" ? WGPUInstanceBackend_Vulkan : backend == "dx12" ? WGPUInstanceBackend_DX12 : WGPUInstanceBackend_Primary;
    extras.dx12ShaderCompiler = WGPUDx12Compiler_Fxc;
    WGPUInstanceDescriptor id = WGPU_INSTANCE_DESCRIPTOR_INIT;
    id.nextInChain = &extras.chain;
    g_instance = wgpuCreateInstance(&id);
    bool done = false;
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = [](WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView, void *out, void *flag) {
        if (s == WGPURequestAdapterStatus_Success) *static_cast<WGPUAdapter *>(out) = a;
        *static_cast<bool *>(flag) = true;
    };
    acb.userdata1 = &g_adapter;
    acb.userdata2 = &done;
    wgpuInstanceRequestAdapter(g_instance, &options, acb);
    while (!done) wgpuInstanceProcessEvents(g_instance);
    if (!g_adapter) return false;
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    wgpuAdapterGetInfo(g_adapter, &info);
    const char *api = info.backendType == WGPUBackendType_Vulkan ? "Vulkan" : info.backendType == WGPUBackendType_D3D12 ? "Direct3D 12" : info.backendType == WGPUBackendType_Metal ? "Metal" : "other";
    std::printf("[probe] machine: %.*s on %s (adapter type %d)\n", static_cast<int>(info.device.length), info.device.data, api, static_cast<int>(info.adapterType));
    wgpuAdapterInfoFreeMembers(info);
    g_has_dual = wgpuAdapterHasFeature(g_adapter, WGPUFeatureName_DualSourceBlending);
    g_has_prim = wgpuAdapterHasFeature(g_adapter, WGPUFeatureName_PrimitiveIndex);
    g_has_d32s8 = wgpuAdapterHasFeature(g_adapter, WGPUFeatureName_Depth32FloatStencil8);
    g_has_ts = wgpuAdapterHasFeature(g_adapter, WGPUFeatureName_TimestampQuery);
    WGPULimits supported = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(g_adapter, &supported);
    std::printf("[probe] adapter offers: DualSourceBlending %s, PrimitiveIndex %s, Depth32FloatStencil8 %s, TimestampQuery %s; maxStorageBuffersPerShaderStage %u\n", g_has_dual ? "yes" : "NO",
                g_has_prim ? "yes" : "NO", g_has_d32s8 ? "yes" : "NO", g_has_ts ? "yes" : "NO", supported.maxStorageBuffersPerShaderStage);
    std::vector<WGPUFeatureName> wanted;
    if (g_has_dual) wanted.push_back(WGPUFeatureName_DualSourceBlending);
    if (g_has_prim) wanted.push_back(WGPUFeatureName_PrimitiveIndex);
    if (g_has_d32s8) wanted.push_back(WGPUFeatureName_Depth32FloatStencil8);
    WGPULimits limits = WGPU_LIMITS_INIT;
    limits.maxStorageBuffersPerShaderStage = supported.maxStorageBuffersPerShaderStage;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredLimits = &limits;
    dd.requiredFeatureCount = wanted.size();
    dd.requiredFeatures = wanted.data();
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    done = false;
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = WGPUCallbackMode_AllowSpontaneous;
    dcb.callback = [](WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView m, void *out, void *flag) {
        if (s == WGPURequestDeviceStatus_Success) *static_cast<WGPUDevice *>(out) = d;
        else std::printf("[probe] device request failed: %.*s\n", static_cast<int>(m.length), m.data);
        *static_cast<bool *>(flag) = true;
    };
    dcb.userdata1 = &g_device;
    dcb.userdata2 = &done;
    wgpuAdapterRequestDevice(g_adapter, &dd, dcb);
    while (!done) wgpuInstanceProcessEvents(g_instance);
    if (!g_device) return false;
    g_queue = wgpuDeviceGetQueue(g_device);
    return true;
}

// ---------- row 1 ----------
struct Pipe {
    WGPURenderPipeline pipe = nullptr;
    WGPUBindGroupLayout layout = nullptr;
};

// A pipeline with the given entry points, one colour target and an optional depth-stencil state; returns null when creation made an error.
WGPURenderPipeline make_pipeline(const std::string &wgsl, WGPUTextureFormat color_format, const WGPUBlendState *blend, const WGPUDepthStencilState *ds, WGPUPipelineLayout layout, WGPUColorWriteMask mask = WGPUColorWriteMask_All,
                                 const WGPUVertexBufferLayout *vb = nullptr, const char *vs = "vs", const char *fs = "fs") {
    g_errors = 0;
    WGPUShaderModule module = make_module(wgsl);
    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = color_format;
    color.blend = blend;
    color.writeMask = mask;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = sv(fs);
    fragment.targetCount = color_format == WGPUTextureFormat_Undefined ? 0 : 1;
    fragment.targets = &color;
    WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rd.layout = layout;
    rd.vertex.module = module;
    rd.vertex.entryPoint = sv(vs);
    if (vb) rd.vertex.bufferCount = 1, rd.vertex.buffers = vb;
    rd.fragment = &fragment;
    rd.depthStencil = ds;
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(g_device, &rd);
    wgpuShaderModuleRelease(module);
    wgpuDevicePoll(g_device, false, nullptr);
    if (g_errors) {
        if (p) wgpuRenderPipelineRelease(p);
        return nullptr;
    }
    return p;
}

WGPUPipelineLayout empty_layout() {
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    return wgpuDeviceCreatePipelineLayout(g_device, &pld);
}

const char *const kQuad = R"(
var<private> P: array<vec2f, 6> = array<vec2f, 6>(vec2f(-1, -1), vec2f(1, -1), vec2f(-1, 1), vec2f(-1, 1), vec2f(1, -1), vec2f(1, 1));
)";

void row1() {
    std::printf("[probe] ---- row 1: features and a one-draw test each ----\n");
    WGPUPipelineLayout layout = empty_layout();
    // 1a dual-source blending: output 0 = colour and the raw alpha, output 1 = blend factor in its alpha. Expected: rgb = src*f + dst*(1-f), alpha = raw.
    for (int variant = 0; variant < 2; ++variant) {
        const float factor = variant == 0 ? 0.25f : 1.5f;  // 1.5: a PS2 factor above 1.0 (the chip allows up to 255/128)
        if (!g_has_dual) {
            std::printf("[probe] dual-source blending (factor %.2f): feature not offered by this adapter\n", factor);
            continue;
        }
        char buf[1200];
        std::snprintf(buf, sizeof buf, R"(struct O { @location(0) @blend_src(0) c: vec4f, @location(0) @blend_src(1) f: vec4f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { return vec4f(P[i], 0, 1); }
@fragment fn fs() -> O { var o: O; o.c = vec4f(0.2, 0.4, 0.8, 0.5); o.f = vec4f(0, 0, 0, %f); return o; }
)", factor);
        WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
        blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_Src1Alpha, WGPUBlendFactor_OneMinusSrc1Alpha};
        blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_Zero};
        WGPURenderPipeline pipe = make_pipeline(std::string("enable dual_source_blending;\n") + kQuad + buf, WGPUTextureFormat_RGBA8Unorm, &blend, nullptr, layout);
        if (!pipe) {
            std::printf("[probe] dual-source blending (factor %.2f): pipeline refused: %s\n", factor, g_last_error.substr(0, 700).c_str());
            continue;
        }
        WGPUTexture t = make_texture(4, 4, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc);
        WGPUTextureView v = view_of(t);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
        Pass p;
        p.color = v;
        p.clear_color = {0.8, 0.4, 0.2, 1.0};  // destination
        WGPURenderPassEncoder rp = begin_pass(enc, p);
        wgpuRenderPassEncoderSetPipeline(rp, pipe);
        wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
        wgpuRenderPassEncoderEnd(rp);
        wgpuRenderPassEncoderRelease(rp);
        submit(enc);
        const auto px = read_texture(t, 4, 4);
        const float s[3] = {0.2f, 0.4f, 0.8f}, d[3] = {0.8f, 0.4f, 0.2f};
        const float f_used_clamped = std::min(factor, 1.0f);
        std::uint8_t expect_unclamped[3], expect_clamped[3];
        for (int i = 0; i < 3; ++i) {
            expect_unclamped[i] = static_cast<std::uint8_t>(std::lround(std::clamp(s[i] * factor + d[i] * (1 - factor), 0.0f, 1.0f) * 255.0f));
            expect_clamped[i] = static_cast<std::uint8_t>(std::lround(std::clamp(s[i] * f_used_clamped + d[i] * (1 - f_used_clamped), 0.0f, 1.0f) * 255.0f));
        }
        const bool match_u = std::abs(px[0] - expect_unclamped[0]) <= 1 && std::abs(px[1] - expect_unclamped[1]) <= 1 && std::abs(px[2] - expect_unclamped[2]) <= 1;
        const bool match_c = std::abs(px[0] - expect_clamped[0]) <= 1 && std::abs(px[1] - expect_clamped[1]) <= 1 && std::abs(px[2] - expect_clamped[2]) <= 1;
        std::printf("[probe] dual-source blending (factor %.2f, raw alpha 0.5 kept): got rgba %u %u %u %u; exact-formula rgb %u %u %u, factor-clamped-to-1 rgb %u %u %u; alpha %s => %s\n", factor, px[0], px[1], px[2], px[3],
                    expect_unclamped[0], expect_unclamped[1], expect_unclamped[2], expect_clamped[0], expect_clamped[1], expect_clamped[2], std::abs(px[3] - 128) <= 1 ? "raw (right)" : "NOT raw",
                    match_u ? "factor used as given (works above 1.0)" : match_c ? "FACTOR CLAMPED to 1.0" : "neither");
        wgpuTextureViewRelease(v);
        wgpuTextureRelease(t);
        wgpuRenderPipelineRelease(pipe);
    }
    // 1b primitive index: two triangles of a quad into an R32Uint target
    if (!g_has_prim) std::printf("[probe] primitive index: feature not offered by this adapter\n");
    else {
        const char *code = R"(@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { return vec4f(P[i], 0, 1); }
@fragment fn fs(@builtin(primitive_index) pi: u32) -> @location(0) u32 { return pi + 10u; }
)";
        WGPURenderPipeline pipe = make_pipeline(std::string("enable primitive_index;\n") + kQuad + code, WGPUTextureFormat_R32Uint, nullptr, nullptr, layout);
        if (!pipe) std::printf("[probe] primitive index: pipeline refused: %s\n", g_last_error.substr(0, 700).c_str());
        else {
            WGPUTexture t = make_texture(8, 8, WGPUTextureFormat_R32Uint, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc);
            WGPUTextureView v = view_of(t);
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
            Pass p;
            p.color = v;
            WGPURenderPassEncoder rp = begin_pass(enc, p);
            wgpuRenderPassEncoderSetPipeline(rp, pipe);
            wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
            submit(enc);
            const auto px = read_texture(t, 8, 8);
            int n10 = 0, n11 = 0, other = 0;
            for (int i = 0; i < 64; ++i) {
                std::uint32_t val;
                std::memcpy(&val, &px[static_cast<std::size_t>(i) * 4], 4);
                (val == 10 ? n10 : val == 11 ? n11 : other)++;
            }
            std::printf("[probe] primitive index: pixels of triangle 0: %d, triangle 1: %d, other: %d (of 64) => %s\n", n10, n11, other, n10 > 0 && n11 > 0 && other == 0 ? "works" : "WRONG");
            wgpuTextureViewRelease(v);
            wgpuTextureRelease(t);
            wgpuRenderPipelineRelease(pipe);
        }
    }
    // 1c stencil: mark the left half, then draw everywhere where the mark is set
    for (int variant = 0; variant < 2; ++variant) {
        const WGPUTextureFormat fmt = variant == 0 ? WGPUTextureFormat_Depth24PlusStencil8 : WGPUTextureFormat_Depth32FloatStencil8;
        const char *name = variant == 0 ? "Depth24PlusStencil8" : "Depth32FloatStencil8";
        if (variant == 1 && !g_has_d32s8) {
            std::printf("[probe] %s: feature not offered by this adapter\n", name);
            continue;
        }
        const char *code = R"(
struct U { half: f32 };
@vertex fn vs_left(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { var p = P[i]; p.x = (p.x - 1.0) * 0.5; return vec4f(p, 0.5, 1); }
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { return vec4f(P[i], 0.5, 1); }
@fragment fn fs() -> @location(0) vec4f { return vec4f(1, 0, 0, 1); }
)";
        WGPUDepthStencilState mark = WGPU_DEPTH_STENCIL_STATE_INIT, use = WGPU_DEPTH_STENCIL_STATE_INIT;
        mark.format = use.format = fmt;
        mark.depthWriteEnabled = use.depthWriteEnabled = WGPUOptionalBool_False;
        mark.depthCompare = use.depthCompare = WGPUCompareFunction_Always;
        mark.stencilFront.compare = mark.stencilBack.compare = WGPUCompareFunction_Always;
        mark.stencilFront.passOp = mark.stencilBack.passOp = WGPUStencilOperation_Replace;
        use.stencilFront.compare = use.stencilBack.compare = WGPUCompareFunction_Equal;
        use.stencilFront.passOp = use.stencilBack.passOp = WGPUStencilOperation_Keep;
        mark.stencilReadMask = mark.stencilWriteMask = use.stencilReadMask = use.stencilWriteMask = 0xFF;
        WGPURenderPipeline pm = make_pipeline(std::string(kQuad) + code, WGPUTextureFormat_RGBA8Unorm, nullptr, &mark, layout, WGPUColorWriteMask_None, nullptr, "vs_left", "fs");
        WGPURenderPipeline pu = pm ? make_pipeline(std::string(kQuad) + code, WGPUTextureFormat_RGBA8Unorm, nullptr, &use, layout) : nullptr;
        if (!pm || !pu) {
            std::printf("[probe] %s stencil test: pipeline refused: %s\n", name, g_last_error.substr(0, 700).c_str());
            continue;
        }
        WGPUTexture t = make_texture(8, 8, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc);
        WGPUTexture z = make_texture(8, 8, fmt, WGPUTextureUsage_RenderAttachment);
        WGPUTextureView tv = view_of(t), zv = view_of(z);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
        Pass p;
        p.color = tv;
        p.depth = zv;
        p.has_stencil = true;
        WGPURenderPassEncoder rp = begin_pass(enc, p);
        wgpuRenderPassEncoderSetPipeline(rp, pm);
        wgpuRenderPassEncoderSetStencilReference(rp, 1);
        wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
        wgpuRenderPassEncoderSetPipeline(rp, pu);
        wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
        wgpuRenderPassEncoderEnd(rp);
        wgpuRenderPassEncoderRelease(rp);
        submit(enc);
        const auto px = read_texture(t, 8, 8);
        int red = 0, clear = 0;
        bool left_ok = true;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const bool r = px[static_cast<std::size_t>(y * 8 + x) * 4] == 255;
                (r ? red : clear)++;
                if (r != (x < 4)) left_ok = false;
            }
        std::printf("[probe] %s stencil test (mark the left half, draw where marked): red pixels %d, untouched %d (of 64) => %s\n", name, red, clear, left_ok ? "works" : "WRONG");
        wgpuTextureViewRelease(tv), wgpuTextureViewRelease(zv), wgpuTextureRelease(t), wgpuTextureRelease(z);
        wgpuRenderPipelineRelease(pm), wgpuRenderPipelineRelease(pu);
    }
    // 1d a vertex shader that reads a storage buffer (sprites expanded on the card)
    {
        WGPUBindGroupLayoutEntry le = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        le.binding = 0;
        le.visibility = WGPUShaderStage_Vertex;
        le.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        ld.entryCount = 1;
        ld.entries = &le;
        WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
        WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl;
        WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(g_device, &pld);
        const char *code = R"(
@group(0) @binding(0) var<storage, read> corners: array<vec4f>;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { return corners[i]; }
@fragment fn fs() -> @location(0) vec4f { return vec4f(0, 1, 0, 1); }
)";
        WGPURenderPipeline pipe = make_pipeline(code, WGPUTextureFormat_RGBA8Unorm, nullptr, nullptr, pl);
        if (!pipe) std::printf("[probe] vertex shader reading a storage buffer: pipeline refused: %s\n", g_last_error.substr(0, 700).c_str());
        else {
            const float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
            WGPUBuffer sb = make_buffer(sizeof tri, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
            wgpuQueueWriteBuffer(g_queue, sb, 0, tri, sizeof tri);
            WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
            be.binding = 0;
            be.buffer = sb;
            be.size = sizeof tri;
            WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bd.layout = bgl;
            bd.entryCount = 1;
            bd.entries = &be;
            WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_device, &bd);
            WGPUTexture t = make_texture(4, 4, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc);
            WGPUTextureView v = view_of(t);
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
            Pass p;
            p.color = v;
            WGPURenderPassEncoder rp = begin_pass(enc, p);
            wgpuRenderPassEncoderSetPipeline(rp, pipe);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, nullptr);
            wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
            submit(enc);
            const auto px = read_texture(t, 4, 4);
            std::printf("[probe] vertex shader reading a storage buffer: pixel (1,1) = %u %u %u %u => %s\n", px[20], px[21], px[22], px[23], px[21] == 255 ? "works" : "WRONG");
        }
    }
    wgpuPipelineLayoutRelease(layout);
}

// ---------- row 2 ----------
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void row2(int reps) {
    std::printf("[probe] ---- row 2: what ending a pass and copying a box costs (frame 640x448 RGBA8; wall clock of encode + submit + wait; best / median of %d) ----\n", reps);
    constexpr std::uint32_t W = 640, H = 448;
    const int ns[5] = {1, 8, 64, 512, 1000};
    const int boxes[3] = {16, 64, 256};
    WGPUBindGroupLayoutEntry le[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    le[0].binding = 0;
    le[0].visibility = WGPUShaderStage_Fragment;
    le[0].texture.sampleType = WGPUTextureSampleType_Float;
    le[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    le[1].binding = 1;
    le[1].visibility = WGPUShaderStage_Vertex;
    le[1].buffer.type = WGPUBufferBindingType_Uniform;
    le[1].buffer.hasDynamicOffset = true;
    le[1].buffer.minBindingSize = 16;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 2;
    ld.entries = le;
    WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &bgl;
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(g_device, &pld);
    const std::string code = std::string(kQuad) + R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var<uniform> box: vec4f;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let u = (P[i] + vec2f(1.0)) * 0.5;
    let px = box.xy + u * box.zw;
    return vec4f(px.x / 640.0 * 2.0 - 1.0, 1.0 - px.y / 448.0 * 2.0, 0.0, 1.0);
}
@fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
    let c = textureLoad(src, vec2i(p.xy), 0);
    return vec4f(c.rgb * 0.5 + vec3f(0.1), 1.0);
}
)";
    WGPURenderPipeline pipe = make_pipeline(code, WGPUTextureFormat_RGBA8Unorm, nullptr, nullptr, pl);
    if (!pipe) {
        std::printf("[probe] row 2: pipeline refused: %s\n", g_last_error.c_str());
        return;
    }
    WGPUTexture target = make_texture(W, H, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding);
    WGPUTexture copy = make_texture(W, H, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding);
    WGPUTextureView tv = view_of(target), cv = view_of(copy);
    constexpr std::uint32_t kStride = 256;
    WGPUBuffer ub = make_buffer(static_cast<std::uint64_t>(kStride) * 1000, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    WGPUBindGroupEntry be[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    be[0].binding = 0;
    be[0].textureView = cv;
    be[1].binding = 1;
    be[1].buffer = ub;
    be[1].size = 16;
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = bgl;
    bd.entryCount = 2;
    bd.entries = be;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_device, &bd);
    std::printf("[probe] row2 columns: groups per frame, box, single pass (N draws, no copies) | N passes, no copies | N passes + N box copies | per group: pass break only, pass break + copy, in microseconds (CPU encode share of the last in brackets)\n");
    for (int box : boxes) {
        for (int n : ns) {
            std::vector<float> boxes_data(static_cast<std::size_t>(kStride) / 4 * n, 0.0f);
            for (int i = 0; i < n; ++i) {
                float *b = &boxes_data[static_cast<std::size_t>(i) * kStride / 4];
                b[0] = static_cast<float>((i * 37) % static_cast<int>(W - box)), b[1] = static_cast<float>((i * 53) % static_cast<int>(H - box)), b[2] = static_cast<float>(box), b[3] = static_cast<float>(box);
            }
            wgpuQueueWriteBuffer(g_queue, ub, 0, boxes_data.data(), boxes_data.size() * 4);
            std::vector<double> t_single, t_pass, t_copy, cpu_copy;
            for (int rep = 0; rep < reps + 2; ++rep) {
                for (int mode = 0; mode < 3; ++mode) {
                    const auto t0 = Clock::now();
                    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
                    Pass p;
                    p.color = tv;
                    p.clear = false;
                    if (mode == 0) {
                        WGPURenderPassEncoder rp = begin_pass(enc, p);
                        wgpuRenderPassEncoderSetPipeline(rp, pipe);
                        for (int i = 0; i < n; ++i) {
                            const std::uint32_t off = static_cast<std::uint32_t>(i) * kStride;
                            wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &off);
                            wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
                        }
                        wgpuRenderPassEncoderEnd(rp);
                        wgpuRenderPassEncoderRelease(rp);
                    } else {
                        for (int i = 0; i < n; ++i) {
                            if (mode == 2) {
                                WGPUTexelCopyTextureInfo s = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, d = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                                s.texture = target;
                                d.texture = copy;
                                const float *b = &boxes_data[static_cast<std::size_t>(i) * kStride / 4];
                                s.origin = d.origin = {static_cast<std::uint32_t>(b[0]), static_cast<std::uint32_t>(b[1]), 0};
                                const WGPUExtent3D ext = {static_cast<std::uint32_t>(box), static_cast<std::uint32_t>(box), 1};
                                wgpuCommandEncoderCopyTextureToTexture(enc, &s, &d, &ext);
                            }
                            WGPURenderPassEncoder rp = begin_pass(enc, p);
                            wgpuRenderPassEncoderSetPipeline(rp, pipe);
                            const std::uint32_t off = static_cast<std::uint32_t>(i) * kStride;
                            wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 1, &off);
                            wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
                            wgpuRenderPassEncoderEnd(rp);
                            wgpuRenderPassEncoderRelease(rp);
                        }
                    }
                    submit(enc);
                    const double cpu = ms_since(t0);
                    wait_gpu();
                    const double total = ms_since(t0);
                    if (rep >= 2) (mode == 0 ? t_single : mode == 1 ? t_pass : t_copy).push_back(total), mode == 2 ? cpu_copy.push_back(cpu) : (void)0;
                }
            }
            const double bs = *std::min_element(t_single.begin(), t_single.end()), bp = *std::min_element(t_pass.begin(), t_pass.end()), bc = *std::min_element(t_copy.begin(), t_copy.end());
            const double ms_ = median(t_single), mp = median(t_pass), mc = median(t_copy);
            std::printf("[probe] row2 N %4d box %3d: single pass %7.3f / %7.3f ms | passes %7.3f / %7.3f ms | passes+copies %7.3f / %7.3f ms | per group: break %6.1f us, break+copy %6.1f us (cpu %5.1f)\n", n, box, bs, ms_, bp, mp, bc, mc,
                        (bp - bs) / n * 1000.0, (bc - bs) / n * 1000.0, median(cpu_copy) / n * 1000.0);
        }
    }
}

// ---------- row 3 ----------
void row3(int reps, int shapes, float side, bool ascending) {
    std::printf("[probe] ---- row 3: depth written from the fragment shader against the card's depth test (%d sprites of about %.0f px, z %s, frame 640x448, depth32float; best / median of %d) ----\n", shapes, side,
                ascending ? "ascending (every sprite passes the test)" : "random", reps);
    constexpr std::uint32_t W = 640, H = 448;
    struct V {
        float x, y, z, u, v;
    };
    std::vector<V> verts;
    verts.reserve(static_cast<std::size_t>(shapes) * 6);
    std::uint32_t rng = 12345;
    const auto rnd = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    };
    double covered = 0;
    for (int i = 0; i < shapes; ++i) {
        const float w = side * (0.3f + 1.4f * rnd()), h = side * (0.3f + 1.4f * rnd());
        const float x = rnd() * (W - w), y = rnd() * (H - h);
        const float z = ascending ? (static_cast<float>(i) + 0.5f) / static_cast<float>(shapes) : rnd();
        const float u0 = rnd() * 0.5f, v0 = rnd() * 0.5f;
        covered += static_cast<double>(w) * h;
        const V q[4] = {{x, y, z, u0, v0}, {x + w, y, z, u0 + 0.5f, v0}, {x, y + h, z, u0, v0 + 0.5f}, {x + w, y + h, z, u0 + 0.5f, v0 + 0.5f}};
        const int idx[6] = {0, 1, 2, 2, 1, 3};
        for (int k : idx) verts.push_back(q[k]);
    }
    std::printf("[probe] row3 shape: %.2f Mpixels covered per frame (%.1f layers deep on average)\n", covered / 1e6, covered / (W * H));
    WGPUBuffer vbuf = make_buffer(verts.size() * sizeof(V), WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
    wgpuQueueWriteBuffer(g_queue, vbuf, 0, verts.data(), verts.size() * sizeof(V));
    // a 256 x 256 texture whose alpha fails the test (below 0.1) in about 12 % of the texels
    std::vector<std::uint8_t> texels(256 * 256 * 4);
    for (int i = 0; i < 256 * 256; ++i) {
        texels[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(rnd() * 255), texels[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(rnd() * 255), texels[static_cast<std::size_t>(i) * 4 + 2] = 200;
        texels[static_cast<std::size_t>(i) * 4 + 3] = rnd() < 0.12f ? 0 : 255;
    }
    WGPUTexture tex = make_texture(256, 256, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = tex;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = 256 * 4;
    layout.rowsPerImage = 256;
    const WGPUExtent3D ext = {256, 256, 1};
    wgpuQueueWriteTexture(g_queue, &dst, texels.data(), texels.size(), &layout, &ext);
    WGPUTextureView texv = view_of(tex);
    WGPUBindGroupLayoutEntry le = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    le.binding = 0;
    le.visibility = WGPUShaderStage_Fragment;
    le.texture.sampleType = WGPUTextureSampleType_Float;
    le.texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 1;
    ld.entries = &le;
    WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &bgl;
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(g_device, &pld);
    WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
    be.binding = 0;
    be.textureView = texv;
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = bgl;
    bd.entryCount = 1;
    bd.entries = &be;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_device, &bd);
    WGPUVertexAttribute attrs[2] = {{nullptr, WGPUVertexFormat_Float32x3, 0, 0}, {nullptr, WGPUVertexFormat_Float32x2, 12, 1}};
    WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbl.arrayStride = sizeof(V);
    vbl.attributeCount = 2;
    vbl.attributes = attrs;

    struct Variant {
        const char *name;
        bool discard, frag_depth, color, depth_write;
        WGPURenderPipeline pipe = nullptr;
        std::vector<double> t;
    };
    // pass1 = the colour pass of the two-pass alpha test (depth test on, depth writes off, no discard); pass2 = the depth-only pass with the discard
    std::vector<Variant> variants = {
        {"A  card depth test + write, colour, no discard (a plain draw)", false, false, true, true},
        {"B  card depth test + write, colour, alpha test by discard", true, false, true, true},
        {"C  depth from the shader + colour, no discard", false, true, true, true},
        {"D  depth from the shader + colour, alpha test by discard", true, true, true, true},
        {"E  pass 1 of the two-pass alpha test: colour, depth test only, no discard", false, false, true, false},
        {"F  pass 2: depth only, card depth write, discard", true, false, false, true},
        {"G  pass 2: depth only, depth from the shader, discard", true, true, false, true},
    };
    for (Variant &v : variants) {
        std::string code = R"(
struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) z: f32 };
@group(0) @binding(0) var tex: texture_2d<f32>;
@vertex fn vs(@location(0) p: vec3f, @location(1) uv: vec2f) -> VO {
    var o: VO;
    o.pos = vec4f(p.x / 640.0 * 2.0 - 1.0, 1.0 - p.y / 448.0 * 2.0, FRAGDEPTH_POS, 1.0);
    o.uv = uv;
    o.z = p.z;
    return o;
}
struct FO { @location(0) c: vec4f DEPTH_FIELD };
@fragment fn fs(i: VO) -> FO {
    let t = textureLoad(tex, vec2i(i.uv * 255.0), 0);
    DISCARD
    var o: FO;
    o.c = vec4f(t.rgb, 1.0);
    DEPTH_ASSIGN
    return o;
}
)";
        const auto replace = [&](const std::string &key, const std::string &val) {
            for (std::size_t at; (at = code.find(key)) != std::string::npos;) code.replace(at, key.size(), val);
        };
        replace("FRAGDEPTH_POS", v.frag_depth ? "0.5" : "p.z");
        replace("DEPTH_FIELD", v.frag_depth ? ", @builtin(frag_depth) d: f32" : "");
        replace("DISCARD", v.discard ? "if (t.a < 0.1) { discard; }" : "");
        replace("DEPTH_ASSIGN", v.frag_depth ? "o.d = i.z;" : "");
        WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
        ds.format = WGPUTextureFormat_Depth32Float;
        ds.depthWriteEnabled = v.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        ds.depthCompare = WGPUCompareFunction_GreaterEqual;
        v.pipe = make_pipeline(code, WGPUTextureFormat_RGBA8Unorm, nullptr, &ds, pl, v.color ? WGPUColorWriteMask_All : WGPUColorWriteMask_None, &vbl);
        if (!v.pipe) std::printf("[probe] row3 variant %c: pipeline refused: %s\n", v.name[0], g_last_error.substr(0, 200).c_str());
    }
    WGPUTexture target = make_texture(W, H, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment);
    WGPUTexture depth = make_texture(W, H, WGPUTextureFormat_Depth32Float, WGPUTextureUsage_RenderAttachment);
    WGPUTextureView tv = view_of(target), zv = view_of(depth);
    double e_cpu = 0;
    for (int rep = 0; rep < reps + 2; ++rep) {
        for (Variant &v : variants) {
            if (!v.pipe) continue;
            const auto t0 = Clock::now();
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
            Pass p;
            p.color = tv;
            p.depth = zv;
            WGPURenderPassEncoder rp = begin_pass(enc, p);
            wgpuRenderPassEncoderSetPipeline(rp, v.pipe);
            wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, nullptr);
            wgpuRenderPassEncoderSetVertexBuffer(rp, 0, vbuf, 0, verts.size() * sizeof(V));
            wgpuRenderPassEncoderDraw(rp, static_cast<std::uint32_t>(verts.size()), 1, 0, 0);
            wgpuRenderPassEncoderEnd(rp);
            wgpuRenderPassEncoderRelease(rp);
            submit(enc);
            wait_gpu();
            if (rep >= 2) v.t.push_back(ms_since(t0));
        }
    }
    (void)e_cpu;
    double base = 0;
    for (Variant &v : variants) {
        if (v.t.empty()) continue;
        const double b = *std::min_element(v.t.begin(), v.t.end()), m = median(v.t);
        if (v.name[0] == 'A') base = b;
        std::printf("[probe] row3 %-78s best %7.3f ms  median %7.3f ms  (%.2fx of A)\n", v.name, b, m, base > 0 ? b / base : 0.0);
    }
    const auto best = [&](char c) {
        for (Variant &v : variants)
            if (v.name[0] == c && !v.t.empty()) return *std::min_element(v.t.begin(), v.t.end());
        return 0.0;
    };
    std::printf("[probe] row3 two-pass alpha test, card depth: E + F = %.3f ms (B alone, one draw: %.3f); two-pass with shader depth in pass 2: E + G = %.3f ms; shader depth on the one-draw form: D / B = %.2fx\n", best('E') + best('F'), best('B'),
                best('E') + best('G'), best('B') > 0 ? best('D') / best('B') : 0.0);
}

}  // namespace

int main(int argc, char **argv) {
    std::string backend = "auto", rows = "1,2,3";
    int reps = 15, shapes = 50000;
    float side = 12.0f;
    bool ascending = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--backend") backend = next();
        else if (a == "--rows") rows = next();
        else if (a == "--reps") reps = std::atoi(next().c_str());
        else if (a == "--shapes") shapes = std::atoi(next().c_str());
        else if (a == "--side") side = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--order") ascending = next() != "random";
    }
    if (!open_device(backend)) {
        std::printf("[probe] no device\n");
        return 1;
    }
    if (rows.find('1') != std::string::npos) row1();
    if (rows.find('2') != std::string::npos) row2(reps);
    if (rows.find('3') != std::string::npos) row3(reps, shapes, side, ascending);
    return 0;
}
