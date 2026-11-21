#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ps2::gs::queue {

enum Type : std::uint32_t { kWrap, kGif, kGifReset, kReset, kSoftReset, kVblank, kPresent, kDownload, kVif1, kVif1Write, kVuReset, kState, kVuFence, kDownloadAsync, kPath3Idle, kDownloadTrip };

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
    // While holding, a sleeping consumer is woken only once wake_bytes_ (kWakeBytes) have been published since it was found asleep;
    // flush() and a wait for space always wake it, so the producer must flush before it sleeps or stops.
    void hold() { holding_ = true; }
    void flush();
    bool peek(Header &header, const std::uint8_t *&payload);
    void pop(const Header &header);
    void wait_for_work(int timeout_ms);
    void wake();

    static constexpr std::uint64_t kBatchBytes = 16u << 10;
    static constexpr std::uint64_t kWakeBytes = 256u << 10;
    void set_wake_bytes(std::uint64_t bytes) { wake_bytes_ = bytes; }   // after start(); for tests
    // A message whose data is more than a quarter of the ring goes through the heap: the ring carries only its header and a pointer to a
    // copy, which pop() frees. So every message in the ring takes at most a quarter of it, and the
    // producer's wait for space always ends (a message over half the ring could wait for a tail that never comes).
    static constexpr std::uint32_t kSpilled = 0x80000000u;

    // Lending: the consumer of this ring that is also the only producer of `out` (the vector-unit thread: it reads the
    // vu ring and writes the drawing ring) passes data on without copying it. A post to `out` of at least kLendMin bytes whose data lies
    // inside the message this ring's consumer is working on (between its peek() and pop()) carries only a pointer to it (type marked
    // kLent), and this ring keeps that message's space from its producer until `out`'s consumer has popped the pointer. At most a
    // quarter of this ring is kept so (beyond that its consumer waits for `out`'s consumer), so its producer always finds space. Called once by that thread.
    void lend_into(Ring &out);
    static constexpr std::uint32_t kLent = 0x40000000u;
    static constexpr std::uint32_t kLendMin = 256;

private:
    void put(const Header &header, const void *data, std::uint32_t ring_bytes);
    void post_spilled(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    void publish(bool force = false);
    void wait_for_space(std::uint64_t until_tail);
    void wake_producer();
    void post_lent(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    void note_loan(std::uint64_t start);
    void release_loans(bool block);
    std::uint64_t space_end(std::memory_order order) const;   // the producer: where the free space ends (the tail, or the oldest kept message)

    std::unique_ptr<std::uint8_t[]> buffer_;
    std::uint64_t capacity_ = 0, mask_ = 0, spill_above_ = 0, wake_bytes_ = kWakeBytes;
    // the producer's own line: where it writes next, what it last published, the tail it last read
    alignas(64) std::uint64_t write_ = 0, published_ = 0, tail_seen_ = 0;
    bool holding_ = false, wake_owed_ = false;
    std::uint64_t owed_from_ = 0;   // the published position when a held batch first found the consumer asleep and did not wake it
    Ring *lender_ = nullptr;        // lending: the ring whose current message a post may point into (read by every post: on this line)
    // the consumer's own line: where it reads next, the head it last read
    alignas(64) std::uint64_t read_ = 0, head_seen_ = 0;
    std::uint8_t *spilled_ = nullptr;   // the heap copy of the message peek() returned last, freed by pop()
    // the payload of the message peek() returned last (no window: the ring holds a pointer, kSpilled or kLent); lending, the consumer's
    // side: whether a post to the borrower took a pointer into that payload (and the borrower's write position after that post), and the
    // messages kept for the borrower, oldest first
    const std::uint8_t *window_lo_ = nullptr, *window_hi_ = nullptr;
    bool lent_ = false;
    std::uint64_t lent_upto_ = 0, next_check_ = 0;
    Ring *borrower_ = nullptr;
    struct Loan { std::uint64_t start, upto; };   // this ring's message at `start` is kept until the borrower's tail reaches `upto`
    std::unique_ptr<Loan[]> loans_;
    std::uint32_t loan_first_ = 0, loan_end_ = 0;
    static constexpr std::uint32_t kLoans = 1u << 14;
    static constexpr std::uint64_t kLendCheck = 256u << 10;   // how far the consumer reads between looks at the borrower's tail
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::uint64_t> held_{~0ull};   // the start of the oldest message kept for the borrower (none: all ones); written by the consumer
    // the consumer's sleep: it sleeps on posted_ after setting consumer_waiting_; read by the producer after each publish
    alignas(64) std::atomic<std::uint32_t> posted_{0}, consumer_waiting_{0};
    // the producer's sleep when the ring is full: on freed_ after setting producer_waiting_; read by the consumer after each pop
    alignas(64) std::atomic<std::uint32_t> freed_{0}, producer_waiting_{0};
};

// Page guards (queue.cpp): guard() makes whole pages of guard_page() bytes inaccessible (on) or readable and
// writable again (off); a touch of a guarded page calls the handler with the address, which returns true when it has made the access
// possible. False where the system has no guards (then nothing was changed).
bool guard(void *at, std::size_t bytes, bool on);
void on_guard_fault(bool (*handler)(std::uintptr_t address));
std::size_t guard_page();
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
