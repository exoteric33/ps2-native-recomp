#include "sif.hpp"

#include "../hw.hpp"
#include "iop.hpp"
#include "io.hpp"
#include "kernel.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <map>
#include <vector>

namespace ps2::iop {
// libs.cpp: a guest size that reaches past the end of memory is said there (rate-limited).
void bad_span(const char *what, std::uint32_t address, std::uint64_t size, const char *outcome);
}

namespace ps2::sif {

namespace {

// A copy between the two memories with a size the game chose, cut so it neither reads nor writes past either one (EE 32 MiB at
// `ee`, IOP 2 MiB at `iop_address`). Real games never reach the end, so for them the size comes back unchanged.
std::uint32_t fit_copy(const char *what, std::uint32_t ee, std::uint32_t iop_address, std::uint64_t size) {
    const std::uint64_t room = std::min<std::uint64_t>(kRamSize - (ee & (kRamSize - 1)),
                                                       iop::kRamSize - (iop::phys(iop_address) & (iop::kRamSize - 1)));
    if (size <= room) return static_cast<std::uint32_t>(size);
    iop::bad_span(what, ee, size, "cut to fit");
    return static_cast<std::uint32_t>(room);
}

// The same for a copy into EE memory only (a file read the EE asked for): cut at the end of the 32 MiB.
std::uint32_t fit_ee(const char *what, std::uint32_t ee, std::uint32_t size) {
    const std::uint32_t room = kRamSize - (ee & (kRamSize - 1));
    if (size <= room) return size;
    iop::bad_span(what, ee, size, "cut at the end of EE memory");
    return room;
}

// Text and bytes the EE hands over in its memory, read round the 32 MiB: a name without its 0 at the end of memory (or a size
// past it) cannot read past g_ram. `limit` is far above any real name, so real games read exactly what they did before.
std::string ee_text(std::uint32_t a, std::size_t limit = 4096) {
    std::string s;
    for (std::size_t i = 0; i < limit; ++i) {
        const char ch = static_cast<char>(g_ram[(a + i) & (kRamSize - 1)]);
        if (!ch) break;
        s += ch;
    }
    return s;
}

std::string ee_bytes(std::uint32_t a, std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(g_ram[(a + i) & (kRamSize - 1)]);
    return s;
}

constexpr std::uint32_t kRpcRecvAddr = 0xDEADBEF0u;
constexpr std::uint32_t kServerXor = 0xACACACACu;
constexpr std::uint32_t kCmdBuffer = 0x0001E000u;
constexpr std::uint32_t kCmdRend = 0x80000008u;
constexpr std::uint32_t kCmdSetSreg = 0x80000001u;
constexpr std::uint32_t kIopRpcPacketSize = 64u;
constexpr std::uint32_t kIopRpcPacketCount = 32u;
// An HLE server answers after about the time a call takes on a real IOP. Games
// depend on it: the calling thread blocks meanwhile and lower priority threads
// run, as Downhill Domination's loader needs its worker to reach SleepThread.
constexpr std::uint64_t kHleReplyUsec = 50;

struct Header {
    std::uint32_t size_word, dest, cid, opt;
    std::uint32_t packet_size() const { return size_word & 0xFFu; }
};

struct Call {
    Header header;
    std::uint32_t record_id, packet_addr, rpc_id, client_data, rpc_number, send_size, recv, recv_size, recv_mode, server_data;
};

struct Rend {
    Header header;
    std::uint32_t record_id, packet_addr, rpc_id, client_data, command, server_data, buffer, cbuffer;
};

struct Server {
    bool hle = false;
    HleServer invoke;
    std::uint32_t sd = 0, sid = 0, func = 0, buf = 0, cfunc = 0, cbuf = 0, queue = 0;
};

struct Request {
    Call call;
    std::uint32_t size;
    std::vector<std::uint8_t> data;
};

struct PendingBind {
    Rend reply;
    std::uint64_t deadline;
};

// An IOP module's bind to an EE server the EE has not registered yet. The EE's libsifrpc answers such a bind with server 0 at
// once, and the usual IOP client loops until it gets a server; a client that binds once would keep server 0 for good. Atlus's
// SDFSDMAN (Digital Devil Saga) sends the EE its ready flag (sreg 31) and binds to the EE's sound RPC server in the next
// instruction, and the EE registers that server only after it has seen the flag: on the hardware the EE wins that race, here the
// bind reached the EE first and every later call (voices, movie audio) went to server 0. So a bind the EE answers with server
// 0 is sent again a little later, until the EE answers with a server or the deadline passes, as an EE bind to an IOP server
// that is not registered yet (g_binds) is held until the server appears. Not part of the save state: a bind is microseconds long.
struct IopBind {
    std::uint32_t server;  // the SID bound to
    std::uint64_t deadline;
    bool reported;
};
std::map<std::uint32_t, IopBind> g_iop_binds;  // by client
constexpr std::uint64_t kIopBindRetryUsec = 1000;
constexpr std::uint64_t kIopBindTimeoutUsec = 4000000;
constexpr std::uint32_t kIopBindRetryJob = 1;  // Job::a of a kJobSifReply job that sends an IOP bind again (b client, c server)

struct CmdHandler {
    std::uint32_t handler, data;
};

// SMFLAG bits the IOP raises when it is up: SIFINIT (sifman), CMDINIT (sifcmd) and BOOTEND (the end of the boot, what SifIopSync
// looks at); a booted console has all three. A reset takes them down for the time the IOP needs to boot (kIopBootUsec, IOP clock),
// then they come back. g_boot_end is the IOP clock when that is over, 0 when no boot is running.
constexpr std::uint32_t kSmflagUp = 0x70000u;
constexpr std::uint64_t kIopBootUsec = 2000;
std::uint32_t g_mainaddr = 0, g_msflag = 0, g_smflag = kSmflagUp;
std::uint64_t g_boot_end = 0;
std::uint32_t g_ee_recv = 0;
std::uint32_t g_data_addr = 0;
bool g_processed = true;
// What the IOP sends the EE over SIF0, in order: command packets, and empty entries for an IOP sceSifSetDma with SIF_DMA_INT_O (see
// pump). g_processed is false while the EE's SIF0 channel is stopped after an interrupt, until its handler calls isceSifSetDChain.
std::deque<std::vector<std::uint8_t>> g_outgoing;
// SifDmaTransfer_t.attr (ps2sdk sifman.h, AFL-2.0). INT_I (0x2) sets the IRQ bit of the IOP's own DMA tag (the IOP's SIF0 interrupt,
// which here runs the sceSifSetDmaIntr callbacks anyway); INT_O sets the IRQ bit of the EE's tag (ps2sdk sifman.c sif_dma_setup_tag:
// 0x80000000 in the tag's count word): the EE's channel 5 stops after that transfer and raises its interrupt (CIS5), the same as for
// every command packet, which sceSifSendCmd sends with INT_O. ERT, SPR, BSN and TAG only mean something on the EE side.
constexpr std::uint32_t kSifDmaIntO = 0x4u;
// g_servers is the endpoint selected by SID for binding. Guest RPC calls carry
// the server-data pointer returned by that bind, so retain every registered
// guest endpoint independently instead of redirecting calls by SID.
std::map<std::uint32_t, Server> g_servers;
std::map<std::uint32_t, Server> g_guest_servers;
std::map<std::uint32_t, std::vector<std::uint32_t>> g_server_order;
std::map<std::uint32_t, PendingBind> g_binds;
std::map<std::uint32_t, Call> g_held_calls;
std::deque<Call> g_hle_replies;
std::map<std::uint32_t, std::deque<Request>> g_requests;
std::map<std::uint32_t, int> g_queue_threads;
std::map<std::uint32_t, CmdHandler> g_cmd_handlers;
// What the EE sent the IOP during the run, for the summary in dump(): SIF commands by id, those no IOP handler took, and binds by
// server id. Only for the log: not in save states, and an IOP reset does not clear them (the summary covers the whole run).
std::map<std::uint32_t, std::uint64_t> g_ee_commands;
std::map<std::uint32_t, std::uint64_t> g_unhandled_commands;
std::map<std::uint32_t, std::uint64_t> g_ee_binds;
std::map<std::uint32_t, std::uint64_t> g_ee_calls;  // RPC calls by server id (the server of an endpoint the EE called)
std::uint32_t g_sreg[32] = {};
std::map<std::uint32_t, int> g_iop_clients;
std::uint32_t g_iop_rpc_id = 1;
std::uint32_t g_iop_rpc_packets = 0;
std::uint32_t g_iop_rpc_next_packet = 0;
std::array<std::uint8_t, kIopRpcPacketCount> g_iop_rpc_packet_busy{};
std::array<std::uint32_t, kIopRpcPacketCount> g_iop_rpc_packet_reply{};

std::uint32_t acquire_iop_rpc_packet(std::uint32_t reply) {
    for (std::uint32_t n = 0; n < kIopRpcPacketCount; ++n) {
        const std::uint32_t slot = (g_iop_rpc_next_packet + n) % kIopRpcPacketCount;
        if (g_iop_rpc_packet_busy[slot]) continue;
        g_iop_rpc_packet_busy[slot] = 1;
        g_iop_rpc_packet_reply[slot] = reply;
        g_iop_rpc_next_packet = (slot + 1) % kIopRpcPacketCount;
        const std::uint32_t address = g_iop_rpc_packets + slot * kIopRpcPacketSize;
        std::memset(iop::ram_ptr(address), 0, kIopRpcPacketSize);
        return address;
    }
    std::fprintf(stderr, "[sif:iop-rpc] no free IOP request packet\n");
    return 0;
}

void release_iop_rpc_packet(std::uint32_t destination) {
    if (!g_iop_rpc_packets) return;
    const std::uint32_t end = g_iop_rpc_packets + kIopRpcPacketCount * kIopRpcPacketSize;
    if (destination >= g_iop_rpc_packets && destination < end &&
        (destination - g_iop_rpc_packets) % kIopRpcPacketSize == 0) {
        const std::uint32_t slot = (destination - g_iop_rpc_packets) / kIopRpcPacketSize;
        g_iop_rpc_packet_busy[slot] = 0;
        g_iop_rpc_packet_reply[slot] = 0;
        return;
    }
    for (std::uint32_t slot = 0; slot < kIopRpcPacketCount; ++slot) {
        if (!g_iop_rpc_packet_busy[slot] || !g_iop_rpc_packet_reply[slot] ||
            g_iop_rpc_packet_reply[slot] != destination) continue;
        g_iop_rpc_packet_busy[slot] = 0;
        g_iop_rpc_packet_reply[slot] = 0;
        // An rmode=0 call with a result writes only to recvbuf; its packet
        // header must report completion to sceSifCheckStatRpc as well.
        iop::write32(g_iop_rpc_packets + slot * kIopRpcPacketSize + 16u, 0);
        iop::write32(g_iop_rpc_packets + slot * kIopRpcPacketSize + 24u, 0);
        return;
    }
}

// The RPC end command (cid 0x80000008) frees the call's packet through the client's own packet pointer (hdr.pkt_addr, client + 0), not through
// the end packet's pkt_addr. This is the rule of PS2SDK's IOP sifcmd library (AFL-2.0, iop/system/sifcmd/src/sifrpc.c: its end handler
// frees rpkt->cd->hdr.pkt_addr and clears it). The EE's sceSifExecRequest never fills the end packet's pkt_addr for a call (it only sets
// client and cid, clears rpc_id and rec_id), and Silent Hill 2's EE sends exactly that: pkt_addr 0. Only a bind reply carries the packet
// address. A pointer that is not a busy ring packet of this very call (the same rpc_id) is left alone.
void release_client_iop_rpc_packet(std::uint32_t client) {
    if (!g_iop_rpc_packets) return;
    const std::uint32_t packet = iop::phys(iop::read32(client));
    const std::uint32_t end = g_iop_rpc_packets + kIopRpcPacketCount * kIopRpcPacketSize;
    if (packet < g_iop_rpc_packets || packet >= end || (packet - g_iop_rpc_packets) % kIopRpcPacketSize != 0) return;
    const std::uint32_t slot = (packet - g_iop_rpc_packets) / kIopRpcPacketSize;
    if (!g_iop_rpc_packet_busy[slot] || iop::read32(packet + 24u) != iop::read32(client + 4u)) return;
    g_iop_rpc_packet_busy[slot] = 0;
    g_iop_rpc_packet_reply[slot] = 0;
}

std::uint32_t begin_iop_rpc_packet(std::uint32_t client, std::uint32_t reply, std::uint32_t &record_id) {
    const std::uint32_t address = acquire_iop_rpc_packet(reply);
    if (!address) return 0;
    const std::uint32_t slot = (address - g_iop_rpc_packets) / kIopRpcPacketSize;
    record_id = (slot << 16u) | 5u; // allocated packet with a ring index
    const std::uint32_t rpc_id = g_iop_rpc_id++;
    iop::write32(address + 16u, record_id);
    iop::write32(address + 24u, rpc_id);
    iop::write32(client + 0u, address);
    iop::write32(client + 4u, rpc_id);
    return address;
}
void send(const void *packet, std::uint32_t size);

// The end of an IOP client's bind without a reply from the EE (a bind sent again found no request packet): the client's fields
// as an EE reply would set them, and its thread woken, as the RPC end command does.
void finish_iop_bind(std::uint32_t client, std::uint32_t server_data, std::uint32_t buff, std::uint32_t cbuff) {
    iop::write32(client + 36, server_data);
    iop::write32(client + 20, buff);
    iop::write32(client + 24, cbuff);
    iop::write32(client + 0, 0);
    auto it = g_iop_clients.find(client);
    if (it != g_iop_clients.end()) {
        if (iop::kernel::Thread *t = iop::kernel::thread(it->second)) iop::kernel::wake(*t, 0);
        iop::kernel::reschedule();
        g_iop_clients.erase(it);
    }
}

// The bind request of an IOP client, as sceSifBindRpc sends it (and sends again, see IopBind).
bool send_iop_bind(std::uint32_t client, std::uint32_t server) {
    std::uint32_t record_id = 0;
    const std::uint32_t packet_addr = begin_iop_rpc_packet(client, 0, record_id);
    if (!packet_addr) return false;
    iop::write32(client + 36, 0);
    iop::write32(client + 8, 0xFFFFFFFFu);
    const std::uint32_t packet[9] = {0x24u, 0, 0x80000009u, 0, record_id, packet_addr, iop::read32(client + 4), client, server};
    send(packet, sizeof packet);
    return true;
}

std::uint32_t g_loop_stub = 0, g_finish_stub = 0, g_exec_stub = 0;
// Where this boot made the loop stub, and whether the three stubs (and the two ioman stubs) sit in the other arrangement now. A loaded
// state does not boot again, so the stubs stay where the last state put them until the next load moves them.
std::uint32_t g_boot_loop_stub = 0;
bool g_stubs_shifted = false;
// A server thread that runs its own loop (sceSifGetNextRequest, then sceSifExecRequest, then SleepThread) instead of sceSifRpcLoop: the
// requests it has taken but not run yet (by handle), and where each thread goes on after the request it runs.
std::map<std::uint32_t, Request> g_taken;
std::map<int, std::uint32_t> g_exec_return;
std::uint32_t g_next_handle = 0;
std::uint32_t g_iop_dma_id = 1, g_ee_dma_id = 1;
std::uint64_t g_ee_dma_done = 0;

template <typename T>
T ee_read(std::uint32_t a) {
    T v;
    std::memcpy(&v, g_ram + (a & (kRamSize - 1)), sizeof v);
    return v;
}

void send(const void *packet, std::uint32_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(packet);
    g_outgoing.emplace_back(bytes, bytes + size);
    pump();
}

Rend make_rend(const Header &h, std::uint32_t record, std::uint32_t packet, std::uint32_t rpc, std::uint32_t client, std::uint32_t command) {
    Rend r{};
    r.header.size_word = sizeof(Rend);
    r.header.dest = h.dest;
    r.header.cid = kCmdRend;
    r.record_id = record;
    r.packet_addr = packet;
    r.rpc_id = rpc;
    r.client_data = client;
    r.command = command;
    return r;
}

void reply_call(const Call &call) {
    Rend r = make_rend(call.header, call.record_id, call.packet_addr, call.rpc_id, call.client_data, 0x8000000Au);
    send(&r, sizeof r);
}

void deliver_request(std::uint32_t queue) {
    auto t = g_queue_threads.find(queue);
    if (t == g_queue_threads.end()) return;
    iop::kernel::Thread *th = iop::kernel::thread(t->second);
    if (!th) return;
    if (th->status == iop::kernel::kWait && th->wait == iop::kernel::kWaitRpc) {
        iop::kernel::wake(*th, 0);
        iop::kernel::reschedule();
    } else if (th->rpc_queue != queue) {
        // The thread does not run sceSifRpcLoop: the library wakes it the way WakeupThread would (a thread that is not asleep yet
        // finds the wake-up when it goes to sleep), and it fetches the request with sceSifGetNextRequest.
        if (th->status == iop::kernel::kWait && th->wait == iop::kernel::kWaitSleep) {
            iop::kernel::wake(*th, 0);
            iop::kernel::reschedule();
        } else {
            ++th->wakeups;
        }
    }
}

const Server *first_guest_server(std::uint32_t sid) {
    const auto order = g_server_order.find(sid);
    if (order != g_server_order.end()) {
        for (const std::uint32_t sd : order->second) {
            const auto server = g_guest_servers.find(sd);
            if (server != g_guest_servers.end()) return &server->second;
        }
    }
    const auto server = g_servers.find(sid);
    return server != g_servers.end() && !server->second.hle ? &server->second : nullptr;
}

// The server id behind a call's endpoint: a module's own server has the address of its struct as endpoint, a built-in one sid ^ kServerXor
// (0x2CACACAF is 0x80000003, the IOP heap). 0 when no server of ours has it. A log that shows the endpoint only has to be decoded by hand.
std::uint32_t server_id_of_endpoint(std::uint32_t endpoint) {
    if (const auto guest = g_guest_servers.find(endpoint); guest != g_guest_servers.end()) return guest->second.sid;
    if (const auto hle = g_servers.find(endpoint ^ kServerXor); hle != g_servers.end()) return hle->second.sid;
    return 0;
}

bool fill_bind_endpoint(Rend &reply, std::uint32_t sid) {
    if (const Server *server = first_guest_server(sid)) {
        reply.server_data = server->sd;
        reply.buffer = server->buf;
        reply.cbuffer = server->cbuf;
        return true;
    }
    const auto server = g_servers.find(sid);
    if (server != g_servers.end() && server->second.hle) {
        reply.server_data = sid ^ kServerXor;
        reply.buffer = kRpcRecvAddr;
        reply.cbuffer = 0xDEADCAFEu;
        return true;
    }
    return false;
}

void complete_pending_bind(std::uint32_t sid) {
    auto bind = g_binds.find(sid);
    if (bind == g_binds.end() || !fill_bind_endpoint(bind->second.reply, sid)) return;
    send(&bind->second.reply, sizeof(Rend));
    g_binds.erase(bind);
}

void bind(const std::uint8_t *packet) {
    const auto *h = reinterpret_cast<const Header *>(packet);
    std::uint32_t record, paddr, rpc, client, server;
    std::memcpy(&record, packet + 16, 4);
    std::memcpy(&paddr, packet + 20, 4);
    std::memcpy(&rpc, packet + 24, 4);
    std::memcpy(&client, packet + 28, 4);
    std::memcpy(&server, packet + 32, 4);
    ++g_ee_binds[server];
    Rend r = make_rend(*h, record, paddr, rpc, client, 0x80000009u);
    if (fill_bind_endpoint(r, server)) {
        send(&r, sizeof r);
        return;
    }
    std::fprintf(stderr, "[sif] bind to 0x%08X waits for the server\n", server);
    g_binds[server] = PendingBind{r, now() + kEeClock * 4};
}

void call(const std::uint8_t *packet) {
    Call c;
    std::memcpy(&c, packet, sizeof c);
    const auto guest = g_guest_servers.find(c.server_data);
    // A client that bound before this build (a state of version 4 to 6) holds sid ^ kServerXor as its endpoint, also for a server of an
    // IOP module, and its data went to kRpcRecvAddr (g_data_addr). Such a call is still answered the old way instead of being dropped.
    const Server *legacy = nullptr;
    if (guest == g_guest_servers.end()) {
        const auto old = g_servers.find(c.server_data ^ kServerXor);
        if (old != g_servers.end() && !old->second.hle) legacy = &old->second;
    }
    if (guest == g_guest_servers.end() && !legacy) {
        const std::uint32_t sid = c.server_data ^ kServerXor;
        const auto it = g_servers.find(sid);
        if (it == g_servers.end() || !it->second.hle) {
            std::fprintf(stderr, "[sif] call %u to unknown endpoint 0x%08X\n", c.rpc_number, c.server_data);
            reply_call(c);
            return;
        }
        ++g_ee_calls[sid];
        // The IOP runs behind the EE by up to a slice; catch it up so the reply
        // latency counts from the moment of the call.
        sync_iop();
        g_held_calls[sid] = c;
        if (it->second.invoke(c.rpc_number, g_data_addr, c.send_size, c.recv & (kRamSize - 1), c.recv_size)) {
            g_held_calls.erase(sid);
            g_hle_replies.push_back(c);
            iop::kernel::schedule(iop::kernel::now() + iop::kernel::usec_to_cycles(kHleReplyUsec), iop::kernel::Job{iop::kernel::kJobSifReply});
        }
        return;
    }
    const Server &s = legacy ? *legacy : guest->second;
    ++g_ee_calls[s.sid];
    Request request{c, c.send_size, {}};
    if (s.buf && legacy) {
        const std::uint32_t size = (c.send_size + 15u) & ~15u;
        const std::uint32_t from = g_data_addr & (kRamSize - 1);
        request.data.assign(g_ram + from, g_ram + from + std::min(size, kRamSize - from));
    } else if (s.buf) {
        if (c.send_size > std::numeric_limits<std::uint32_t>::max() - 15u) {
            std::fprintf(stderr, "[sif] reject RPC fn %u: send size 0x%08X overflows alignment\n", c.rpc_number, c.send_size);
            return;
        }
        const std::uint32_t size = (c.send_size + 15u) & ~15u;
        const std::uint32_t from = iop::phys(s.buf) & (iop::kRamSize - 1u);
        if (size > iop::kRamSize - from) {
            std::fprintf(stderr,
                         "[sif] reject RPC fn %u: guest server 0x%08X buffer 0x%08X size 0x%X exceeds IOP RAM\n",
                         c.rpc_number, s.sd, s.buf, size);
            return;
        }
        const std::uint8_t *source = iop::ram_ptr(s.buf);
        request.data.assign(source, source + size);
    }
    g_requests[s.queue].push_back(std::move(request));
    deliver_request(s.queue);
}

void other_data(const std::uint8_t *packet) {
    std::uint32_t record, paddr, rpc, receive, src, dst, size;
    std::memcpy(&record, packet + 16, 4);
    std::memcpy(&paddr, packet + 20, 4);
    std::memcpy(&rpc, packet + 24, 4);
    std::memcpy(&receive, packet + 28, 4);
    std::memcpy(&src, packet + 32, 4);
    std::memcpy(&dst, packet + 36, 4);
    std::memcpy(&size, packet + 40, 4);
    size = fit_copy("SIF other data", dst, src, size);
    std::memcpy(g_ram + (dst & (kRamSize - 1)), iop::ram_ptr(src), size);
    watch_span(dst, size, "sif:other_data");
    Rend r = make_rend(*reinterpret_cast<const Header *>(packet), record, paddr, rpc, receive, 0x8000000Cu);
    send(&r, sizeof r);
}

void command(std::uint32_t src, std::uint32_t size) {
    const std::uint8_t *packet = g_ram + (src & (kRamSize - 1));
    const auto *h = reinterpret_cast<const Header *>(packet);
    if (g_options.trace_iop_modules) {
        std::uint32_t words[14];
        std::memcpy(words, packet, sizeof words);
        if (h->cid == 0x8000000Au) {
            const auto endpoint = g_guest_servers.find(words[13]);
            const std::uint32_t iop_buffer = endpoint == g_guest_servers.end() ? 0u : endpoint->second.buf;
            const bool guest_buffer = iop_buffer != 0;
            const std::uint32_t sid = server_id_of_endpoint(words[13]);
            char server[32] = "unknown server";
            if (sid) std::snprintf(server, sizeof server, "server 0x%08X", sid);
            std::fprintf(stderr, "[sif] call endpoint 0x%08X (%s) fn %u send %u recv %u mode %u %s buffer 0x%08X args",
                         words[13], server, words[8], words[9], words[11], words[12],
                         guest_buffer ? "IOP" : "EE", guest_buffer ? iop_buffer : g_data_addr);
            for (std::uint32_t i = 0; i < 8u && i * 4u < words[9]; ++i) {
                const std::uint32_t value = guest_buffer ? iop::read32(iop_buffer + i * 4u)
                                                         : ee_read<std::uint32_t>(g_data_addr + i * 4u);
                std::fprintf(stderr, " %08X", value);
            }
            std::fprintf(stderr, "\n");
        }
        else if (h->cid == 0x80000009u) std::fprintf(stderr, "[sif] bind 0x%08X\n", words[8]);
        else {
            std::fprintf(stderr, "[sif] command 0x%08X:", h->cid);
            for (int i = 0; i < 14; ++i) std::fprintf(stderr, " %08X", words[i]);
            std::fprintf(stderr, "\n");
        }
    }
    ++g_ee_commands[h->cid];
    switch (h->cid) {
    case 0x80000001u: {
        std::uint32_t index, value;
        std::memcpy(&index, packet + 16, 4);
        std::memcpy(&value, packet + 20, 4);
        g_sreg[index & 31u] = value;
        break;
    }
    case 0x80000002u: {
        std::uint32_t address;
        std::memcpy(&address, packet + 16, 4);
        if (h->opt == 0) {
            g_ee_recv = address & (kRamSize - 1);
        } else {
            std::uint32_t sreg[8] = {sizeof(std::uint32_t) * 6, 0, kCmdSetSreg, 0, 0, 1};
            send(sreg, 24);
        }
        break;
    }
    case 0x80000008u: {
        std::uint32_t words[12];
        std::memcpy(words, packet, sizeof words);
        const std::uint32_t client = words[7], cmd = words[8];
        release_client_iop_rpc_packet(client);
        release_iop_rpc_packet(words[5]);
        if (cmd == 0x80000009u) {
            auto pending = g_iop_binds.find(client);
            if (pending != g_iop_binds.end()) {
                const std::uint32_t server = pending->second.server;
                if (!words[9] && iop::kernel::now() < pending->second.deadline) {
                    if (!pending->second.reported) {
                        pending->second.reported = true;
                        std::fprintf(stderr, "[sif] IOP bind to EE server 0x%08X (client 0x%08X) waits for the server\n", server, client);
                    }
                    // The client keeps its packet pointer (the released packet still reports the call as running to
                    // sceSifCheckStatRpc) and its thread stays blocked; the next request takes a packet again.
                    // A job, not a lambda: an IOP event that is a lambda cannot be saved, so a save state taken while the bind
                    // waits would fail (kernel.cpp).
                    iop::kernel::schedule(iop::kernel::now() + iop::kernel::usec_to_cycles(kIopBindRetryUsec),
                                          iop::kernel::Job{iop::kernel::kJobSifReply, kIopBindRetryJob, client, server});
                    break;
                }
                if (!words[9]) std::fprintf(stderr, "[sif] IOP bind to EE server 0x%08X (client 0x%08X) timed out\n", server, client);
                else if (pending->second.reported)
                    std::fprintf(stderr, "[sif] IOP bind to EE server 0x%08X (client 0x%08X) bound to 0x%08X\n", server, client, words[9]);
                g_iop_binds.erase(pending);
            }
            iop::write32(client + 36, words[9]);
            iop::write32(client + 20, words[10]);
            iop::write32(client + 24, words[11]);
        } else if (cmd == 0x8000000Au) {
            const std::uint32_t end = iop::read32(client + 28);
            if (end) iop::kernel::call_guest(end, iop::read32(client + 32));
        }
        iop::write32(client + 0, 0);
        auto it = g_iop_clients.find(client);
        if (it != g_iop_clients.end()) {
            if (iop::kernel::Thread *t = iop::kernel::thread(it->second)) iop::kernel::wake(*t, 0);
            iop::kernel::reschedule();
            g_iop_clients.erase(it);
        }
        break;
    }
    case 0x80000009u: bind(packet); break;
    case 0x8000000Au: call(packet); break;
    case 0x8000000Cu: other_data(packet); break;
    default: {
        auto it = g_cmd_handlers.find(h->cid);
        if (it == g_cmd_handlers.end()) {
            // The console's IOP drops a packet whose slot holds no handler (ps2sdk iop/system/sifcmd, _sceSifCmdIntrHdlr; the only
            // system slots Sony's SIFCMD and SIFRPC fill are 0, 1, 2, 8, 9, 10 and 12). Data sent with it (sceSifSendCmd's extra
            // transfer, size and IOP address in the header) came in the same DMA list before the packet and is already in IOP memory,
            // so a command that only carries data needs nothing more. DreamWorks Madagascar: Escape 2 Africa (public issue 70) sends
            // 0x80000004 before every module it loads and then 30 times a second: said once per command id here, counted in dump().
            if (g_unhandled_commands[h->cid]++ == 0) {
                std::uint32_t words[8] = {};
                std::memcpy(words, packet, sizeof words);
                std::fprintf(stderr,
                             "[sif] command 0x%08X from the EE has no handler on the IOP: dropped as on the console (packet %u bytes, "
                             "%u bytes of data with it to IOP 0x%08X, opt 0x%X, words %08X %08X %08X %08X, ee buffer 0x%08X); "
                             "later ones are only counted\n",
                             h->cid, h->packet_size(), h->size_word >> 8, h->dest, h->opt, words[4], words[5], words[6], words[7], src);
            }
            break;
        }
        std::memcpy(iop::ram_ptr(kCmdBuffer), packet, std::min<std::uint32_t>(size, 0x400u));
        iop::kernel::call_guest(it->second.handler, kCmdBuffer, it->second.data);
        break;
    }
    }
}

bool g_fileio_async = false;
std::uint32_t g_fileio_result = 0;
char g_loadfile_version[4] = {'3', '0', '0', '0'};

void read_reboot_image(const std::string &image_path) {
    const auto entry = disc::find(image_path);
    if (!entry || entry->directory) return;
    std::vector<std::uint8_t> image((entry->size + disc::kSector - 1) / disc::kSector * disc::kSector);
    if (image.empty() || !disc::read(entry->sector, static_cast<std::uint32_t>(image.size() / disc::kSector), image.data())) return;
    for (std::size_t at = 0; at + 16 <= image.size() && image[at]; at += 16)
        if (std::memcmp(&image[at], "FILEIO\0", 7) == 0) g_fileio_async = true;
    // A FILEIO module older than 2.0 speaks the synchronous protocol, whatever else was found above (follows upstream 8ebb09b)
    static const char kFileio[] = "PsIIfileio  ";
    const auto fileio = std::search(image.begin(), image.end(), kFileio, kFileio + sizeof kFileio - 1);
    if (image.end() - fileio >= static_cast<std::ptrdiff_t>(sizeof kFileio + 3)) {
        const std::string version(fileio + sizeof kFileio - 1, fileio + sizeof kFileio + 3);
        const bool digits = std::all_of(version.begin(), version.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        if (digits && version < "2000") g_fileio_async = false;
        std::fprintf(stderr, "[sif] FILEIO from %s reports %s, %s protocol\n", image_path.c_str(), version.c_str(), g_fileio_async ? "asynchronous" : "synchronous");
    }
    static const char kTag[] = "PsIIloadfile";
    const auto it = std::search(image.begin(), image.end(), kTag, kTag + sizeof kTag - 1);
    if (image.end() - it < static_cast<std::ptrdiff_t>(sizeof kTag + 3)) return;
    std::memcpy(g_loadfile_version, &*(it + sizeof kTag - 1), 4);
    std::fprintf(stderr, "[sif] LOADFILE from %s reports %.4s\n", image_path.c_str(), g_loadfile_version);
}

// The IOP starts booting: SMFLAG shows no SIFINIT, CMDINIT or BOOTEND until the boot time is over (get_reg). On the console the EE
// clears these bits itself just before and after the reset command (ps2sdk's SifIopReset: SifSetReg(SMFLAG, ...)) and the IOP sets
// them again as its modules start. A library may wait for the bit to be down first (a vendor SifIopSync that returns 1 while
// BOOTEND is up, polled in a loop that ends when it returns 0); with the bits always up that loop never ended (docs/ichinen-boot.md).
void begin_boot() { g_boot_end = iop::kernel::now() + iop::kernel::usec_to_cycles(kIopBootUsec); }

// SMFLAG as the EE and the IOP's modules see it. g_smflag keeps what the IOP has raised, so a state saved while the IOP boots
// (not stored) still has the bits when it is loaded.
std::uint32_t smflag() {
    if (!g_boot_end) return g_smflag;
    if (iop::kernel::now() < g_boot_end) return g_smflag & ~kSmflagUp;
    g_boot_end = 0;
    g_smflag |= kSmflagUp;
    return g_smflag;
}

// What the EE sends with ps2sdk's SifIopReset(arg, mode) and the vendor library's sceSifRebootIop(image): SIF command 0x80000003,
// the text length at +16, the mode at +20 and the text at +24 (iopcontrol.c, AFL-2.0). The text is "rom0:UDNL <image>" to start the
// IOP again from an image that replaces the ROM's modules (IOPRP*.IMG), or empty (SifIopReset("", 0)) to start it from the ROM's
// own modules. The console forgets everything the game loaded in both cases; `reboot` wipes the IOP only for the empty text, because
// an image's modules are not read here (only their FILEIO and LOADFILE versions) and the games that reboot with one are the ones
// that work today (docs/iop-reboot-empty.md).
void reboot(std::uint32_t src) {
    constexpr std::uint32_t kCmdReset = 0x80000003u;
    const std::uint8_t *packet = g_ram + (src & (kRamSize - 1));
    std::uint32_t cid, args_size, mode;
    std::memcpy(&cid, packet + 8, 4);
    std::memcpy(&args_size, packet + 16, 4);
    std::memcpy(&mode, packet + 20, 4);
    const std::string path(reinterpret_cast<const char *>(packet + 24), strnlen(reinterpret_cast<const char *>(packet + 24), std::min<std::uint32_t>(args_size, 80)));
    std::fprintf(stderr, "[sif] IOP reboot requested with \"%s\"\n", path.c_str());
    if (path.find("IOPRP") != std::string::npos) g_fileio_async = true;
    if (const auto space = path.find(' '); space != std::string::npos) read_reboot_image(path.substr(space + 1));
    std::string image = path;
    if (image.rfind("rom0:UDNL", 0) == 0) image.erase(0, 9);
    image.erase(0, image.find_first_not_of(' '));
    if (cid != kCmdReset) {
        std::fprintf(stderr, "[sif] that transfer to IOP address 0 carries command 0x%08X, not a reset\n", cid);
        return;
    }
    if (!image.empty()) {
        begin_boot();
        return;
    }
    // The new IOP has none of the game's modules, threads, servers or queued answers: a packet or thread of the old one that survived
    // would write into EE memory the game has reused, and an answer still queued for the EE would meet the new handshake. The EE's
    // view (its receive address, the two flag registers) stays; the boot-end, command and SIF bits of SMFLAG go down and come back
    // when the IOP's boot time is over (begin_boot).
    const std::size_t modules = iop::kernel::modules().size(), threads = iop::kernel::thread_ids().size();
    const std::uint32_t mainaddr = g_mainaddr, msflag = g_msflag, ee_recv = g_ee_recv;
    iop::soft_reset();
    g_mainaddr = mainaddr;
    g_msflag = msflag;
    g_ee_recv = ee_recv;
    begin_boot();
    std::fprintf(stderr, "[sif] the IOP starts from its own modules (mode %u): %zu modules and %zu threads of the game are gone\n", mode, modules, threads);
}

std::uint32_t arg(const iop::Cpu &c, int i) { return i < 4 ? c.r[4 + i] : iop::read32(c.r[29] + 16u + static_cast<std::uint32_t>(i - 4) * 4u); }

void def(const char *library, std::uint32_t index, const char *name, std::function<void(iop::Cpu &)> body) {
    iop::register_hle(library, index, [library, name, body = std::move(body)](iop::Cpu &c) {
        iop::kernel::trace_hle(library, name, c);
        body(c);
    });
}

// Hands a request to its server: the data into the server's buffer, then the server's function is called with `ret` as its return address.
void start_request(iop::Cpu &c, iop::kernel::Thread &t, const Request &request, std::uint32_t ret) {
    const std::uint32_t server = request.call.server_data;
    const Server *found = nullptr;
    if (const auto endpoint = g_guest_servers.find(server); endpoint != g_guest_servers.end()) found = &endpoint->second;
    // A call queued in a state of version 4 to 6 has sid ^ kServerXor as its endpoint (see call()).
    else if (const auto old = g_servers.find(server ^ kServerXor); old != g_servers.end() && !old->second.hle) found = &old->second;
    if (!found) {
        std::fprintf(stderr, "[sif] queued call %u lost endpoint 0x%08X\n", request.call.rpc_number, server);
        reply_call(request.call);
        return;
    }
    const Server &s = *found;
    if (s.buf && !request.data.empty()) {
        iop::watch_copy("RPC data", s.buf, 0, static_cast<std::uint32_t>(request.data.size()), 0);
        std::memcpy(iop::ram_ptr(s.buf), request.data.data(), request.data.size());
    }
    t.rpc_server = server;
    g_held_calls[server] = request.call;
    c.r[4] = request.call.rpc_number;
    c.r[5] = s.buf;
    c.r[6] = request.size;
    c.r[31] = ret;
    c.pc = s.func;
    c.npc = c.pc + 4;
}

// The server's function returned (v0 points at its answer): the answer goes to the EE, and the call is finished.
void finish_request(iop::Cpu &c, iop::kernel::Thread &t) {
    auto it = g_held_calls.find(t.rpc_server);
    if (it == g_held_calls.end()) return;
    const Call call = it->second;
    g_held_calls.erase(it);
    const std::uint32_t result = c.r[2];
    if (iop::kernel::g_trace_modules && result && call.recv_size >= 4) {
        // --trace-iop: the first words the server answers with (the memory card's results are what a save is judged by)
        std::fprintf(stderr, "[sif] reply server 0x%08X fn %u:", server_id_of_endpoint(call.server_data), call.rpc_number);
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(4, call.recv_size / 4); ++i)
            std::fprintf(stderr, " %08X", iop::read32(result + 4 * i));
        std::fprintf(stderr, "\n");
    } else if (iop::kernel::g_trace_modules && !result && call.recv_size) {
        // A function that returns 0 gives the EE nothing back (the receive buffer keeps what the EE had in it): shown, because the other
        // way round, a long list of replies with no line for one call, reads like a call that was never answered.
        std::fprintf(stderr, "[sif] reply server 0x%08X fn %u: the function returned 0, no data goes back (recv %u bytes)\n",
                     server_id_of_endpoint(call.server_data), call.rpc_number, call.recv_size);
    }
    if (call.recv && result && call.recv_size) {
        const std::uint32_t n = fit_copy("SIF RPC reply", call.recv, result, (std::uint64_t{call.recv_size} + 3u) & ~std::uint64_t{3});
        std::memcpy(g_ram + (call.recv & (kRamSize - 1)), iop::ram_ptr(result), n);
        watch_span(call.recv, n, "sif:rpc_finish");
    }
    reply_call(call);
}

void rpc_loop(iop::Cpu &c) {
    iop::kernel::Thread *t = iop::kernel::current();
    if (!t) return;
    auto &queue = g_requests[t->rpc_queue];
    if (queue.empty()) {
        c.pc = g_loop_stub;
        c.npc = c.pc + 4;
        iop::kernel::block(c, iop::kernel::kWaitRpc, static_cast<int>(t->rpc_queue));
        return;
    }
    const Request request = queue.front();
    queue.pop_front();
    start_request(c, *t, request, g_finish_stub);
}

void rpc_finish(iop::Cpu &c) {
    iop::kernel::Thread *t = iop::kernel::current();
    if (!t) return;
    finish_request(c, *t);
    c.pc = g_loop_stub;
    c.npc = c.pc + 4;
}

// sceSifExecRequest: the server's function runs on the calling thread, and its return goes back to the caller of sceSifExecRequest.
void exec_finish(iop::Cpu &c) {
    iop::kernel::Thread *t = iop::kernel::current();
    if (!t) return;
    finish_request(c, *t);
    c.r[2] = 0;
    c.pc = g_exec_return[t->id];
    c.npc = c.pc + 4;
}

void install_iop_side() {
    iop::kernel::register_job(iop::kernel::kJobSifDmaCallback, [](const iop::kernel::Job &j) { iop::kernel::call_guest(j.a, j.b); });
    iop::kernel::register_job(iop::kernel::kJobSifReply, [](const iop::kernel::Job &j) {
        if (j.a == kIopBindRetryJob) {
            const std::uint32_t client = j.b, server = j.c;
            // A state saved while the bind waited does not hold g_iop_binds (the map is not in the state): wait again from here.
            if (!g_iop_binds.count(client))
                g_iop_binds[client] = IopBind{server, iop::kernel::now() + iop::kernel::usec_to_cycles(kIopBindTimeoutUsec), true};
            if (!send_iop_bind(client, server)) {
                std::fprintf(stderr, "[sif] IOP bind to EE server 0x%08X (client 0x%08X) has no request packet\n", server, client);
                g_iop_binds.erase(client);
                finish_iop_bind(client, 0, 0, 0);
            }
            return;
        }
        if (g_hle_replies.empty()) return;
        const Call c = g_hle_replies.front();
        g_hle_replies.pop_front();
        reply_call(c);
    });
    g_loop_stub = iop::kernel::hle_stub(rpc_loop);
    g_finish_stub = iop::kernel::hle_stub(rpc_finish);
    g_exec_stub = iop::kernel::hle_stub(exec_finish);
    g_boot_loop_stub = g_loop_stub;
    g_stubs_shifted = false;
    // The EE sends completion DMA to the request packet's pkt_addr. It must
    // never alias the caller's SifRpcClientData_t, whose buf field lives at +20.
    g_iop_rpc_packets = iop::kernel::kernel_alloc(kIopRpcPacketCount * kIopRpcPacketSize);
    g_iop_rpc_next_packet = 0;
    g_iop_rpc_packet_busy.fill(0);
    g_iop_rpc_packet_reply.fill(0);

    iop::kernel::register_hle_library("sifman", 0x0101);
    def("sifman", 4, "sceSifDma2Init", [](iop::Cpu &c) { c.r[2] = 0; });
    def("sifman", 5, "sceSifInit", [](iop::Cpu &c) { c.r[2] = 0; });
    def("sifman", 6, "sceSifSetDChain", [](iop::Cpu &c) { c.r[2] = 0; });
    auto set_dma = [](iop::Cpu &c, std::uint32_t callback, std::uint32_t data) {
        const std::uint32_t list = arg(c, 0), count = arg(c, 1);
        bool interrupt = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t src = iop::read32(list + i * 16), dst = iop::read32(list + i * 16 + 4), size = iop::read32(list + i * 16 + 8);
            const std::uint32_t attr = iop::read32(list + i * 16 + 12);
            if (g_options.trace_iop_hle) {
                std::uint32_t nonzero = 0;
                for (std::uint32_t k = 0; k < std::min(size, iop::kRamSize); ++k) nonzero += *iop::ram_ptr(src + k) != 0;
                std::fprintf(stderr, "[sif] iop dma 0x%08X -> ee 0x%08X size 0x%X attr 0x%X nonzero %u words %08X %08X %08X %08X\n", src, dst, size, attr,
                             nonzero, iop::read32(src), iop::read32(src + 4), iop::read32(src + 8), iop::read32(src + 12));
            }
            const std::uint32_t n = fit_copy("sceSifSetDma", dst, src, size);
            std::memcpy(g_ram + (dst & (kRamSize - 1)), iop::ram_ptr(src), n);
            watch_span(dst, n, "sif:iop_dma");
            if (attr & kSifDmaIntO) {
                // The data is in EE memory now (as before); the EE's channel 5 interrupt follows in order behind the command packets
                // already on their way, when the EE's channel runs (pump).
                static bool said = false;
                if (!said) {
                    said = true;
                    std::fprintf(stderr, "[sif] an IOP sceSifSetDma asks for the EE's SIF0 interrupt (attr 0x%X, to EE 0x%08X, ra 0x%08X), shown once\n",
                                 attr, dst, c.r[31]);
                }
                g_outgoing.emplace_back();
                interrupt = true;
            }
        }
        c.r[2] = g_iop_dma_id++;
        if (callback) iop::kernel::schedule(iop::kernel::now() + 32, iop::kernel::Job{iop::kernel::kJobSifDmaCallback, callback, data});
        if (interrupt) pump();
    };
    def("sifman", 7, "sceSifSetDma", [set_dma](iop::Cpu &c) { set_dma(c, 0, 0); });
    def("sifman", 8, "sceSifDmaStat", [](iop::Cpu &c) { c.r[2] = static_cast<std::uint32_t>(-1); });
    def("sifman", 29, "sceSifCheckInit", [](iop::Cpu &c) { c.r[2] = 1; });
    def("sifman", 32, "sceSifSetDmaIntr", [set_dma](iop::Cpu &c) { set_dma(c, arg(c, 2), arg(c, 3)); });
    def("sifman", 23, "sceSifGetSMFlag", [](iop::Cpu &c) { c.r[2] = smflag(); });
    def("sifman", 24, "sceSifSetSMFlag", [](iop::Cpu &c) { g_smflag |= arg(c, 0); c.r[2] = 0; });

