// No game data: the SIF log names the server an RPC call of the EE went to, and the summary at the end of a run counts the calls per
// server id (public issue 71, DreamWorks Over the Hedge). A traced log showed "call endpoint 0x2CACACAF fn 1 ... args 00008000" right
// after the bind of the game's own I/O driver 0x00012345; it took a decode by hand (0x2CACACAF is 0x80000003 ^ 0xACACACAC) to see that
// the call went to the IOP heap, and that the driver itself was never called. The EE side is played with the two transfers the console's
// sceSifCallRpc makes: the arguments, then the call packet (tests/iop_bounds.cpp does the same).
// Build: ninja ps2sifcalllogtest, run build/.../ps2sifcalllogtest (prints "SIF call log: 0 failed").
#include "iop/iop.hpp"
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

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

constexpr std::uint32_t kXor = 0xACACACACu;
constexpr std::uint32_t kArgs = 0x1E00200, kRecv = 0x1E00300, kList = 0x1E00000, kPacket = 0x1E00100;

// A call from the EE to the endpoint `endpoint`, as the EE kernel sends it: the arguments' address (sceSifSetDma to the RPC receive
// address), then the call packet to the command buffer.
void ee_call(std::uint32_t endpoint, std::uint32_t method, std::uint32_t send_size, std::uint32_t recv_size) {
    std::uint32_t packet[14] = {56, 0, 0x8000000Au, 0, 0, 0, 0, 0, method, send_size, kRecv, recv_size, 0, endpoint};
    std::memcpy(ps2::g_ram + kPacket, packet, sizeof packet);
    const std::uint32_t list[8] = {kArgs, 0xDEADBEF0u, send_size, 0, kPacket, 0x0001E000u, sizeof packet, 0};
    std::memcpy(ps2::g_ram + kList, list, sizeof list);
    sif::set_dma(kList, 2);
}

bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

std::string hex(std::uint32_t v) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08X", v);
    return buffer;
}
}  // namespace

int main() {
    iop::reset();
    ps2::g_options.trace_iop_modules = true;
    iop::kernel::g_trace_modules = true;

    const std::filesystem::path capture = std::filesystem::temp_directory_path() / "ps2-sif-call-log-test.txt";
    if (!std::freopen(capture.string().c_str(), "w", stderr)) {
        std::printf("cannot open %s\n", capture.string().c_str());
        return 1;
    }

    constexpr std::uint32_t kHeap = 0x80000003u, kReady = 0x8000059Au, kNobody = 0x7F000001u;
    ee_call(kHeap ^ kXor, 5, 4, 4);   // sceSifQueryMemSize: the IOP heap, twice
    ee_call(kHeap ^ kXor, 5, 4, 4);
    ee_call(kReady ^ kXor, 0, 4, 4);  // the disc-ready server, once
    ee_call(kNobody ^ kXor, 1, 4, 4); // a server nobody registered: the old line, and the new one names no server
    iop::run(16384);
    sif::dump();
    std::fflush(stderr);
    std::ostringstream text;
    text << std::ifstream(capture).rdbuf();
    const std::string log = text.str();

    check(has(log, "call endpoint " + hex(kHeap ^ kXor) + " (server " + hex(kHeap) + ") fn 5"), "a call to the IOP heap names its server");
    check(has(log, "call endpoint " + hex(kReady ^ kXor) + " (server " + hex(kReady) + ") fn 0"), "a call to the disc-ready server names its server");
    check(has(log, "call endpoint " + hex(kNobody ^ kXor) + " (unknown server) fn 1"), "a call to an endpoint nobody serves says so");
    check(has(log, "EE calls by server id: " + hex(kHeap) + " x2 " + hex(kReady) + " x1\n"), "the summary counts the calls per server id");
    check(!has(log, hex(kNobody) + " x"), "a call to an unknown endpoint is not counted as a call to a server");

    std::filesystem::remove(capture);
    std::printf("SIF call log: %d failed\n", failed);
    return failed ? 1 : 0;
}
