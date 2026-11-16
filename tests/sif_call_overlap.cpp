// No game data: two RPC calls on one EE client before the first one ended (issue 106, Seek and Destroy). The EE library keeps
// a call's state in the client struct (packet +0, packet id +4, semaphore +8, end function +28, its argument +32; ps2sdk
// ee/kernel/src/sifrpc.c, AFL-2.0), and its end command handler reads them back from the client. Thread 1 calls and waits on
// semaphore 49; thread 2 calls on the same client before the first end arrives and writes semaphore 53 there. The first end must
// still wake semaphore 49 and run the first call's end function, the second end semaphore 53. The EE side is played by hand: the
// call packets go out with sceSifSetDma as sceSifSendCmd sends them, and each end packet is taken the way the EE's SIF interrupt
// handler takes it (copy the packet, isceSifSetDChain, then the end handler reads the client).
// Build: ninja ps2sifcalloverlaptest, run build/.../ps2sifcalloverlaptest (prints "SIF call overlap: 0 failed").
#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>

namespace {
namespace iop = ps2::iop;
namespace sif = ps2::sif;

int failed = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failed; std::printf("FAIL: %s\n", message); }
}

constexpr std::uint32_t kXor = 0xACACACACu, kCmdBuffer = 0x0001E000u;
constexpr std::uint32_t kList = 0x1E00000, kSetRecv = 0x1E00100, kRecvBuf = 0x1E00800, kReply = 0x1E00300;
constexpr std::uint32_t kServer1 = 0x7F000106u, kServer2 = 0x7F000107u;

std::uint32_t rd(std::uint32_t a) {
    std::uint32_t v;
    std::memcpy(&v, ps2::g_ram + a, 4);
    return v;
}
void wr(std::uint32_t a, std::uint32_t v) { std::memcpy(ps2::g_ram + a, &v, 4); }

void send_packet(std::uint32_t packet, std::uint32_t size) {
    const std::uint32_t list[4] = {packet, kCmdBuffer, size, 0};
    std::memcpy(ps2::g_ram + kList, list, sizeof list);
    sif::set_dma(kList, 1);
}

// sceSifCallRpc: the client's fields first, then the call packet (which lies at `packet` and names itself as pkt_addr).
void call(std::uint32_t client, std::uint32_t packet, std::uint32_t id, std::int32_t sema, std::uint32_t end, std::uint32_t param,
          std::uint32_t function, std::uint32_t server) {
    wr(client + 0, packet);
    wr(client + 4, id);
    wr(client + 8, static_cast<std::uint32_t>(sema));
    wr(client + 28, end);
    wr(client + 32, param);
    const std::uint32_t words[14] = {56, 0, 0x8000000Au, 0, 0, packet, id, client, function, 0, kReply, 0, 1, server ^ kXor};
    std::memcpy(ps2::g_ram + packet, words, sizeof words);
    send_packet(packet, sizeof words);
}

struct Seen {
    std::uint32_t cid, client;
    std::int32_t sema;
    std::uint32_t end, param;
};

// The EE's SIF interrupt: copy the packet, isceSifSetDChain, then the end handler (_request_end) reads the client, signals its
// semaphore, frees the packet and clears client + 0.
Seen take_end() {
    Seen s{};
    s.cid = rd(kRecvBuf + 8);
    s.client = rd(kRecvBuf + 28);
    wr(kRecvBuf, 0);
    sif::packet_processed();
    if (s.cid != 0x80000008u) return s;
    s.sema = static_cast<std::int32_t>(rd(s.client + 8));
    s.end = rd(s.client + 28);
    s.param = rd(s.client + 32);
    wr(s.client, 0);
    return s;
}
}  // namespace

int main() {
    iop::reset();
    sif::register_hle_server(kServer1, [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) { return false; });
    sif::register_hle_server(kServer2, [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) { return false; });
    // Where the IOP's packets land in EE memory (command 0x80000002, as sceSifInitCmd sends it).
    const std::uint32_t set_recv[6] = {24, 0, 0x80000002u, 0, kRecvBuf, 0};
    std::memcpy(ps2::g_ram + kSetRecv, set_recv, sizeof set_recv);
    send_packet(kSetRecv, sizeof set_recv);

    // 1. The overlap of issue 106: a waiting call (semaphore 49), then a second waiting call on the same client (semaphore 53).
    constexpr std::uint32_t kClient = 0x1E00400, kPacketA = 0x1E00500, kPacketB = 0x1E00540;
    call(kClient, kPacketA, 0x11, 49, 0x00101000u, 0xA, 0x80E0, kServer1);
    call(kClient, kPacketB, 0x12, 53, 0x00102000u, 0xB, 0x8040, kServer2);
    sif::complete_call(kServer1);
    const Seen first = take_end();
    check(first.cid == 0x80000008u && first.client == kClient, "the first end command reaches the EE");
    check(first.sema == 49, "the first call's end wakes the semaphore its caller waits on (49), not the second call's (53)");
    check(first.end == 0x00101000u && first.param == 0xA, "the first call's end runs the first call's end function");
    sif::complete_call(kServer2);
    const Seen second = take_end();
    check(second.cid == 0x80000008u && second.sema == 53, "the second call's end wakes semaphore 53");
    check(second.end == 0x00102000u && second.param == 0xB, "the second call's end runs the second call's end function");

    // 2. No overlap: one call, its end. The client is not touched.
    constexpr std::uint32_t kClient2 = 0x1E00600, kPacketC = 0x1E00580;
    call(kClient2, kPacketC, 0x13, 50, 0x00103000u, 0xC, 1, kServer1);
    wr(kClient2 + 28, 0x00104000u);  // a value the game itself changed after the send stays as it is
    sif::complete_call(kServer1);
    const Seen alone = take_end();
    check(alone.sema == 50 && alone.end == 0x00104000u, "a call that does not overlap leaves the client as the game has it");

    // 3. An overlap of two calls that do not wait (semaphore -1): nothing is lost on a console either, so nothing changes.
    constexpr std::uint32_t kClient3 = 0x1E00700, kPacketD = 0x1E005C0, kPacketE = 0x1E00A00;
    call(kClient3, kPacketD, 0x14, -1, 0x00105000u, 0xD, 2, kServer1);
    call(kClient3, kPacketE, 0x15, -1, 0x00106000u, 0xE, 3, kServer2);
    sif::complete_call(kServer1);
    const Seen nowait = take_end();
    check(nowait.sema == -1 && nowait.end == 0x00106000u && nowait.param == 0xE,
          "calls that do not wait keep what the console does (the client's latest fields)");
    sif::complete_call(kServer2);
    take_end();

    std::printf("SIF call overlap: %d failed\n", failed);
    return failed ? 1 : 0;
}