    iop::kernel::register_hle_library("sifcmd", 0x0101);
    def("sifcmd", 4, "sceSifInitCmd", [](iop::Cpu &c) { c.r[2] = 0; });
    def("sifcmd", 6, "sceSifGetSreg", [](iop::Cpu &c) { c.r[2] = g_sreg[arg(c, 0) & 31u]; });
    def("sifcmd", 7, "sceSifSetSreg", [](iop::Cpu &c) { g_sreg[arg(c, 0) & 31u] = arg(c, 1); c.r[2] = 0; });
    def("sifcmd", 8, "sceSifSetCmdBuffer", [](iop::Cpu &c) { c.r[2] = 0; });
    def("sifcmd", 10, "sceSifAddCmdHandler", [](iop::Cpu &c) {
        g_cmd_handlers[arg(c, 0)] = CmdHandler{arg(c, 1), arg(c, 2)};
        c.r[2] = 0;
    });
    def("sifcmd", 11, "sceSifRemoveCmdHandler", [](iop::Cpu &c) { g_cmd_handlers.erase(arg(c, 0)); c.r[2] = 0; });
    auto send_cmd = [](iop::Cpu &c) {
        const std::uint32_t cid = arg(c, 0), packet = arg(c, 1);
        const std::uint32_t src = arg(c, 3), dst = arg(c, 4), extra = arg(c, 5);
        // A packet starts with its 16-byte header (written below) and lies in the IOP's memory; anything else is refused or cut.
        if (arg(c, 2) < 16) { iop::bad_span("sceSifSendCmd packet", packet, arg(c, 2), "refused (shorter than its header)"); c.r[2] = 0; return; }
        const std::uint32_t size = fit_copy("sceSifSendCmd packet", 0, packet, arg(c, 2));
        if (extra) {
            const std::uint32_t n = fit_copy("sceSifSendCmd data", dst, src, extra);
            std::memcpy(g_ram + (dst & (kRamSize - 1)), iop::ram_ptr(src), n);
            watch_span(dst, n, "sif:send_cmd");
        }
        std::vector<std::uint8_t> bytes(size);
        std::memcpy(bytes.data(), iop::ram_ptr(packet), size);
        const std::uint32_t words[2] = {(size & 0xFFu) | (extra << 8), dst};
        std::memcpy(bytes.data(), words, 8);
        std::memcpy(bytes.data() + 8, &cid, 4);
        send(bytes.data(), size);
        c.r[2] = 1;
    };
    def("sifcmd", 12, "sceSifSendCmd", send_cmd);
    def("sifcmd", 13, "isceSifSendCmd", send_cmd);
    def("sifcmd", 14, "sceSifInitRpc", [](iop::Cpu &c) { c.r[2] = 0; });
    def("sifcmd", 15, "sceSifBindRpc", [](iop::Cpu &c) {
        const std::uint32_t client = arg(c, 0), server = arg(c, 1), mode = arg(c, 2);
        if (!send_iop_bind(client, server)) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        g_iop_binds[client] = IopBind{server, iop::kernel::now() + iop::kernel::usec_to_cycles(kIopBindTimeoutUsec), false};
        c.r[2] = 0;
        if (!(mode & 1u) && iop::kernel::current()) {
            g_iop_clients[client] = iop::kernel::current()->id;
            iop::kernel::block(c, iop::kernel::kWaitRpc, 0);
        }
    });
    def("sifcmd", 16, "sceSifCallRpc", [](iop::Cpu &c) {
        const std::uint32_t client = arg(c, 0), fn = arg(c, 1), mode = arg(c, 2);
        const std::uint32_t send_ptr = arg(c, 3), send_size = arg(c, 4), recv = arg(c, 5), recv_size = arg(c, 6);
        const std::uint32_t end_function = arg(c, 7);
        iop::write32(client + 28, end_function);
        iop::write32(client + 32, arg(c, 8));
        const bool nowait = (mode & 1u) != 0;
        if (nowait) iop::write32(client + 8, 0xFFFFFFFFu); // PS2SDK stores sema_id=-1 for NOWAIT.
        const std::uint32_t rmode = nowait ? static_cast<std::uint32_t>(end_function != 0) : 1u;
        const std::uint32_t destination = iop::read32(client + 20);
        const std::uint32_t ee_destination = destination & (kRamSize - 1u);
        if (send_size) {
            if (!destination || ee_destination >= kRamSize || send_size > kRamSize - ee_destination) {
                std::fprintf(stderr,
                             "[sif:iop-rpc] reject fn %u client 0x%08X EE buffer 0x%08X send 0x%08X from IOP 0x%08X\n",
                             fn, client, destination, send_size, send_ptr);
                c.r[2] = static_cast<std::uint32_t>(-1);
                return;
            }
            if (g_options.trace_iop_modules) {
                std::uint32_t words[4] = {};
                for (std::uint32_t i = 0; i < std::min<std::uint32_t>(4u, send_size / 4u); ++i)
                    words[i] = iop::read32(send_ptr + i * 4u);
                std::fprintf(stderr,
                             "[sif:iop-rpc] fn %u ra 0x%08X client 0x%08X sd 0x%08X send 0x%08X -> EE 0x%08X size 0x%X data %08X %08X %08X %08X\n",
                             fn, c.r[31], client, iop::read32(client + 36), send_ptr, destination, send_size,
                             words[0], words[1], words[2], words[3]);
            }
            std::memcpy(g_ram + ee_destination, iop::ram_ptr(send_ptr), send_size);
            watch_span(destination, send_size, "sif:iop_rpc_send");
        }
        std::uint32_t record_id = 0;
        const std::uint32_t packet_addr = begin_iop_rpc_packet(client, !rmode && recv_size ? recv : 0, record_id);
        if (!packet_addr) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        // sceSifSendCmd carries send_size in the upper header bits and the EE
        // destination in header.dest, in addition to the payload transfer.
        const std::uint32_t packet[14] = {(0x38u & 0xFFu) | (send_size << 8), destination, 0x8000000Au,
                                          0, record_id, packet_addr, iop::read32(client + 4), client, fn, send_size, recv, recv_size, rmode,
                                          iop::read32(client + 36)};
        send(packet, sizeof packet);
        c.r[2] = 0;
        if (!(mode & 1u) && iop::kernel::current()) {
            g_iop_clients[client] = iop::kernel::current()->id;
            iop::kernel::block(c, iop::kernel::kWaitRpc, 0);
        }
    });
    def("sifcmd", 18, "sceSifCheckStatRpc", [](iop::Cpu &c) {
        const std::uint32_t client = arg(c, 0);
        const std::uint32_t packet = iop::read32(client);
        const std::uint32_t phys = iop::phys(packet);
        if (!packet || phys >= iop::kRamSize || phys > iop::kRamSize - kIopRpcPacketSize) {
            c.r[2] = 0;
            return;
        }
        c.r[2] = iop::read32(client + 4u) == iop::read32(packet + 24u) &&
                 (iop::read32(packet + 16u) & 1u) ? 1u : 0u;
    });
    def("sifcmd", 17, "sceSifRegisterRpc", [](iop::Cpu &c) {
        const std::uint32_t sd = arg(c, 0), id = arg(c, 1);
        Server s;
        s.sd = sd;
        s.sid = id;
        s.func = arg(c, 2);
        s.buf = arg(c, 3);
        s.cfunc = arg(c, 4);
        s.cbuf = arg(c, 5);
        s.queue = arg(c, 6);
        // PS2SDK's SifRpcServerData_t is 68 bytes. Its trailing fields are
        // rid (+52), link (+56), next (+60), and base (+64).
        iop::write32(sd + 56, 0);
        iop::write32(sd + 60, 0);
        iop::write32(sd + 0, id);
        iop::write32(sd + 4, s.func);
        iop::write32(sd + 8, s.buf);
        iop::write32(sd + 16, s.cfunc);
        iop::write32(sd + 20, s.cbuf);
        iop::write32(sd + 64, s.queue);
        std::uint32_t tail = iop::read32(s.queue + 8);
        if (!tail) {
            iop::write32(s.queue + 8, sd);
        } else {
            for (std::uint32_t traversed = 0; traversed < 256; ++traversed) {
                if (tail == sd) break;
                const std::uint32_t next = iop::read32(tail + 56);
                if (!next) {
                    iop::write32(tail + 56, sd);
                    break;
                }
                tail = next;
            }
        }
        g_guest_servers[sd] = s;
        auto &registered = g_server_order[id];
        if (std::find(registered.begin(), registered.end(), sd) == registered.end()) registered.push_back(sd);
        const auto active = g_servers.find(id);
        if (active == g_servers.end() || active->second.hle) g_servers[id] = s;
        if (iop::kernel::g_trace_modules) std::fprintf(stderr, "[sif] server 0x%08X registered (func 0x%08X buf 0x%08X)\n", id, s.func, s.buf);
        complete_pending_bind(id);
        c.r[2] = 0;
    });
    def("sifcmd", 19, "sceSifSetRpcQueue", [](iop::Cpu &c) {
        const std::uint32_t qd = arg(c, 0), thread_id = arg(c, 1);
        iop::write32(qd + 0, thread_id);
        iop::write32(qd + 4, 0);
        iop::write32(qd + 8, 0);
        iop::write32(qd + 12, 0);
        iop::write32(qd + 16, 0);
        iop::write32(qd + 20, 0);
        g_queue_threads[qd] = static_cast<int>(thread_id);
        c.r[2] = 0;
    });
    // A server thread that runs its own loop: sceSifGetNextRequest takes the next request of the queue (0 when there is none), a handle
    // that only sceSifExecRequest looks at, and sceSifExecRequest runs the server's function for it. Tekken 5's file server does this,
    // sleeping in between; deliver_request wakes it.
    def("sifcmd", 20, "sceSifGetNextRequest", [](iop::Cpu &c) {
        auto &queue = g_requests[arg(c, 0)];
        if (queue.empty()) { c.r[2] = 0; return; }
        const std::uint32_t handle = 0x00100000u + 0x40u * ++g_next_handle;
        g_taken[handle] = std::move(queue.front());
        queue.pop_front();
        c.r[2] = handle;
    });
    def("sifcmd", 21, "sceSifExecRequest", [](iop::Cpu &c) {
        iop::kernel::Thread *t = iop::kernel::current();
        const auto it = g_taken.find(arg(c, 0));
        if (!t || it == g_taken.end()) { c.r[2] = 0; return; }
        const Request request = std::move(it->second);
        g_taken.erase(it);
        g_exec_return[t->id] = c.r[31];
        start_request(c, *t, request, g_exec_stub);
    });
    def("sifcmd", 22, "sceSifRpcLoop", [](iop::Cpu &c) {
        iop::kernel::Thread *t = iop::kernel::current();
        if (t) t->rpc_queue = arg(c, 0);
        rpc_loop(c);
    });
    def("sifcmd", 23, "sceSifGetOtherData", [](iop::Cpu &c) {
        const std::uint32_t src = arg(c, 1), dst = arg(c, 2), size = arg(c, 3);
        iop::watch_copy("sceSifGetOtherData", dst, src, size, c.r[31]);
        std::memcpy(iop::ram_ptr(dst), g_ram + (src & (kRamSize - 1)), fit_copy("sceSifGetOtherData", src, dst, size));
        c.r[2] = 0;
    });
    def("sifcmd", 24, "sceSifRemoveRpc", [](iop::Cpu &c) {
        const std::uint32_t sd = arg(c, 0);
        const auto endpoint = g_guest_servers.find(sd);
        if (endpoint != g_guest_servers.end()) {
            const std::uint32_t sid = endpoint->second.sid;
            g_guest_servers.erase(endpoint);
            auto order = g_server_order.find(sid);
            if (order != g_server_order.end()) {
                auto &registered = order->second;
                registered.erase(std::remove(registered.begin(), registered.end(), sd), registered.end());
                if (registered.empty()) g_server_order.erase(order);
            }
            auto active = g_servers.find(sid);
            if (active != g_servers.end() && !active->second.hle && active->second.sd == sd) {
                const Server *next = first_guest_server(sid);
                if (next) active->second = *next;
                else g_servers.erase(active);
            }
        }
        c.r[2] = 0;
    });
    def("sifcmd", 25, "sceSifRemoveRpcQueue", [](iop::Cpu &c) { g_queue_threads.erase(arg(c, 0)); c.r[2] = 0; });
    auto send_cmd_intr = [send_cmd](iop::Cpu &c) {
        const std::uint32_t callback = arg(c, 6), data = arg(c, 7);
        send_cmd(c);
        // PADMAN waits on a semaphore signalled by this DMA completion before
        // it starts another poll. Use the same deferred interrupt-context job
        // as sceSifSetDmaIntr; the packet and its extra data are already copied.
        if (c.r[2] && callback)
            iop::kernel::schedule(iop::kernel::now() + 32, iop::kernel::Job{iop::kernel::kJobSifDmaCallback, callback, data});
    };
    def("sifcmd", 28, "sceSifSendCmdIntr", send_cmd_intr);
    def("sifcmd", 29, "isceSifSendCmdIntr", send_cmd_intr);
}

// printf in ps2sdk ends in fioWrite(1, ...): echo stdout and stderr to the log.
void fileio_echo(std::uint32_t fd, std::uint32_t ptr, std::uint32_t size) {
    if (fd != 1 && fd != 2) return;
    ptr &= kRamSize - 1;
    std::fwrite(g_ram + ptr, 1, std::min(size, kRamSize - ptr), stderr);
}

void fileio_reply(std::uint32_t args, std::uint32_t command, const std::vector<std::uint32_t> &body) {
    if (g_fileio_result) {
        const std::uint32_t header[4] = {ee_read<std::uint32_t>(args), command, ee_read<std::uint32_t>(args + 4), ee_read<std::uint32_t>(args + 8)};
        std::memcpy(g_ram + (g_fileio_result & (kRamSize - 1)), header, 16);
        watch_span(g_fileio_result, 16, "sif:fileio_hdr");
        std::memcpy(g_ram + (g_fileio_result & (kRamSize - 1)) + 16, body.data(), body.size() * 4);
        watch_span(g_fileio_result + 16, static_cast<std::uint32_t>(body.size() * 4), "sif:fileio_body");
    }
    const std::uint32_t packet[4] = {16, 0, 0x80000011u, 0};
    send(packet, 16);
}

bool fileio_async(std::uint32_t method, std::uint32_t args, std::uint32_t, std::uint32_t ret, std::uint32_t) {
    const auto word = [args](std::uint32_t offset) { return ee_read<std::uint32_t>(args + offset); };
    const auto text = [args](std::uint32_t offset) { return ee_text(args + offset); };
    store<std::uint32_t>(g_ram + ret, 1);
    switch (method) {
    case 255:
        g_fileio_result = word(0);
        std::memcpy(g_ram + ret, g_loadfile_version, 4);
        watch_span(ret, 4, "sif:loadfile_version");
        return true;
    case 0: {
        const std::int32_t fd = iop::io::open(text(20), word(12));
        std::fprintf(stderr, "[fileio] open %s -> %d\n", text(20).c_str(), fd);
        fileio_reply(args, 0, {static_cast<std::uint32_t>(fd), 0, 0, 0});
        return true;
    }
    case 1:
        fileio_reply(args, 1, {static_cast<std::uint32_t>(iop::io::close(static_cast<int>(word(12)))), 0, 0, 0});
        return true;
    case 2: {
        // Cut to what fits in EE memory from the destination, so a bad size does not ask the host for up to 4 GiB.
        const std::uint32_t size = fit_ee("fileio read", word(16), word(20));
        std::vector<std::uint8_t> buffer(size);
        const int n = iop::io::read(static_cast<int>(word(12)), buffer.data(), size);
        if (n > 0) { std::memcpy(g_ram + (word(16) & (kRamSize - 1)), buffer.data(), static_cast<std::size_t>(n)); watch_span(word(16), static_cast<std::uint32_t>(n), "sif:file_read"); }
        fileio_reply(args, 2, {static_cast<std::uint32_t>(n), 0, 0, 0});
        return true;
    }
    case 3:
        fileio_echo(word(12), word(16), word(20));
        fileio_reply(args, 3, {word(20), 0, 0, 0});
        return true;
    case 4: {
        const std::int64_t pos = iop::io::seek(static_cast<int>(word(12)), static_cast<std::int32_t>(word(16)), static_cast<int>(word(20)));
        fileio_reply(args, 4, {static_cast<std::uint32_t>(pos), 0, 0, 0});
        return true;
    }
    case 12: {
        std::uint32_t mode = 0, fsize = 0;
        const bool found = iop::io::stat(text(16), mode, fsize);
        std::vector<std::uint32_t> body(2 + 10, 0);
        body[0] = found ? 0u : static_cast<std::uint32_t>(-2);
        body[1] = word(12);
        body[2] = mode;
        body[4] = fsize;
        fileio_reply(args, 12, body);
        return true;
    }
    case 23: {
        const std::string device = text(12);
        const std::uint32_t cmd = word(12 + 0x800), out = word(12 + 0x808) & (kRamSize - 1);
        const std::uint32_t result = device.starts_with("cdrom") ? 0u : static_cast<std::uint32_t>(-19);
        if (cmd == 0x4322u) store<std::uint32_t>(g_ram + out, 0x0Au);
        else if (cmd == 0x4325u) store<std::uint32_t>(g_ram + out, 2u);
        else if (cmd == 0x4320u) store<std::uint32_t>(g_ram + out, 0u);
        else if (cmd != 0x430Cu) std::fprintf(stderr, "[fileio] devctl %s 0x%X\n", device.c_str(), cmd);
        fileio_reply(args, 23, {result, 0, 0, 0});
        return true;
    }
    default:
        std::fprintf(stderr, "[fileio] async method %u\n", method);
        fileio_reply(args, method, {static_cast<std::uint32_t>(-1), 0, 0, 0});
        return true;
    }
}

// The FILEIO calls come in two packet layouts. The old one is what ps2sdk's EE library sends (fileio.c, AFL-2.0: struct _fio_open_arg
// is the mode at +0 and the name at +4, _fio_close_arg the fd, _fio_read_arg and _fio_write_arg the fd, the address and the size, _fio_lseek_arg
// the fd, the offset and the whence, _fio_getstat_arg the address of the result at +0 and the name at +4; the answer is the call's own
// reply). The newer one, which the runtime has used since it saw FILEIO 2.05, has a header of three words in front: fd, address, size at
// +12, +16, +20, the open's mode at +12 and name at +20, the stat's result address at +12 and name at +16; it answers into the buffer
// the EE gave with call 255 and announces it with command 0x80000011. A game library can speak the old layout to a FILEIO 2.05
// (Quake III Revolution: "rom0:ROMVER" with "VER" read as the mode), so every packet is told apart by what it holds.
// A device name: letters or digits, at least two, then ':' (rom0:, cdrom0:, mc0:, host:).
bool ee_device_path(std::uint32_t at) {
    std::size_t length = 0;
    for (; length < 16; ++length) {
        const unsigned char ch = g_ram[(at + length) & (kRamSize - 1)];
        if (ch == ':') return length >= 2;
        if (!std::isalnum(ch)) return false;
    }
    return false;
}

// Open and stat carry a name: the old layout has a device name at +4 and no device name where the newer one keeps its name. The calls with
// only numbers (close, read, write, lseek) are old when the packet is too short for the newer layout (or, for write, no longer than the
// old one's 28 bytes), the word at +0 can be a descriptor (the newer layout has the buffer's address there) and it is not that buffer.
bool fileio_old_layout(std::uint32_t method, std::uint32_t args, std::uint32_t size) {
    const std::uint32_t first = ee_read<std::uint32_t>(args);
    const bool descriptor = first < 256 && !(g_fileio_result && first == g_fileio_result);
    switch (method) {
    case 0: return ee_device_path(args + 4) && !ee_device_path(args + 20);
    case 12: return ee_device_path(args + 4) && !ee_device_path(args + 16);
    case 1: return descriptor && size < 16;
    case 2:
    case 4: return descriptor && size < 24;
    case 3: return descriptor && size <= 28;
    default: return false;
    }
}

// LOADFILE call 1, sceSifLoadElf / sceSifLoadElfPart: the IOP reads an EE program from the disc and copies it into the EE's memory
// (public issue 98, Xenosaga Episode I: New Game loads its game program this way and calls into it, which stopped at empty memory
// while this call was not answered). The packet and the answer follow ps2sdk (AFL-2.0, common/include/loadfile-common.h and
// iop/system/loadfile/src/eeelfloader.c): the entry point at +0, the gp at +4, the path at +8 (252 bytes), the section name at +260.
// "all" copies the file contents of every loadable segment (a segment below 0x80000 refuses the file, memory past a segment's file
// contents is left as it is); another name copies that one section's contents to its address. The answer is the entry point and a
// gp of 0, or an entry point of 0 and the error in the fourth word, which the EE library turns into "load failed".
bool load_ee_elf(std::uint32_t args, std::uint32_t ret, std::uint32_t ret_size) {
    const std::string path = ee_text(args + 8, 252);
    const std::string section = ee_text(args + 8 + 252, 252);
    constexpr std::int32_t kFileError = -203, kIllegalObject = -201;   // KE_FILEERR, KE_ILLEGAL_OBJECT
    std::uint32_t answer[4] = {0, 0, 0, 0};
    const auto finish = [&](std::int32_t error, const char *why) {
        if (error) {
            answer[0] = 0;
            answer[3] = static_cast<std::uint32_t>(error);
            std::fprintf(stderr, "[loadfile] EE program %s (%s): %s, not loaded\n", path.c_str(), section.c_str(), why);
        }
        const std::uint32_t n = std::min<std::uint32_t>(ret_size ? ret_size : 8u, sizeof answer);
        for (std::uint32_t i = 0; i < n; ++i) g_ram[(ret + i) & (kRamSize - 1)] = reinterpret_cast<const std::uint8_t *>(answer)[i];
        watch_span(ret, n, "sif:loadfile_elf");
        return true;
    };

    std::vector<std::uint8_t> file;
    std::string lower = path;
    for (char &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.rfind("cdrom", 0) == 0 && disc::is_open())
        if (const auto entry = disc::find(path); entry && !entry->directory && entry->size <= (64u << 20)) {
            file.resize((entry->size + disc::kSector - 1) / disc::kSector * disc::kSector);
            if (!disc::read(entry->sector, static_cast<std::uint32_t>(file.size() / disc::kSector), file.data())) file.clear();
            else file.resize(entry->size);
        }
    if (file.empty()) return finish(kFileError, "cannot read the file");

    // Every read of the file's own headers is checked against its length, in 64 bits.
    bool bad = false;
    const auto u16 = [&](std::uint64_t o) -> std::uint32_t {
        if (o + 2 > file.size()) { bad = true; return 0; }
        return static_cast<std::uint32_t>(file[o] | (file[o + 1] << 8));
    };
    const auto u32 = [&](std::uint64_t o) -> std::uint32_t {
        if (o + 4 > file.size()) { bad = true; return 0; }
        return static_cast<std::uint32_t>(file[o] | (file[o + 1] << 8) | (file[o + 2] << 16) | (file[o + 3] << 24));
    };
    if (file.size() < 0x34 || std::memcmp(file.data(), "\x7F" "ELF", 4) != 0 || u16(0x12) != 8u)
        return finish(kIllegalObject, "not a MIPS ELF file");
    struct Copy { std::uint32_t to, from, size; };
    std::vector<Copy> copies;
    const auto add = [&](std::uint32_t to, std::uint32_t from, std::uint32_t size) {
        const std::uint32_t at = to & 0x1FFFFFFFu;   // kseg0/kseg1 addresses are the same memory
        if (static_cast<std::uint64_t>(from) + size > file.size() || static_cast<std::uint64_t>(at) + size > kRamSize) return false;
        copies.push_back({at, from, size});
        return true;
    };
    if (section == "all") {
        for (std::uint32_t i = 0, n = u16(0x2C); i < n && !bad; ++i) {
            const std::uint64_t ph = static_cast<std::uint64_t>(u32(0x1C)) + static_cast<std::uint64_t>(i) * u16(0x2A);
            if (u32(ph) != 1u || !u32(ph + 16)) continue;
            if (u32(ph + 8) < 0x80000u) return finish(kIllegalObject, "a segment lies below 0x80000");
            if (!add(u32(ph + 8), u32(ph + 4), u32(ph + 16))) return finish(kIllegalObject, "a segment lies outside the file or main memory");
        }
    } else {
        const std::uint64_t table = u32(0x20), entry = u16(0x2E);
        const std::uint64_t names = u32(table + u16(0x32) * entry + 16);
        bool found = false;
        for (std::uint32_t i = 0, n = u16(0x30); i < n && !bad && !found; ++i) {
            const std::uint64_t sh = table + i * entry;
            if (!u32(sh + 20)) continue;
            std::string name;
            for (std::uint64_t o = names + u32(sh); o < file.size() && file[o] && name.size() < 256; ++o) name += static_cast<char>(file[o]);
            if (name != section) continue;
            found = true;
            if (u32(sh + 12) < 0x80000u) return finish(kIllegalObject, "the section lies below 0x80000");
            if (!add(u32(sh + 12), u32(sh + 16), u32(sh + 20))) return finish(kIllegalObject, "the section lies outside the file or main memory");
        }
        if (!found && !bad) return finish(kIllegalObject, "no section of that name");
    }
    if (bad) return finish(kIllegalObject, "damaged headers");
    std::uint32_t bytes = 0;
    for (const Copy &c : copies) {
        std::memcpy(g_ram + c.to, file.data() + c.from, c.size);
        watch_span(c.to, c.size, "sif:loadfile_elf");
        bytes += c.size;
    }
    answer[0] = u32(0x18);
    std::fprintf(stderr, "[loadfile] EE program %s (%s): %zu parts, %u bytes, entry 0x%08X\n", path.c_str(), section.c_str(), copies.size(),
                 bytes, answer[0]);
    return finish(0, nullptr);
}

void install_servers() {
    register_hle_server(0x80000006u, [](std::uint32_t method, std::uint32_t args, std::uint32_t, std::uint32_t ret, std::uint32_t ret_size) {
        if (method == 0x01) return load_ee_elf(args, ret, ret_size);
        if (method == 0x00) {
            const std::string path = ee_text(args + 8);
            const std::uint32_t arg_len = ee_read<std::uint32_t>(args);
            const std::string margs = ee_bytes(args + 8 + 252, std::min<std::uint32_t>(arg_len, 252));
            std::fprintf(stderr, "[loadfile] %s\n", path.c_str());
            const std::int32_t id = iop::load_module(path, margs, [ret](int module, int result) {
                store<std::int32_t>(g_ram + ret, module);
                store<std::int32_t>(g_ram + ret + 4, result);
                complete_call(0x80000006u);
            });
            if (id < 0) {
                store<std::int32_t>(g_ram + ret, id);
                store<std::int32_t>(g_ram + ret + 4, 0);
                return true;
            }
            return false;
        }
        if (method == 0x06) {
            const std::uint32_t ptr = ee_read<std::uint32_t>(args) & (iop::kRamSize - 1);
            const std::uint32_t arg_len = ee_read<std::uint32_t>(args + 4);
            const std::string margs = ee_bytes(args + 8 + 252, std::min<std::uint32_t>(arg_len, 252));
            const std::uint8_t *elf = iop::ram_ptr(ptr);
            // The module's length is worked out from its own headers (the end of the section headers, of each program segment and of
            // each section that has file contents). They come from the game's memory, so every read is checked against the end of
            // the IOP's memory, in 64 bits: a header that lies past it gives 0 and the module is refused, as the disc loader does.
            const std::uint64_t room = iop::kRamSize - ptr;
            bool bad = false;
            const auto u16 = [elf, room, &bad](std::uint64_t o) -> std::uint64_t {
                if (o + 2 > room) { bad = true; return 0; }
                return static_cast<std::uint32_t>(elf[o] | (elf[o + 1] << 8));
            };
            const auto u32 = [elf, room, &bad](std::uint64_t o) -> std::uint64_t {
                if (o + 4 > room) { bad = true; return 0; }
                return static_cast<std::uint32_t>(elf[o] | (elf[o + 1] << 8) | (elf[o + 2] << 16) | (elf[o + 3] << 24));
            };
            std::uint64_t whole = 0;
            if (room >= 4 && std::memcmp(elf, "\x7F" "ELF", 4) == 0) {
                whole = u32(0x20) + u16(0x30) * u16(0x2E);
                for (std::uint32_t i = 0; i < u16(0x2C) && !bad; ++i) {
                    const std::uint64_t ph = u32(0x1C) + i * u16(0x2A);
                    whole = std::max(whole, u32(ph + 4) + u32(ph + 16));
                }
                for (std::uint32_t i = 0; i < u16(0x30) && !bad; ++i) {
                    const std::uint64_t sh = u32(0x20) + i * u16(0x2E);
                    if (u32(sh + 4) != 8u) whole = std::max(whole, u32(sh + 16) + u32(sh + 20));
                }
            }
            if (bad) {
                iop::bad_span("loadfile module headers", ptr, whole, "module refused");
                whole = 0;
            }
            const std::uint32_t size = static_cast<std::uint32_t>(std::min(whole, room));
            std::fprintf(stderr, "[loadfile] buffer at 0x%X, %u bytes\n", ptr, size);
            const std::int32_t id = size ? iop::load_module_buffer(std::vector<std::uint8_t>(elf, elf + size), "buffer", margs, [ret](int module, int result) {
                store<std::int32_t>(g_ram + ret, module);
                store<std::int32_t>(g_ram + ret + 4, result);
                complete_call(0x80000006u);
            }) : -1;
            if (id < 0) {
                store<std::int32_t>(g_ram + ret, id);
                store<std::int32_t>(g_ram + ret + 4, 0);
                return true;
            }
            return false;
        }
        // SearchModuleByName: the id of a module that is loaded (the name sits after two words), else "unknown module".
        if (method == 0x09) {
            const std::string name = ee_text(args + 8, 252);
            std::int32_t found = -202;
            for (const auto &[id, m] : iop::kernel::modules()) {
                if (m.name.size() != name.size()) continue;
                bool same = true;
                for (std::size_t i = 0; i < name.size() && same; ++i)
                    same = std::tolower(static_cast<unsigned char>(m.name[i])) == std::tolower(static_cast<unsigned char>(name[i]));
                if (same) { found = id; break; }
            }
            store<std::int32_t>(g_ram + ret, found);
            return true;
        }
        if (method == 0xFF) {
            std::memcpy(g_ram + ret, g_loadfile_version, 4);
        watch_span(ret, 4, "sif:loadfile_version");
            return true;
        }
        std::fprintf(stderr, "[loadfile] method %u\n", method);
        store<std::int32_t>(g_ram + ret, 0);
        return true;
    });
    register_hle_server(0x80000001u, [](std::uint32_t method, std::uint32_t args, std::uint32_t size, std::uint32_t ret, std::uint32_t ret_size) {
        if (g_fileio_async) {
            if (!fileio_old_layout(method, args, size)) return fileio_async(method, args, size, ret, ret_size);
            static bool told = false;
            if (!told) { told = true; std::fprintf(stderr, "[fileio] a call in the old packet layout under the asynchronous protocol: answered the old way\n"); }
        }
        const auto word = [args](int i) { return ee_read<std::uint32_t>(args + 4u * static_cast<std::uint32_t>(i)); };
        const auto text = [args](int i) { return ee_text(args + 4u * static_cast<std::uint32_t>(i)); };
        std::int32_t result = 0;
        switch (method) {
        case 0:
            result = iop::io::open(text(1), word(0));
            std::fprintf(stderr, "[fileio] open %s -> %d\n", text(1).c_str(), result);
            break;
        case 1: result = iop::io::close(static_cast<int>(word(0))); break;
        case 2: {
            // Cut to what fits in EE memory from the destination, so a bad size does not ask the host for up to 4 GiB.
            const std::uint32_t size = fit_ee("fileio read", word(1), word(2));
            std::vector<std::uint8_t> buffer(size);
            result = iop::io::read(static_cast<int>(word(0)), buffer.data(), size);
            if (result > 0) { std::memcpy(g_ram + (word(1) & (kRamSize - 1)), buffer.data(), static_cast<std::size_t>(result)); watch_span(word(1), static_cast<std::uint32_t>(result), "sif:file_read"); }
            break;
        }
        case 3:
            fileio_echo(word(0), word(1), word(2));
            result = static_cast<std::int32_t>(word(2));
            break;
        case 4: result = static_cast<std::int32_t>(iop::io::seek(static_cast<int>(word(0)), static_cast<std::int32_t>(word(1)), static_cast<int>(word(2)))); break;
        case 12: {
            std::uint32_t mode = 0, size = 0;
            if (iop::io::stat(text(1), mode, size)) {
                const std::uint32_t p = word(0) & (kRamSize - 1);
                std::memset(g_ram + p, 0, 40);
                store<std::uint32_t>(g_ram + p, mode);
                store<std::uint32_t>(g_ram + p + 8, size);
            } else {
                result = -2;
            }
            break;
        }
        case 255: break;
        default:
            std::fprintf(stderr, "[fileio] method %u\n", method);
            result = -1;
            break;
        }
        store<std::int32_t>(g_ram + ret, result);
        return true;
    });
    register_hle_server(0x80000003u, [](std::uint32_t method, std::uint32_t args, std::uint32_t, std::uint32_t ret, std::uint32_t) {
        switch (method) {
        case 1: store<std::uint32_t>(g_ram + ret, iop::kernel::alloc(ee_read<std::uint32_t>(args))); break;
        case 2: iop::kernel::free(ee_read<std::uint32_t>(args)); store<std::uint32_t>(g_ram + ret, 0); break;
        // sceSifLoadIopHeap: the IOP address, then the path of a file to read whole into it.
        // Sound drivers get their bank and sequence data this way.
        case 3: {
            std::uint32_t address = ee_read<std::uint32_t>(args);
            const std::string path(reinterpret_cast<const char *>(g_ram + args + 4));
            std::int32_t result = iop::io::open(path, 1);
            if (result >= 0) {
                const int fd = result;
                std::vector<std::uint8_t> buffer(64u << 10);
                for (int got; (got = iop::io::read(fd, buffer.data(), static_cast<std::uint32_t>(buffer.size()))) > 0; address += static_cast<std::uint32_t>(got))
                    for (int i = 0; i < got; ++i) *iop::ram_ptr(address + static_cast<std::uint32_t>(i)) = buffer[static_cast<std::size_t>(i)];
                iop::io::close(fd);
                result = 0;
            }
            store<std::int32_t>(g_ram + ret, result);
            break;
        }
        // sceSifAllocSysMemory: size first, then the allocation type and address, which a
        // first-fit heap can ignore. Answering 0 here sends the game to wipe IOP memory from 0.
        case 4: store<std::uint32_t>(g_ram + ret, iop::kernel::alloc(ee_read<std::uint32_t>(args))); break;
        case 5: store<std::uint32_t>(g_ram + ret, iop::kRamSize); break;
        case 6: store<std::uint32_t>(g_ram + ret, iop::kernel::free_max()); break;
        case 7: store<std::uint32_t>(g_ram + ret, iop::kernel::free_total()); break;
        default:
            std::fprintf(stderr, "[iopheap] method %u\n", method);
            store<std::uint32_t>(g_ram + ret, 0);
            break;
        }
        return true;
    });
}

}

