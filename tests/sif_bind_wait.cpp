// No game data: an EE bind to an RPC server no IOP module has registered. A retail IOP answers it at once with a null server
// (ps2sdk iop/system/sifcmd sifrpc.c, _request_bind, AFL-2.0) and the EE library's caller binds again. The runner holds the first
// bind to an id for 4 s of game time, so a server that registers a moment later still gets it; but Mercenaries (PAL) binds a server
// that never comes (0x00534E03) about 150 times, and at 4 s each it sat 10 minutes on its rating screen. Checked here: the first bind
// still waits and a server registering during that wait answers it; once a bind to an id timed out, later binds to it are answered
// "no server" within a frame and logged once; a server that registers after the timeout gets the next bind as before; an IOP reset
// forgets the timed-out ids. The EE side is played by hand: bind packets go out with sceSifSetDma as sceSifSendCmd sends them, and
// each reply is taken the way the EE's SIF interrupt handler takes it (copy the packet, then isceSifSetDChain).
// Build: ninja ps2sifbindtest, run build/.../ps2sifbindtest (prints "SIF bind wait: 0 failed").
#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
namespace iop = ps2::iop;
namespace sif = ps2::sif;

int failed = 0;
void check(bool ok, const char *message) {
    // stdout: stderr is redirected into the capture file while the checks run
    if (!ok) { ++failed; std::printf("FAIL: %s\n", message); }
}

constexpr std::uint32_t kXor = 0xACACACACu, kCmdBuffer = 0x0001E000u;
constexpr std::uint32_t kList = 0x1E00000, kSetRecv = 0x1E00100, kBindPacket = 0x1E00200, kRecvBuf = 0x1E00800, kClient = 0x1E00400;
constexpr std::uint64_t kFrame = ps2::kEeClock / 60, kStep = ps2::kEeClock / 600;
constexpr std::uint32_t kNever = 0x00534E03u, kLate = 0x7F00B001u, kSoon = 0x7F00B002u;

std::uint32_t rd(std::uint32_t a) {
    std::uint32_t v;
    std::memcpy(&v, ps2::g_ram + a, 4);
    return v;
}

void send_packet(std::uint32_t packet, std::uint32_t size) {
    const std::uint32_t list[4] = {packet, kCmdBuffer, size, 0};
    std::memcpy(ps2::g_ram + kList, list, sizeof list);
    sif::set_dma(kList, 1);
}

// Where the IOP's packets land in EE memory (command 0x80000002, as sceSifInitCmd sends it).
void set_receive_buffer() {
    const std::uint32_t set_recv[6] = {24, 0, 0x80000002u, 0, kRecvBuf, 0};
    std::memcpy(ps2::g_ram + kSetRecv, set_recv, sizeof set_recv);
    send_packet(kSetRecv, sizeof set_recv);
}

// EE game time moves on by `cycles`, and the IOP side looks at its pending binds as at an IOP catch-up.
void advance(std::uint64_t cycles) {
    ps2::set_budget(ps2::g_cpu, static_cast<std::int64_t>(cycles));
    ps2::g_cpu.budget = 0;
    sif::pump();
}

struct Reply {
    bool came = false;
    std::uint64_t after = 0;  // EE cycles from the bind to its reply
    std::uint32_t server_data = 0;
};

// sceSifBindRpc's packet (header, record id, packet address, rpc id, client, server id), then wait for the reply for at most `limit`
// cycles. `during` runs once after `at` cycles of waiting (a server that registers meanwhile).
template <typename F>
Reply bind(std::uint32_t server, std::uint64_t limit, std::uint64_t at, F during) {
    std::memset(ps2::g_ram + kRecvBuf, 0, 64);
    const std::uint32_t words[10] = {40, 0, 0x80000009u, 0, 0, kBindPacket, 1, kClient, server, 0};
    std::memcpy(ps2::g_ram + kBindPacket, words, sizeof words);
    const std::uint64_t start = ps2::now();
    send_packet(kBindPacket, sizeof words);
    bool ran = false;
    Reply r;
    for (;;) {
        const std::uint64_t waited = ps2::now() - start;
        if (rd(kRecvBuf + 8) == 0x80000008u) {
            r.came = rd(kRecvBuf + 32) == 0x80000009u && rd(kRecvBuf + 28) == kClient;
            r.after = waited;
            r.server_data = rd(kRecvBuf + 36);
            std::memset(ps2::g_ram + kRecvBuf, 0, 64);
            sif::packet_processed();
            return r;
        }
        if (waited > limit) return r;
        if (!ran && waited >= at) {
            ran = true;
            during();
        }
        advance(kStep);
    }
}
Reply bind(std::uint32_t server, std::uint64_t limit) { return bind(server, limit, ~0ull, [] {}); }

