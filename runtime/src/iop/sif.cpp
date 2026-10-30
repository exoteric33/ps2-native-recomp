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
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <stdexcept>
#include <map>
#include <vector>

namespace ps2::sif {

namespace {

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

struct CmdHandler {
    std::uint32_t handler, data;
};

std::uint32_t g_mainaddr = 0, g_msflag = 0, g_smflag = 0x60000u;
std::uint32_t g_ee_recv = 0;
std::uint32_t g_data_addr = 0;
bool g_processed = true;
std::deque<std::vector<std::uint8_t>> g_outgoing;
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
std::uint32_t g_loop_stub = 0, g_finish_stub = 0, g_exec_stub = 0;
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
    if (guest == g_guest_servers.end()) {
        const std::uint32_t sid = c.server_data ^ kServerXor;
        const auto it = g_servers.find(sid);
        if (it == g_servers.end() || !it->second.hle) {
            std::fprintf(stderr, "[sif] call %u to unknown endpoint 0x%08X\n", c.rpc_number, c.server_data);
            reply_call(c);
            return;
        }
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
    const Server &s = guest->second;
    Request request{c, c.send_size, {}};
    if (s.buf) {
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
            std::fprintf(stderr, "[sif] call endpoint 0x%08X fn %u send %u recv %u mode %u %s buffer 0x%08X args",
                         words[13], words[8], words[9], words[11], words[12],
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
        release_iop_rpc_packet(words[5]);
        if (cmd == 0x80000009u) {
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
            std::fprintf(stderr, "[sif] command 0x%08X has no handler (from ee buffer 0x%08X)\n", h->cid, src);
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
    static const char kTag[] = "PsIIloadfile";
    const auto it = std::search(image.begin(), image.end(), kTag, kTag + sizeof kTag - 1);
    if (image.end() - it < static_cast<std::ptrdiff_t>(sizeof kTag + 3)) return;
    std::memcpy(g_loadfile_version, &*(it + sizeof kTag - 1), 4);
    std::fprintf(stderr, "[sif] LOADFILE from %s reports %.4s\n", image_path.c_str(), g_loadfile_version);
}

void reboot(std::uint32_t src) {
    const std::uint8_t *packet = g_ram + (src & (kRamSize - 1));
    std::uint32_t args_size;
    std::memcpy(&args_size, packet + 16, 4);
    const std::string path(reinterpret_cast<const char *>(packet + 24), strnlen(reinterpret_cast<const char *>(packet + 24), std::min<std::uint32_t>(args_size, 80)));
    std::fprintf(stderr, "[sif] IOP reboot requested with \"%s\"\n", path.c_str());
    if (path.find("IOPRP") != std::string::npos) g_fileio_async = true;
    if (const auto space = path.find(' '); space != std::string::npos) read_reboot_image(path.substr(space + 1));
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
    const auto endpoint = g_guest_servers.find(server);
    if (endpoint == g_guest_servers.end()) {
        std::fprintf(stderr, "[sif] queued call %u lost endpoint 0x%08X\n", request.call.rpc_number, server);
        reply_call(request.call);
        return;
    }
    const Server &s = endpoint->second;
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
        std::fprintf(stderr, "[sif] reply server 0x%08X fn %u:", call.server_data ^ kServerXor, call.rpc_number);
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(4, call.recv_size / 4); ++i)
            std::fprintf(stderr, " %08X", iop::read32(result + 4 * i));
        std::fprintf(stderr, "\n");
    }
    if (call.recv && result && call.recv_size) {
        std::memcpy(g_ram + (call.recv & (kRamSize - 1)), iop::ram_ptr(result), (call.recv_size + 3u) & ~3u);
        watch_span(call.recv, (call.recv_size + 3u) & ~3u, "sif:rpc_finish");
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
    iop::kernel::register_job(iop::kernel::kJobSifReply, [](const iop::kernel::Job &) {
        if (g_hle_replies.empty()) return;
        const Call c = g_hle_replies.front();
        g_hle_replies.pop_front();
        reply_call(c);
    });
    g_loop_stub = iop::kernel::hle_stub(rpc_loop);
    g_finish_stub = iop::kernel::hle_stub(rpc_finish);
    g_exec_stub = iop::kernel::hle_stub(exec_finish);
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
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t src = iop::read32(list + i * 16), dst = iop::read32(list + i * 16 + 4), size = iop::read32(list + i * 16 + 8);
            if (g_options.trace_iop_hle) {
                std::uint32_t nonzero = 0;
                for (std::uint32_t k = 0; k < size; ++k) nonzero += iop::ram_ptr(src)[k] != 0;
                std::fprintf(stderr, "[sif] iop dma 0x%08X -> ee 0x%08X size 0x%X nonzero %u words %08X %08X %08X %08X\n", src, dst, size, nonzero,
                             iop::read32(src), iop::read32(src + 4), iop::read32(src + 8), iop::read32(src + 12));
            }
            std::memcpy(g_ram + (dst & (kRamSize - 1)), iop::ram_ptr(src), std::min(size, kRamSize - (dst & (kRamSize - 1))));
            watch_span(dst, std::min(size, kRamSize - (dst & (kRamSize - 1))), "sif:iop_dma");
        }
        c.r[2] = g_iop_dma_id++;
        if (callback) iop::kernel::schedule(iop::kernel::now() + 32, iop::kernel::Job{iop::kernel::kJobSifDmaCallback, callback, data});
    };
    def("sifman", 7, "sceSifSetDma", [set_dma](iop::Cpu &c) { set_dma(c, 0, 0); });
    def("sifman", 8, "sceSifDmaStat", [](iop::Cpu &c) { c.r[2] = static_cast<std::uint32_t>(-1); });
    def("sifman", 29, "sceSifCheckInit", [](iop::Cpu &c) { c.r[2] = 1; });
    def("sifman", 32, "sceSifSetDmaIntr", [set_dma](iop::Cpu &c) { set_dma(c, arg(c, 2), arg(c, 3)); });
    def("sifman", 23, "sceSifGetSMFlag", [](iop::Cpu &c) { c.r[2] = g_smflag; });
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
        const std::uint32_t cid = arg(c, 0), packet = arg(c, 1), size = arg(c, 2);
        const std::uint32_t src = arg(c, 3), dst = arg(c, 4), extra = arg(c, 5);
        if (extra) { std::memcpy(g_ram + (dst & (kRamSize - 1)), iop::ram_ptr(src), extra); watch_span(dst, extra, "sif:send_cmd"); }
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
        std::uint32_t record_id = 0;
        const std::uint32_t packet_addr = begin_iop_rpc_packet(client, 0, record_id);
        if (!packet_addr) { c.r[2] = static_cast<std::uint32_t>(-1); return; }
        iop::write32(client + 36, 0);
        iop::write32(client + 8, 0xFFFFFFFFu);
        const std::uint32_t packet[9] = {0x24u, 0, 0x80000009u, 0, record_id, packet_addr,
                                         iop::read32(client + 4), client, server};
        send(packet, sizeof packet);
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
        std::memcpy(iop::ram_ptr(dst), g_ram + (src & (kRamSize - 1)), size);
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
    def("sifcmd", 28, "sceSifSendCmdIntr", send_cmd);
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
    const auto text = [args](std::uint32_t offset) { return std::string(reinterpret_cast<const char *>(g_ram + args + offset)); };
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
        const std::uint32_t size = word(20);
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

void install_servers() {
    register_hle_server(0x80000006u, [](std::uint32_t method, std::uint32_t args, std::uint32_t, std::uint32_t ret, std::uint32_t) {
        if (method == 0x00) {
            const std::string path(reinterpret_cast<const char *>(g_ram + args + 8));
            const std::uint32_t arg_len = ee_read<std::uint32_t>(args);
            const std::string margs(reinterpret_cast<const char *>(g_ram + args + 8 + 252), std::min<std::uint32_t>(arg_len, 252));
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
            const std::string margs(reinterpret_cast<const char *>(g_ram + args + 8 + 252), std::min<std::uint32_t>(arg_len, 252));
            const std::uint8_t *elf = iop::ram_ptr(ptr);
            std::uint32_t size = 0;
            if (std::memcmp(elf, "\x7F" "ELF", 4) == 0) {
                const auto u16 = [elf](std::uint32_t o) { return static_cast<std::uint32_t>(elf[o] | (elf[o + 1] << 8)); };
                const auto u32 = [elf](std::uint32_t o) { return static_cast<std::uint32_t>(elf[o] | (elf[o + 1] << 8) | (elf[o + 2] << 16) | (elf[o + 3] << 24)); };
                size = u32(0x20) + u16(0x30) * u16(0x2E);
                for (std::uint32_t i = 0; i < u16(0x2C); ++i) {
                    const std::uint32_t ph = u32(0x1C) + i * u16(0x2A);
                    size = std::max(size, u32(ph + 4) + u32(ph + 16));
                }
                for (std::uint32_t i = 0; i < u16(0x30); ++i) {
                    const std::uint32_t sh = u32(0x20) + i * u16(0x2E);
                    if (u32(sh + 4) != 8u) size = std::max(size, u32(sh + 16) + u32(sh + 20));
                }
            }
            size = std::min(size, iop::kRamSize - ptr);
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
            const std::string name(reinterpret_cast<const char *>(g_ram + args + 8), strnlen(reinterpret_cast<const char *>(g_ram + args + 8), 252));
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
        if (g_fileio_async) return fileio_async(method, args, size, ret, ret_size);
        const auto word = [args](int i) { return ee_read<std::uint32_t>(args + 4u * static_cast<std::uint32_t>(i)); };
        const auto text = [args](int i) { return std::string(reinterpret_cast<const char *>(g_ram + args + 4u * static_cast<std::uint32_t>(i))); };
        std::int32_t result = 0;
        switch (method) {
        case 0:
            result = iop::io::open(text(1), word(0));
            std::fprintf(stderr, "[fileio] open %s -> %d\n", text(1).c_str(), result);
            break;
        case 1: result = iop::io::close(static_cast<int>(word(0))); break;
        case 2: {
            std::vector<std::uint8_t> buffer(word(2));
            result = iop::io::read(static_cast<int>(word(0)), buffer.data(), word(2));
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
    g_smflag = 0x60000u;
    g_ee_recv = g_data_addr = 0;
    g_processed = true;
    g_outgoing.clear();
    g_servers.clear();
    g_guest_servers.clear();
    g_server_order.clear();
    g_binds.clear();
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
    case 4: return g_smflag;
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
            std::memcpy(iop::ram_ptr(dst), g_ram + src, std::min(size, iop::kRamSize - (iop::phys(dst) & (iop::kRamSize - 1))));
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
    std::memcpy(g_ram + g_ee_recv, packet.data(), packet.size());
    watch_span(g_ee_recv, static_cast<std::uint32_t>(packet.size()), "sif:pump");
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
    if (s.version() >= 8) {
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
    s.pod(g_loop_stub);
    s.pod(g_finish_stub);
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