void reset() {
    g_mainaddr = g_msflag = 0;
    g_smflag = kSmflagUp;
    g_boot_end = 0;
    g_ee_recv = g_data_addr = 0;
    g_processed = true;
    g_outgoing.clear();
    g_servers.clear();
    g_guest_servers.clear();
    g_server_order.clear();
    g_binds.clear();
    g_iop_binds.clear();
    g_held_calls.clear();
    g_hle_replies.clear();
    g_requests.clear();
    g_taken.clear();
    g_exec_return.clear();
    g_queue_threads.clear();
    g_cmd_handlers.clear();
    g_iop_clients.clear();
    std::memset(g_sreg, 0, sizeof g_sreg);
    g_iop_dma_id = g_ee_dma_id = 1;
    g_ee_dma_done = 0;
    g_iop_rpc_id = 1;
    g_iop_rpc_packets = 0;
    g_iop_rpc_next_packet = 0;
    g_iop_rpc_packet_busy.fill(0);
    g_iop_rpc_packet_reply.fill(0);
    g_fileio_async = false;
    g_fileio_result = 0;
}

std::uint32_t get_reg(std::uint32_t reg) {
    switch (reg) {
    case 1: return g_mainaddr;
    case 2: return kCmdBuffer;
    case 3: return g_msflag;
    case 4: return smflag();
    case 0x80000000u: return 0;
    case 0x80000002u: return 1;
    default: return 0;
    }
}