bool no_reply_server(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) { return false; }

std::size_t count(const std::string &text, const std::string &what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}
}  // namespace

int main() {
    const std::filesystem::path capture = std::filesystem::temp_directory_path() / "ps2-sif-bind-wait-test.txt";
    if (!std::freopen(capture.string().c_str(), "w", stderr)) {
        std::printf("cannot open %s\n", capture.string().c_str());
        return 1;
    }
    iop::reset();
    set_receive_buffer();
    const std::uint64_t four_s = ps2::kEeClock * 4;

    // 1. A server that never registers: the first bind waits the full 4 s, then "no server".
    const Reply first = bind(kNever, four_s + kFrame * 10);
    check(first.came && first.server_data == 0, "the first bind to a server that is not there is answered with no server");
    check(first.after >= four_s && first.after <= four_s + kFrame, "the first bind waits the full 4 s, as before");

    // 2. Every later bind to it is answered within a frame, still with no server (Mercenaries' 150 tries).
    bool fast = true, none = true;
    std::uint64_t longest = 0;
    for (int i = 0; i < 150; ++i) {
        const Reply again = bind(kNever, four_s + kFrame * 10);
        fast = fast && again.came && again.after <= kFrame + kStep;
        none = none && again.server_data == 0;
        longest = std::max(longest, again.after);
    }
    check(fast, "later binds to a server that timed out are answered within a frame");
    check(none, "later binds to it still say no server");

    // 3. A server that registers during the first wait (a module that starts a moment after the bind) answers that bind.
    const Reply soon = bind(kSoon, four_s + kFrame * 10, ps2::kEeClock, [] { sif::register_hle_server(kSoon, no_reply_server); });
    check(soon.came && soon.server_data == (kSoon ^ kXor), "a server registering during the first wait answers the bind");
    check(soon.after >= ps2::kEeClock && soon.after < ps2::kEeClock + kFrame, "it answers when it registers, not at the deadline");

    // 4. A server that registers only after a bind to it timed out: the next bind finds it at once.
    const Reply late1 = bind(kLate, four_s + kFrame * 10);
    check(late1.came && late1.server_data == 0 && late1.after >= four_s, "a late server's first bind times out");
    const Reply late2 = bind(kLate, four_s + kFrame * 10);
    check(late2.came && late2.server_data == 0 && late2.after <= kFrame + kStep, "its second bind is answered within a frame");
    const Reply late3 = bind(kLate, four_s + kFrame * 10, kFrame / 2, [] { sif::register_hle_server(kLate, no_reply_server); });
    check(late3.came && late3.server_data == (kLate ^ kXor), "a server registering while a quick bind waits answers it");
    const Reply late4 = bind(kLate, four_s + kFrame * 10);
    check(late4.came && late4.server_data == (kLate ^ kXor) && late4.after == 0, "once registered, binds succeed at once as before");

    sif::dump();
    std::fflush(stderr);

    // 5. An IOP reset (a new IOP image, its modules start again) forgets the ids: the first bind waits the full time again.
    iop::reset();
    set_receive_buffer();
    const Reply reset = bind(kNever, four_s + kFrame * 10);
    check(reset.came && reset.server_data == 0 && reset.after >= four_s, "after an IOP reset the first bind waits the full 4 s again");

    std::fflush(stderr);
    std::ostringstream text;
    text << std::ifstream(capture).rdbuf();
    const std::string log = text.str();
    check(count(log, "bind to 0x00534E03 waits for the server") == 2, "the wait is logged once per id (and again after the reset)");
    check(count(log, "bind to 0x00534E03 timed out; later binds to it are answered after one frame") == 2,
          "the timeout is logged once per id, not once per bind");
    check(count(log, "no server 0x00534E03: a bind to it timed out, 150 later binds") == 1, "the run's summary counts the quick answers");

    std::printf("SIF bind wait: %d failed (longest later bind %.2f frames)\n", failed, static_cast<double>(longest) / kFrame);
    return failed ? 1 : 0;
}
