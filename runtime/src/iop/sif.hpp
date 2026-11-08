#pragma once

#include <cstdint>
#include <functional>

namespace ps2::state {
class Stream;
}

namespace ps2::sif {

void state(ps2::state::Stream &s);

using HleServer = std::function<bool(std::uint32_t method, std::uint32_t args, std::uint32_t args_size, std::uint32_t ret, std::uint32_t ret_size)>;

void reset();
std::uint32_t get_reg(std::uint32_t reg);
void set_reg(std::uint32_t reg, std::uint32_t value);
std::uint32_t set_dma(std::uint32_t transfers, std::uint32_t count);
int dma_stat(std::uint32_t id);
void packet_processed();
void pump();
void dump();
// How many SIF commands with this id came from the EE with no IOP handler for them this run (dropped, as the console does).
std::uint64_t unhandled_commands(std::uint32_t cid);

void register_hle_server(std::uint32_t id, HleServer server);
void complete_call(std::uint32_t id);

}