void set_reg(std::uint32_t reg, std::uint32_t value) {
    if (reg == 1) g_mainaddr = value;
    else if (reg == 3) g_msflag |= value;
}

std::uint32_t set_dma(std::uint32_t transfers, std::uint32_t count) {
    if (!count) count = 1;
    std::uint64_t bytes = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t base = transfers + i * 16;
        const std::uint32_t src = ee_read<std::uint32_t>(base) & (kRamSize - 1);
        const std::uint32_t dst = ee_read<std::uint32_t>(base + 4);
        const std::uint32_t size = ee_read<std::uint32_t>(base + 8) & 0x7FFFFFFFu;
        bytes += size;
        if (dst == kRpcRecvAddr) g_data_addr = src;
        else if (dst == 0) reboot(src);
        else if (dst == kCmdBuffer) command(src, size);
        else {
            iop::watch_copy("EE DMA (sceSifSetDma)", dst, src, size, 0);
            std::uint32_t moved = fit_copy("EE sceSifSetDma", src, dst, size);
            // SIF DMA moves whole quadwords, so a size that is not a multiple of 16 still carries the rest of its last quadword
            // (a sound driver keeps its ready flag there; follows upstream 857356a). Still cut at the end of either memory.
            if (moved == size) {
                const std::uint64_t room = std::min<std::uint64_t>(kRamSize - src, iop::kRamSize - (iop::phys(dst) & (iop::kRamSize - 1)));
                moved = static_cast<std::uint32_t>(std::min<std::uint64_t>((std::uint64_t{size} + 15u) & ~std::uint64_t{15}, room));
            }
            std::memcpy(iop::ram_ptr(dst), g_ram + src, moved);
            release_iop_rpc_packet(dst);
        }
    }
    g_ee_dma_done = std::max(g_ee_dma_done, now()) + 256u + bytes * 2u;
    return g_ee_dma_id++;
}

