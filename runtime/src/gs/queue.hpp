#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ps2::gs::queue {

enum Type : std::uint32_t { kWrap, kGif, kGifReset, kReset, kSoftReset, kVblank, kPresent, kDownload, kVif1, kVif1Write, kVuReset, kState, kVuFence, kDownloadAsync, kPath3Idle };

enum class Role { Emulation, Vu, Render };

struct Header {
    std::uint32_t type, bytes, arg0, arg1;
};

// A ring of messages from one producer thread to one consumer thread. The producer writes a message and
// publishes it by moving the head; the consumer reads it in place and gives its space back by moving the tail after its work.
// Each side keeps its own copy of the other side's position and reads the shared one only when its copy says the ring is full
// (producer) or empty (consumer), so a message costs no cache-line transfer beyond the data and the head itself. A sleeping consumer
// (or a producer waiting for space) is woken through a flag the other side checks after each publish (a Dekker pair of seq_cst
// operations: one locked instruction per publish, none per pop).
class Ring {
public:
    void start(std::size_t capacity);
    bool active() const { return capacity_ != 0; }
    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }  // everything published is done (a message is popped after its work)
    void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    // A batch on the producer side: posts after hold() are written at once but published together, at flush() (or when kBatchBytes
    // have gathered, or before the producer waits for space). Only the producer calls these; a ring nobody holds publishes every post.
    void hold() { holding_ = true; }
    void flush() {
        holding_ = false;
        if (write_ != published_) publish();
    }
    bool peek(Header &header, const std::uint8_t *&payload);
    void pop(const Header &header);
    void wait_for_work(int timeout_ms);
    void wake();

    static constexpr std::uint64_t kBatchBytes = 16u << 10;
    // A message whose data is more than a quarter of the ring goes through the heap: the ring carries only its header and a pointer to a
    // copy, which pop() frees. So every message in the ring takes at most a quarter of it, and the
    // producer's wait for space always ends (a message over half the ring could wait for a tail that never comes).
    static constexpr std::uint32_t kSpilled = 0x80000000u;

private:
    void put(const Header &header, const void *data, std::uint32_t ring_bytes);
    void post_spilled(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    void publish();
    void wait_for_space(std::uint64_t until_tail);
    void wake_producer();

    std::unique_ptr<std::uint8_t[]> buffer_;
    std::uint64_t capacity_ = 0, mask_ = 0, spill_above_ = 0;
    // the producer's own line: where it writes next, what it last published, the tail it last read
    alignas(64) std::uint64_t write_ = 0, published_ = 0, tail_seen_ = 0;
    bool holding_ = false;
    // the consumer's own line: where it reads next, the head it last read
    alignas(64) std::uint64_t read_ = 0, head_seen_ = 0;
    std::uint8_t *spilled_ = nullptr;   // the heap copy of the message peek() returned last, freed by pop()
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    // the consumer's sleep: it sleeps on posted_ after setting consumer_waiting_; read by the producer after each publish
    alignas(64) std::atomic<std::uint32_t> posted_{0}, consumer_waiting_{0};
    // the producer's sleep when the ring is full: on freed_ after setting producer_waiting_; read by the consumer after each pop
    alignas(64) std::atomic<std::uint32_t> freed_{0}, producer_waiting_{0};
};

Ring &render();
Ring &vu();
Role &role();
Ring &outbox();

bool active();
bool vu_active();
void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen);
void notify(std::atomic<std::uint32_t> &value);

}
