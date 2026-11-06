#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::dmac {

void state(ps2::state::Stream &s);

void reset();
void stats();
bool owns(std::uint32_t a);
bool cpcond0();
void enable_write(std::uint32_t v);
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);
void resume(int ch);
bool ipu_pull(std::uint8_t *qword);
bool ipu_push(const std::uint8_t *qword);
std::size_t ipu_push_many(const std::uint8_t *qwords, std::size_t count);

}