int dma_stat(std::uint32_t id) { return id + 1 == g_ee_dma_id && now() < g_ee_dma_done ? 0 : -1; }

void packet_processed() {
    g_processed = true;
    pump();
}

void pump() {
    const std::uint64_t t = now();
    for (auto it = g_binds.begin(); it != g_binds.end();) {
        if (t > it->second.deadline) {
            std::fprintf(stderr, "[sif] bind to 0x%08X timed out\n", it->first);
            Rend r = it->second.reply;
            r.server_data = 0;
            g_outgoing.emplace_back(reinterpret_cast<const std::uint8_t *>(&r), reinterpret_cast<const std::uint8_t *>(&r) + sizeof r);
            it = g_binds.erase(it);
        } else {
            ++it;
        }
    }
    if (g_options.trace_iop_hle && !g_outgoing.empty() && !g_processed) std::fprintf(stderr, "[sif] %zu packets wait for the EE\n", g_outgoing.size());
    if (!g_processed || g_outgoing.empty() || !g_ee_recv) return;
    if (g_outgoing.front().empty()) {
        // An IOP sceSifSetDma with SIF_DMA_INT_O, its data already copied: only the interrupt. The EE's channel stops after it as after
        // a packet; the game's own channel 5 handler starts it again with isceSifSetDChain (Sony's sifcmd handler does not when no packet
        // came: it skips that call when the packet size is 0, seen in God of War II's EE code and in ps2sdk's _SifCmdIntHandler).
        g_outgoing.pop_front();
        if (g_options.trace_iop_modules) std::fprintf(stderr, "[sif] to EE: SIF0 interrupt of an IOP sceSifSetDma\n");
        g_processed = false;
        hw::dmac_raise(5);
        return;
    }
    // A polling server can return tens of thousands of ordinary RPC_END
    // packets during an intro. Trace requests and unusual commands instead.
    std::uint32_t outgoing_cid = 0;
    if (g_outgoing.front().size() >= 12u)
        std::memcpy(&outgoing_cid, g_outgoing.front().data() + 8, sizeof outgoing_cid);
    if (g_options.trace_iop_modules && outgoing_cid != kCmdRend) {
        std::uint32_t cid;
        std::memcpy(&cid, g_outgoing.front().data() + 8, 4);
        std::fprintf(stderr, "[sif] to EE: command 0x%08X:", cid);
        for (std::size_t i = 0; i < g_outgoing.front().size() / 4 && i < 16; ++i) {
            std::uint32_t w;
            std::memcpy(&w, g_outgoing.front().data() + i * 4, 4);
            std::fprintf(stderr, " %08X", w);
        }
        std::fprintf(stderr, "\n");
    }
    const std::vector<std::uint8_t> packet = std::move(g_outgoing.front());
    g_outgoing.pop_front();
    const std::uint32_t at = g_ee_recv & (kRamSize - 1);
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(packet.size(), kRamSize - at));
    if (n < packet.size()) iop::bad_span("SIF packet to EE", at, packet.size(), "cut at the end of EE memory");
    std::memcpy(g_ram + at, packet.data(), n);
    watch_span(at, n, "sif:pump");
    g_processed = false;
    hw::dmac_raise(5);
}

