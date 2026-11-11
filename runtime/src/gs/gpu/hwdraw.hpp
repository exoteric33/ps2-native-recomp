#pragma once
// The experimental hardware drawing path (read the comment at the top of hwdraw.cpp first; it is off unless PS2_GPU_HW=1). It draws the graphics chip's triangles and
// sprites with the graphics card's fixed-function rasterizer (render pipelines) instead of the compute-shader rasterizer in gs.wgsl. With PS2_GPU_BENCH=<vblank>,<reps>
// it replays one captured frame as a measuring aid; without it the running game's plain shapes are drawn this way (the live_* functions).

#include <webgpu/webgpu.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ps2::gs::gpu::hw {

bool enabled();  // PS2_GPU_HW=1
bool match();    // PS2_GPU_HW_MATCH=1: the compute path leaves out the shapes the trial leaves out (to compare the two pictures like for like)
bool left_out(const std::uint32_t *prim, const std::uint32_t *setup);

// PS2_GPU_HW_CLASS=1 (with PS2_GPU_BENCH): every shape of the frame is sorted into one class and the compute path is timed without each class.
bool class_enabled();
int class_count();
const char *class_name(int c);
bool class_is_hardware(int c);  // classes 0 to 2: the card can draw them
int shape_class(const std::uint32_t *prim, const std::uint32_t *setup);

// One recorded command of a captured submit (the compute commands of gpu.cpp, pipeline numbers as in its enum).
struct Cmd {
    std::uint32_t pipeline, x, y, param;
};
struct Sub {
    std::vector<Cmd> commands;
    WGPUBindGroup group;  // the group gpu.cpp built for this submit's parameters and arena
};

// What the trial needs from gpu.cpp (the buffers it shares and the numbers of its pipeline enum).
struct Context {
    WGPUDevice device;
    WGPUQueue queue;
    WGPUBuffer vram, tables, clut, scratch, cache;
    WGPUBindGroupLayout layout;  // gpu.cpp's group 0 (8 bindings)
    std::vector<WGPUComputePipeline> pipelines;
    std::uint32_t id_upload, id_load_clut, id_raster, id_decode, id_snapshot;
    std::uint32_t scale;
    WGPUBuffer vram_copy;
    std::uint64_t vram_bytes;
    const char *gs_shader;  // gs.wgsl text, the trial's compute helpers are appended to it
    void (*read_vram)(std::vector<std::uint32_t> &out) = nullptr;  // copies graphics memory to the processor (for the judge)
    const std::vector<std::uint32_t> *painter_vram = nullptr;  // graphics memory after the painter's replay of the same frame
    void (*read_before)(std::vector<std::uint32_t> &out) = nullptr;  // graphics memory as it was before the frame (the copy the replays start from)
};

// PS2_GPU_HW_DIFF=1 makes the replay of a captured frame do only what tests/hwalpha.cpp needs: the painter once, the card path once,
// both memories kept here, and the bench is armed again for the next frame. The test program draws many small random frames through gpu::draw and compares.
bool diff_enabled();
struct DiffResult {
    bool valid = false;
    std::vector<std::uint32_t> before, painter, card;
    std::uint64_t shapes = 0, shapes_card = 0;          // shapes of the frame, and the ones the card path drew
    std::vector<std::pair<std::string, std::uint64_t>> left_out;  // why the card path left shapes to the painter, and how many
    std::uint64_t draw_calls = 0, two_pass_shapes = 0, exact_order_shapes = 0;  // shapes of runs that are not independent
};
DiffResult &diff_result();


