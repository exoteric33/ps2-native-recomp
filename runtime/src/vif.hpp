#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::vif {

void state(ps2::state::Stream &s);

void reset();
void reset_vu_side();
// The ITOP VU0 sees (XITOP): the one VIF0 latched at its last MSCAL. VU0 has its own, apart from VU1's TOP and ITOP.
std::uint32_t itop(int unit);
void stats();
// VIF1 as VU1's thread sees it, for the report of a packet that does not end (vu_micro.cpp).
void describe_vu1();
void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
void transfer_words(int unit, const std::uint32_t *words, std::uint32_t count);
bool stalled(int unit);
std::uint32_t read(int unit, std::uint32_t reg);
void write(int unit, std::uint32_t reg, std::uint32_t value);
void flush(int unit);

}