void register_hle_server(std::uint32_t id, HleServer server) {
    Server s;
    s.hle = true;
    s.sid = id;
    s.invoke = std::move(server);
    g_servers[id] = std::move(s);
    complete_pending_bind(id);
}

void dump() {
    for (const auto &[id, c] : g_held_calls) std::fprintf(stderr, "[sif] endpoint 0x%08X call fn %u is waiting for its reply\n", id, c.rpc_number);
    for (const auto &[queue, list] : g_requests)
        if (!list.empty()) std::fprintf(stderr, "[sif] queue 0x%08X has %zu requests\n", queue, list.size());
    for (const auto &[id, b] : g_binds) std::fprintf(stderr, "[sif] bind to 0x%08X is waiting\n", id);
    std::fprintf(stderr, "[sif] %zu packets wait for the EE (processed %d)\n", g_outgoing.size(), g_processed ? 1 : 0);
    // Who talked to whom during the run, a few lines: which commands and binds the EE sent, which ones no IOP handler took, and what
    // the IOP's modules registered (command handlers, RPC servers). Enough to tell from a player's log how a game's EE side reaches
    // its sound or other IOP module.
    const auto counts = [](const char *title, const std::map<std::uint32_t, std::uint64_t> &m) {
        if (m.empty()) return;
        std::string line;
        char item[40];
        for (const auto &[id, n] : m) {
            std::snprintf(item, sizeof item, " 0x%08X x%llu", id, static_cast<unsigned long long>(n));
            line += item;
        }
        std::fprintf(stderr, "[sif] %s:%s\n", title, line.c_str());
    };
    counts("commands from the EE", g_ee_commands);
    counts("of them with no IOP handler (dropped)", g_unhandled_commands);
    counts("EE binds by server id", g_ee_binds);
    counts("EE calls by server id", g_ee_calls);
    std::string line;
    char item[64];
    for (const auto &[cid, h] : g_cmd_handlers) {
        std::snprintf(item, sizeof item, " 0x%08X at 0x%08X", cid, h.handler);
        line += item;
    }
    if (!line.empty()) std::fprintf(stderr, "[sif] command handlers of IOP modules:%s\n", line.c_str());
    line.clear();
    for (const auto &[sd, s] : g_guest_servers) {
        const auto calls = g_ee_calls.find(s.sid);
        std::snprintf(item, sizeof item, " 0x%08X (function 0x%08X, %llu EE calls)", s.sid, s.func,
                      static_cast<unsigned long long>(calls == g_ee_calls.end() ? 0 : calls->second));
        line += item;
    }
    if (!line.empty()) std::fprintf(stderr, "[sif] RPC servers of IOP modules:%s\n", line.c_str());
}