// ---- PS2_GPU_HW=1 without PS2_GPU_BENCH draws the running game's plain shapes through the card path.
// gpu.cpp routes each batch of shapes (live_route per setup, live_batch at the flush), tells the target bookkeeping about every other reader and writer of graphics memory
// (live_reader, live_writer, live_painter), and hands the card's work over at submit() in order with the compute commands (live_before, live_encode).
struct LiveBatch {
    const std::uint32_t *prims;    // 28 words per shape (gpu.cpp's record)
    std::uint32_t prim_count;
    const std::uint32_t *setups;   // 40 words per setup
    std::uint32_t setup_count;
    std::uint32_t fbp, fbw, fpsm, zbp, zpsm;
    bool zte, zwrite;
    int minx, miny, maxx, maxy;
    const std::uint64_t *reads;    // byte ranges (low, high) of the textures the batch reads
    std::uint32_t read_count;
    std::uint64_t frame;
};
struct LiveDisplay {
    std::uint32_t fbp, fbw, psm, dbx, dby, width, height, scratch_at;
    bool halved, overlay;
};
bool live_init(Context &ctx);                  // after the device exists; false: the painter draws everything
bool live_on();
void live_position(std::uint32_t commands);   // compute commands queued so far in this submit: gpu.cpp says it before every call below
int live_route(const std::uint32_t *setup);    // 0 painter, 1 card, 2 either (the chip draws nothing for it)
bool live_waits();                             // the last live_route answered 0 only because a pipeline is still being made
bool live_ready_changed(std::uint64_t &seen);  // a pipeline finished since `seen` (updated)
int live_look();                               // the id of the look of the setup live_route answered last (-1: none)
void live_shape(int look, const std::uint32_t *setup, const std::uint32_t *rec, bool fresh);  // draw() added this shape (rec: gpu.cpp's 28 words) to a card batch; fresh: it is the first shape of the batch
bool live_batch(const LiveBatch &b);           // the batch is drawn by the card (false: nothing was done, the painter takes it)
void live_painter(const LiveBatch &b);         // the painter draws this batch: write back what it reads, mark what it writes
void live_reader(std::uint64_t lo, std::uint64_t hi);  // someone reads graphics memory [lo, hi) at this point of the command stream
void live_writer(std::uint64_t lo, std::uint64_t hi);  // someone overwrites graphics memory [lo, hi) here
bool live_display(const LiveDisplay &d, const std::uint32_t *params, std::uint32_t count);  // the picture comes from a target (the display op is queued); false: the old way
void live_state_loaded();                      // memory was replaced by a saved state: every target is dropped, nothing is written back
// the processor-side record of palette alpha, fed in the order of the game's writes.
void pal_upload(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t dsax, std::uint32_t dsay, std::uint32_t rrw, std::uint32_t done, std::uint32_t pixels, std::uint32_t lead_bits,
                std::uint32_t bpp, const void *bytes, std::size_t size, std::uint64_t lo, std::uint64_t hi);  // an upload chunk (after it was queued)
void pal_write(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1);       // a draw or a copy overwrites this rectangle
void pal_clut(std::uint32_t from, std::uint32_t to, std::uint32_t cbp, std::uint32_t cpsm, std::uint32_t csm, std::uint32_t csa, bool eight);  // a palette load into slot `to` (after it was queued)
void pal_state(const std::uint32_t *words, std::size_t count);  // a saved state replaced memory (words null: not known)
bool live_pending();                           // card work waits for the next submit
bool live_before(std::uint32_t command);       // card work waits that goes before compute command `command`
void live_prepare();                           // submit: the data of the card's work goes to the card
// submit: encode the card's work that goes before compute command `upto`. `pass` is the caller's open compute pass (or null): the card's own compute work (loads,
// write-backs) goes into it instead of a pass of its own. Returns the compute pass that is open afterwards (the same, another one, or null after a render pass);
// the caller goes on with it or begins a new one.
WGPUComputePassEncoder live_encode(WGPUCommandEncoder enc, std::uint32_t upto, WGPUComputePassEncoder pass);
void live_submitted();                         // submit: the work is handed over
void live_stats();

// PS2_GPU_HW_MEMLOG=<n> prints one `[memlog]` line every n frames with what the card path holds (textures, buffers, bind groups, pipelines), what wgpu holds in all, and the
// process's resident memory and how many queue submits the card is behind. gpu.cpp calls memlog_tick at the end of each submit; with the switch off that is one test of a bool.
extern bool g_memlog;
void memlog_report(void *instance, void *queue, std::uint64_t frame);
inline void memlog_tick(void *instance, void *queue, std::uint64_t frame) {
    if (g_memlog) memlog_report(instance, queue, frame);
}

// Capture, called from gpu.cpp while PS2_GPU_BENCH records the frame.
void capture_begin();
bool capturing();
void capture_batch(std::uint32_t submit, std::uint32_t command, const std::uint32_t *prims, std::uint32_t prim_count, const std::uint32_t *setups, std::uint32_t setup_count);
void capture_pre(std::uint32_t submit, std::uint32_t command, bool clut, std::uint64_t lo, std::uint64_t hi);

// After the capture: print the survey of the frame, then replay it with hardware drawing and time it.
void survey_print();
void run(Context &ctx, std::vector<Sub> &subs, unsigned reps, void (*wait)(), void (*shot)(const char *tag));

}
