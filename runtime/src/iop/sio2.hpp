#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::sio2 {

void state(ps2::state::Stream &s);

void reset();
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);
void arm_dma(int channel, std::uint32_t madr, std::uint32_t block_words, std::uint32_t blocks);
void set_buttons(int pad, std::uint16_t pressed_mask);
void set_axis(int pad, int axis, std::uint8_t value);
void set_pressure(int pad, int button, std::uint8_t value);
void set_connected(int pad, bool connected);
std::uint16_t rumble(int pad);
void flush_cards();
void flush_cards_now(bool hurry);
bool card_image(std::vector<std::uint8_t> &out, bool wait);
// a card image (16384 pages of 528 bytes): what is wrong with it, empty when it is whole; the 16 spare bytes of a page's 512 data bytes
std::string card_problem(const std::uint8_t *data, std::size_t size);
void page_ecc(const std::uint8_t *data, std::uint8_t *spare);

}