std::uint64_t unhandled_commands(std::uint32_t cid) {
    const auto it = g_unhandled_commands.find(cid);
    return it == g_unhandled_commands.end() ? 0 : it->second;
}

void complete_call(std::uint32_t id) {
    auto it = g_held_calls.find(id);
    if (it == g_held_calls.end()) return;
    const Call c = it->second;
    g_held_calls.erase(it);
    reply_call(c);
}

}

namespace ps2::iop::kernel {

void install_sif() {
    sif::reset();
    sif::install_iop_side();
    sif::install_servers();
    install_cdvdfsv();
}

}

namespace ps2::sif {

void state(ps2::state::Stream &s) {
    s.tag("sif");
    s.pod(g_mainaddr);
    s.pod(g_msflag);
    s.pod(g_smflag);
    if (s.loading()) g_boot_end = 0;
    s.pod(g_ee_recv);
    s.pod(g_data_addr);
    s.pod(g_processed);
    s.deque(g_outgoing, [&s](std::vector<std::uint8_t> &packet) { s.vec(packet); });
    const auto state_server = [&s](Server &server) {
        s.pod(server.hle);
        s.pod(server.sd);
        s.pod(server.func);
        s.pod(server.buf);
        s.pod(server.queue);
        if (s.version() >= 7) {
            s.pod(server.sid);
            s.pod(server.cfunc);
            s.pod(server.cbuf);
        }
    };
    std::map<std::uint32_t, Server> servers;
    if (s.saving()) servers = g_servers;
    s.map(servers, state_server);
    if (s.loading()) {
        for (auto &[id, server] : servers) {
            if (s.version() < 7) server.sid = id;
            if (!server.hle) continue;
            const auto it = g_servers.find(id);
            if (it == g_servers.end() || !it->second.hle) throw std::runtime_error("an HLE RPC server is missing");
            server.invoke = it->second.invoke;
        }
        g_servers = std::move(servers);
    }
    if (s.version() >= 7) {
        std::map<std::uint32_t, Server> guest_servers;
        if (s.saving()) guest_servers = g_guest_servers;
        s.map(guest_servers, state_server);
        if (s.loading()) g_guest_servers = std::move(guest_servers);
        s.map(g_server_order, [&s](std::vector<std::uint32_t> &order) { s.vec(order); });
    } else if (s.loading()) {
        // Older states stored only the most recently registered server for each
        // SID. Recover that single endpoint as the binding selected in that state.
        g_guest_servers.clear();
        g_server_order.clear();
        for (auto &[sid, server] : g_servers) {
            if (server.hle || !server.sd) continue;
            server.sid = sid;
            g_guest_servers[server.sd] = server;
            g_server_order[sid].push_back(server.sd);
        }
    }
    s.map(g_binds);
    s.map(g_held_calls);
    s.map(g_requests, [&s](std::deque<Request> &queue) {
        s.deque(queue, [&s](Request &r) {
            s.pod(r.call);
            s.pod(r.size);
            s.vec(r.data);
        });
    });
    s.map(g_queue_threads);
    s.map(g_cmd_handlers);
    s.pod(g_sreg);
    s.map(g_iop_clients);
    s.pod(g_iop_rpc_id);
    if (s.version() >= 7) {
        s.pod(g_iop_rpc_packets);
        s.pod(g_iop_rpc_next_packet);
        s.pod(g_iop_rpc_packet_busy);
        s.pod(g_iop_rpc_packet_reply);
    } else if (s.loading()) {
        g_iop_rpc_packets = iop::kernel::kernel_alloc(kIopRpcPacketCount * kIopRpcPacketSize);
        g_iop_rpc_next_packet = 0;
        g_iop_rpc_packet_busy.fill(0);
        g_iop_rpc_packet_reply.fill(0);
    }
    // The IOP's stubs sit where the boot that made the state put them, and the state's memory and threads point at those places. The
    // boot hands out the kernel area one stub after the other. Builds from the AddDrv change (public v0.0.3 to v0.0.5) made the two
    // ioman return stubs first, so the three stubs here lay 32 bytes further on: a state with the loop stub exactly 32 bytes past this
    // boot's (and the finish stub right after it, the exec stub after that) is one of those. Any other state has them where this boot
    // does. The state is judged against this boot's place, not the current one: after a load of the first kind the stubs stay in the
    // shifted places, and a state of the other kind moves them back. Nothing moves when the arrangement is the one the state needs.
    const std::uint32_t booted = g_boot_loop_stub;
    s.pod(g_loop_stub);
    s.pod(g_finish_stub);
    if (s.loading() && booted) {
        const bool shifted = g_loop_stub == booted + 32 && g_finish_stub == g_loop_stub + 16;
        if (shifted != g_stubs_shifted) {
            // Boot: loop, finish, exec at booted + 0, 16, 32 and the ioman stubs behind them; shifted: the ioman stubs first, then the
            // three at booted + 32, 48, 64. All the old places get the state's words back before the new ones get their stubs.
            std::vector<std::pair<std::uint32_t, std::uint32_t>> moves;
            const std::uint32_t from = shifted ? 0 : 32, to = shifted ? 32 : 0;
            for (std::uint32_t i = 0; i < 3; ++i) moves.emplace_back(booted + from + 16 * i, booted + to + 16 * i);
            iop::kernel::plan_io_stubs(moves, shifted ? booted : 0);
            iop::kernel::move_hle_stubs(moves);
            g_stubs_shifted = shifted;
        }
        g_exec_stub = g_finish_stub + 16;  // not in the state; right behind the finish stub in both arrangements
    }
    s.pod(g_iop_dma_id);
    s.pod(g_ee_dma_id);
    s.pod(g_fileio_async);
    s.pod(g_fileio_result);
    // Not part of a state: when the last DMA to the EE is over, in IOP time. A load sets the clock back, so a value from the program
    // that has run on would say "busy" for as long as the clock needs to catch up. A program started to load the state has 0.
    if (s.loading()) g_ee_dma_done = 0;
    // HLE RPC replies that wait out an IOP call's latency: in the state since version 5 (upstream's 4). An older state had none
    // waiting, and a load into a running program must not keep the replies of the time before the load.
    if (s.version() >= 5) s.deque(g_hle_replies, [&s](Call &c) { s.pod(c); });
    else if (s.loading()) g_hle_replies.clear();
}

}
