// macOS hides the ucontext routines (the crash report reads the registers from them) unless _XOPEN_SOURCE is defined; _DARWIN_C_SOURCE keeps the rest visible.
#if defined(__APPLE__)
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#endif
#include "crash.hpp"

#include "ps2/crash.hpp"

#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <exception>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define PS2_CRASH_BACKTRACE 1
#endif
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
extern "C" char __executable_start, _end;
#endif
#include <dlfcn.h>
#endif

// The crash report: what it writes, and why it is built the way it is (see the notes below and in the functions).
//
// Rules for everything below: it runs in a process that has just crashed, so it takes no lock, uses no heap on the way (static buffers,
// one write per line), and does nothing that costs time before the crash. The names of functions come from the program's own symbol
// table, read from the program file on disk only now (a stripped program just shows module + offset).
namespace {

constexpr int kMaxFrames = 8;  // the log stays short: the place itself and seven callers
constexpr std::size_t kNameMax = 160;

struct Frame {
    std::uint64_t pc;
    std::uint64_t offset;      // pc minus the start of its module
    bool exe;                  // in the program itself (names are looked up for these)
    char module[48];
    char name[kNameMax + 1];   // empty when unknown
    std::uint64_t name_at;     // address of the function the name belongs to
    std::uint64_t base;        // load address of its module (0 when unknown)
    const char *live;          // "live EE code" / "live VU code" when the module is a library the live compilers made, else null
};

struct State {
    volatile long running;
    char what[400];            // the exception in words
    char thread[40];
    unsigned long tid;
    ps2::crash::Where where;
    Frame frames[kMaxFrames];
    int count;
    crash::After after;
    bool stopped;              // abort(), an exception nobody caught, a refused argument (not a fault at one instruction)
    char report[8192];         // every line written, for `after` (it may have to put them into the log file itself)
    std::size_t report_size;
#ifdef _WIN32
    CONTEXT context;
    int skip;                  // frames of the handler itself at the top of a captured context
    const char *head;
    HANDLE ask, done;          // the helper thread that writes the report (it has a stack of its own)
    volatile DWORD reporter;
#endif
};
State s;

// ------------------------------------------------------------------------------------------------------------------------ output

#ifdef _WIN32
void emit(const char *text, std::size_t length) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(length), &written, nullptr);
}
#else
void emit(const char *text, std::size_t length) {
    ssize_t ignored = write(2, text, length);
    (void)ignored;
}
#endif

// A line of text built in place and written with one call (another thread may hold the lock of the C stream, and a half line is worse
// than none).
struct Line {
    char b[1024];
    std::size_t n = 0;
    Line &put(const char *text) {
        while (*text && n < sizeof b - 2) b[n++] = *text++;
        return *this;
    }
    Line &hex(std::uint64_t v, int digits = 1) {
        char t[16];
        int k = 0;
        do {
            t[k++] = "0123456789ABCDEF"[v & 15];
            v >>= 4;
        } while (v);
        while (k < digits) t[k++] = '0';
        while (k && n < sizeof b - 2) b[n++] = t[--k];
        return *this;
    }
    Line &dec(std::uint64_t v) {
        char t[24];
        int k = 0;
        do {
            t[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (k && n < sizeof b - 2) b[n++] = t[--k];
        return *this;
    }
    void end() {
        b[n++] = '\n';
        emit(b, n);
        for (std::size_t i = 0; i < n && s.report_size + 1 < sizeof s.report; ++i) s.report[s.report_size++] = b[i];
        n = 0;
    }
};

void copy_text(char *to, std::size_t size, const char *from) {
    std::size_t i = 0;
    for (; from[i] && i + 1 < size; ++i) to[i] = from[i];
    to[i] = 0;
}

void append_text(char *to, std::size_t size, const char *from) {
    std::size_t at = std::strlen(to);
    for (std::size_t i = 0; from[i] && at + 1 < size; ++i) to[at++] = from[i];
    to[at] = 0;
}

void append_hex(char *to, std::size_t size, std::uint64_t v, int digits) {
    char t[24];
    int k = 0;
    t[k++] = '0';
    t[k++] = 'x';
    char r[16];
    int m = 0;
    do {
        r[m++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    while (m < digits) r[m++] = '0';
    while (m) t[k++] = r[--m];
    t[k] = 0;
    append_text(to, size, t);
}

// ------------------------------------------------------------------------------------------------------------------------ files

struct File {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    long read(std::uint64_t at, void *to, std::size_t bytes) const {
        LARGE_INTEGER p;
        p.QuadPart = static_cast<LONGLONG>(at);
        DWORD got = 0;
        if (!SetFilePointerEx(h, p, nullptr, FILE_BEGIN) || !ReadFile(h, to, static_cast<DWORD>(bytes), &got, nullptr)) return -1;
        return static_cast<long>(got);
    }
    void close() { CloseHandle(h); }
#else
    int fd = -1;
    bool ok() const { return fd >= 0; }
    long read(std::uint64_t at, void *to, std::size_t bytes) const { return static_cast<long>(pread(fd, to, bytes, static_cast<off_t>(at))); }
    void close() { ::close(fd); }
#endif
};

// ------------------------------------------------------------------------------------------------------------------------ names

// Looks for the function every frame of the program itself is in: the nearest symbol at or below its address, in a code section.
struct Query {
    Frame *frame;
    std::uint64_t rva;         // address inside the file's own numbering (no load offset)
    std::uint64_t best;
    bool found;
    std::uint32_t name_offset; // into the string table
    char short_name[9];        // COFF keeps names of up to 8 characters in the symbol itself
    bool is_short;
};
Query queries[kMaxFrames];
int query_count;

void consider(std::uint64_t address, std::uint32_t name_offset, const char *short_name) {
    for (int i = 0; i < query_count; ++i) {
        Query &q = queries[i];
        if (address > q.rva || (q.found && address <= q.best)) continue;
        q.found = true;
        q.best = address;
        q.name_offset = name_offset;
        q.is_short = short_name != nullptr;
        if (short_name) {
            std::memcpy(q.short_name, short_name, 8);
            q.short_name[8] = 0;
        }
    }
}

// Turns a C++ symbol into the name of its function ("_ZN3ps22vu9interpretEv" -> "ps2::vu::interpret"), without the parameter types.
// Written here because the library demangler allocates (and may realloc a buffer): a crash inside the allocator would hang it. Names it
// cannot read (templates, operators, lambdas) end where it stops, followed by "...", or stay as they are.
void demangle(char *name) {
    const char *p = name;
    if (p[0] == '_' && p[1] == '_' && p[2] == 'Z') ++p;  // macOS puts one more underscore in front
    if (p[0] != '_' || p[1] != 'Z') return;
    p += 2;
    char out[kNameMax + 1];
    std::size_t n = 0;
    bool nested = false, stopped = false, local = false;
    if (*p == 'Z') {  // a local entity (a lambda, a static) of a function: that function's name and "{local}"
        local = true;
        ++p;
    }
    if (*p == 'L') ++p;  // internal linkage
    if (*p == 'N') {
        nested = true;
        ++p;
        while (*p == 'K' || *p == 'V' || *p == 'r' || *p == 'R' || *p == 'O') ++p;  // qualifiers of a member function
    }
    for (int parts = 0; *p; ++parts) {
        if (p[0] == 'S' && p[1] == 't') {  // std::
            p += 2;
            for (const char *t = "std::"; *t && n < kNameMax; ++t) out[n++] = *t;
            continue;
        }
        if (*p == 'C' && (p[1] >= '1' && p[1] <= '5')) {  // a constructor: the class name again
            p += 2;
            for (const char *t = "{constructor}"; *t && n < kNameMax; ++t) out[n++] = *t;
            break;
        }
        if (*p == 'D' && (p[1] >= '0' && p[1] <= '2')) {
            p += 2;
            for (const char *t = "{destructor}"; *t && n < kNameMax; ++t) out[n++] = *t;
            break;
        }
        if (*p < '0' || *p > '9') {
            stopped = nested && *p != 'E';
            break;
        }
        std::size_t length = 0;
        while (*p >= '0' && *p <= '9') length = length * 10 + static_cast<std::size_t>(*p++ - '0');
        if (length > std::strlen(p)) return;  // not a name after all
        if (n && n + 2 < kNameMax && out[n - 1] != ':') {
            out[n++] = ':';
            out[n++] = ':';
        }
        if (length == 12 && std::strncmp(p, "_GLOBAL__N_1", 12) == 0) {
            for (const char *t = "(anonymous)"; *t && n < kNameMax; ++t) out[n++] = *t;
        } else {
            for (std::size_t i = 0; i < length && n < kNameMax; ++i) out[n++] = p[i];
        }
        p += length;
        if (!nested) break;
        if (*p == 'B') break;  // an ABI tag
    }
    if (n >= 2 && out[n - 1] == ':' && out[n - 2] == ':') n -= 2;
    if (local)
        for (const char *t = "::{local}"; *t && n < kNameMax; ++t) out[n++] = *t;
    else if (stopped)
        for (const char *t = "..."; *t && n < kNameMax; ++t) out[n++] = *t;
    if (!n) return;
    out[n] = 0;
    copy_text(name, kNameMax + 1, out);
}

void finish_queries(const File &file, std::uint64_t string_table) {
    for (int i = 0; i < query_count; ++i) {
        Query &q = queries[i];
        if (!q.found) continue;
        Frame &f = *q.frame;
        if (q.is_short) {
            copy_text(f.name, sizeof f.name, q.short_name);
        } else {
            std::memset(f.name, 0, sizeof f.name);
            if (file.read(string_table + q.name_offset, f.name, kNameMax) < 0) f.name[0] = 0;
            f.name[kNameMax] = 0;
        }
        f.name_at = q.best;
        demangle(f.name);
    }
}

std::uint64_t module_base_of(const void *address);

// Marks a frame whose module is one of the libraries the live compilers made (ps2::crash::note_live_library): game code compiled while
// the game ran, not part of the program.
void mark_live(Frame &f) {
    f.live = nullptr;
    if (f.exe || !f.base) return;
    int count = 0;
    const ps2::crash::LiveLibrary *libraries = ps2::crash::live_libraries(count);
    for (int i = 0; i < count; ++i) {
        const std::uint64_t base = libraries[i].base ? libraries[i].base : module_base_of(libraries[i].inside);
        if (base == f.base) {
            f.live = libraries[i].kind == ps2::crash::Thread::LiveVu ? "live VU code" : "live EE code";
            return;
        }
    }
}

#ifdef _WIN32

std::uint64_t module_base_of(const void *address) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof info)) return 0;
    return reinterpret_cast<std::uint64_t>(info.AllocationBase);
}

// The program's symbol table is the COFF one the linker leaves in the .exe (llvm-mingw's lld keeps it unless the program is stripped):
// 18 bytes a symbol, names longer than 8 characters in a string table behind it. Nothing is built for this and nothing is read before
// a crash. The scan goes over the whole table once for all frames, in 64 KB pieces.
bool resolve_program(const wchar_t *path, std::uint64_t base) {
    File file;
    file.h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!file.ok()) return false;
    (void)base;
    static unsigned char head[512];
    bool done = false;
    if (file.read(0, head, 64) == 64) {
        const std::uint32_t pe = *reinterpret_cast<std::uint32_t *>(head + 0x3C);
        unsigned char coff[24];
        if (file.read(pe, coff, 24) == 24 && std::memcmp(coff, "PE\0\0", 4) == 0) {
            const std::uint16_t sections = *reinterpret_cast<std::uint16_t *>(coff + 6);
            const std::uint32_t table = *reinterpret_cast<std::uint32_t *>(coff + 12);
            const std::uint32_t symbols = *reinterpret_cast<std::uint32_t *>(coff + 16);
            const std::uint16_t optional = *reinterpret_cast<std::uint16_t *>(coff + 20);
            static std::uint32_t section_address[128];
            static bool section_code[128];
            int usable = sections < 128 ? sections : 128;
            bool headers = table != 0 && symbols != 0 && usable > 0;
            for (int i = 0; headers && i < usable; ++i) {
                unsigned char sh[40];
                if (file.read(static_cast<std::uint64_t>(pe) + 24 + optional + i * 40u, sh, 40) != 40) {
                    headers = false;
                    break;
                }
                section_address[i] = *reinterpret_cast<std::uint32_t *>(sh + 12);
                section_code[i] = (*reinterpret_cast<std::uint32_t *>(sh + 36) & 0x20000020u) != 0;  // code, or executable
            }
            if (headers) {
                constexpr std::size_t kEntries = 3640;
                static unsigned char chunk[kEntries * 18];
                std::uint32_t skip = 0;
                for (std::uint32_t first = 0; first < symbols; first += kEntries) {
                    const std::uint32_t want = symbols - first < kEntries ? symbols - first : static_cast<std::uint32_t>(kEntries);
                    if (file.read(static_cast<std::uint64_t>(table) + first * 18ull, chunk, want * 18ull) != static_cast<long>(want * 18ull)) break;
                    for (std::uint32_t i = 0; i < want; ++i) {
                        if (skip) {
                            --skip;
                            continue;
                        }
                        const unsigned char *e = chunk + i * 18;
                        skip = e[17];
                        const std::int16_t section = *reinterpret_cast<const std::int16_t *>(e + 12);
                        const std::uint16_t type = *reinterpret_cast<const std::uint16_t *>(e + 14);
                        const unsigned char storage = e[16];
                        if (section < 1 || section > usable || !section_code[section - 1] || (storage != 2 && storage != 3) || (type >> 4) != 2) continue;
                        const std::uint64_t address = section_address[section - 1] + static_cast<std::uint64_t>(*reinterpret_cast<const std::uint32_t *>(e + 8));
                        if (*reinterpret_cast<const std::uint32_t *>(e) == 0) consider(address, *reinterpret_cast<const std::uint32_t *>(e + 4), nullptr);
                        else consider(address, 0, reinterpret_cast<const char *>(e));
                    }
                }
                finish_queries(file, static_cast<std::uint64_t>(table) + symbols * 18ull);
                done = true;
            }
        }
    }
    file.close();
    return done;
}

struct LoaderEntry {  // the part of the loader's module record that is the same on every Windows since XP
    LIST_ENTRY in_load_order;
    LIST_ENTRY in_memory_order;
    LIST_ENTRY in_init_order;
    void *base;
    void *entry;
    ULONG size;
    UNICODE_STRING full_name;
    UNICODE_STRING base_name;
};

// Walks the loader's list of modules without taking its lock (GetModuleFileName would).
const LoaderEntry *find_module(std::uint64_t base) {
    PEB *peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    if (!peb || !peb->Ldr) return nullptr;
    LIST_ENTRY *head = &peb->Ldr->InMemoryOrderModuleList;
    int guard = 0;
    for (LIST_ENTRY *at = head->Flink; at && at != head && guard < 512; at = at->Flink, ++guard) {
        const LoaderEntry *entry = CONTAINING_RECORD(at, LoaderEntry, in_memory_order);
        if (reinterpret_cast<std::uint64_t>(entry->base) == base) return entry;
    }
    return nullptr;
}

void fill_module(Frame &f) {
    MEMORY_BASIC_INFORMATION info;
    f.exe = false;
    f.offset = f.pc;
    f.base = 0;
    f.name[0] = 0;
    f.name_at = 0;
    f.live = nullptr;
    copy_text(f.module, sizeof f.module, "?");
    if (!VirtualQuery(reinterpret_cast<void *>(f.pc), &info, sizeof info) || !info.AllocationBase) return;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(info.AllocationBase);
    f.offset = f.pc - base;
    f.base = base;
    const LoaderEntry *entry = find_module(base);
    if (!entry) {
        copy_text(f.module, sizeof f.module, info.Type == MEM_IMAGE ? "(image)" : "(no module)");
        return;
    }
    // the program itself is the first module in the list
    PEB *peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    f.exe = CONTAINING_RECORD(peb->Ldr->InMemoryOrderModuleList.Flink, LoaderEntry, in_memory_order) == entry;
    const wchar_t *name = entry->base_name.Buffer;
    std::size_t length = entry->base_name.Length / 2, i = 0;
    for (; i < length && i + 1 < sizeof f.module; ++i) f.module[i] = name[i] < 127 ? static_cast<char>(name[i]) : '?';
    f.module[i] = 0;
    mark_live(f);
}

void resolve_names() {
    query_count = 0;
    for (int i = 0; i < s.count; ++i) {
        Frame &f = s.frames[i];
        f.name[0] = 0;
        if (!f.exe) continue;
        // a frame that is a return address points after its call: look one byte back so a call at the very end of a function is not
        // taken for the next function
        queries[query_count++] = Query{&f, f.offset - (i ? 1 : 0), 0, false, 0, {}, false};
    }
    if (!query_count) return;
    PEB *peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    const LoaderEntry *program = CONTAINING_RECORD(peb->Ldr->InMemoryOrderModuleList.Flink, LoaderEntry, in_memory_order);
    static wchar_t path[1024];
    const std::size_t length = program->full_name.Length / 2;
    if (length >= 1023) return;
    std::memcpy(path, program->full_name.Buffer, length * 2);
    path[length] = 0;
    resolve_program(path, 0);
}

int capture_stack(CONTEXT *context, int skip) {
    int count = 0;
    for (int step = 0; step < kMaxFrames + skip + 4 && count < kMaxFrames; ++step) {
        const std::uint64_t pc = context->Rip;
        if (!pc) break;
        if (step >= skip) {
            s.frames[count].pc = pc;
            fill_module(s.frames[count]);
            s.frames[count].name[0] = 0;
            s.count = ++count;
        }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &base, nullptr);
        if (!function) {  // a function without unwind data (a leaf that keeps nothing on the stack): the return address is on top
            if (!context->Rsp) break;
            context->Rip = *reinterpret_cast<DWORD64 *>(context->Rsp);
            context->Rsp += 8;
        } else {
            void *handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, function, context, &handler_data, &frame, nullptr);
        }
    }
    return count;
}

void describe(const EXCEPTION_RECORD *r) {
    const DWORD code = r->ExceptionCode;
    s.what[0] = 0;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR: {
        const ULONG_PTR kind = r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0;
        append_text(s.what, sizeof s.what, code == EXCEPTION_IN_PAGE_ERROR ? "in-page error (a file mapped into memory could not be read) " : "access violation ");
        append_text(s.what, sizeof s.what, kind == 1 ? "writing" : kind == 8 ? "executing" : "reading");
        append_text(s.what, sizeof s.what, " address ");
        append_hex(s.what, sizeof s.what, r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0, 1);
        break;
    }
    case EXCEPTION_STACK_OVERFLOW: append_text(s.what, sizeof s.what, "stack overflow (a function called itself too deep or used too much stack)"); break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: append_text(s.what, sizeof s.what, "integer divide by zero"); break;
    case EXCEPTION_INT_OVERFLOW: append_text(s.what, sizeof s.what, "integer overflow"); break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: append_text(s.what, sizeof s.what, "illegal instruction (the processor cannot run this code)"); break;
    case EXCEPTION_PRIV_INSTRUCTION: append_text(s.what, sizeof s.what, "privileged instruction"); break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: append_text(s.what, sizeof s.what, "array bounds exceeded"); break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: append_text(s.what, sizeof s.what, "misaligned data access"); break;
    case EXCEPTION_BREAKPOINT: append_text(s.what, sizeof s.what, "breakpoint (a trap instruction)"); break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: append_text(s.what, sizeof s.what, "floating point divide by zero"); break;
    case EXCEPTION_FLT_INVALID_OPERATION: append_text(s.what, sizeof s.what, "invalid floating point operation"); break;
    case EXCEPTION_FLT_OVERFLOW: append_text(s.what, sizeof s.what, "floating point overflow"); break;
    case 0xC0000374: append_text(s.what, sizeof s.what, "heap corruption detected by the system"); break;
    case 0xC0000409: append_text(s.what, sizeof s.what, "the program stopped itself (a failed safety check, or abort)"); break;
    case 0xE06D7363:
    case 0x20474343: append_text(s.what, sizeof s.what, "a C++ exception nobody caught"); break;  // MSVC's code, and libc++/libunwind's ("GCC ")
    default: append_text(s.what, sizeof s.what, "unknown exception"); break;
    }
    append_text(s.what, sizeof s.what, " [exception code ");
    append_hex(s.what, sizeof s.what, code, 8);
    append_text(s.what, sizeof s.what, "]");
}

#elif defined(__linux__)  // ---------------------------------------------------------------------------------------------- Linux

// Linux: the base comes from dladdr when the library is noted, so this is not needed in the report.
std::uint64_t module_base_of(const void *) { return 0; }

// The same on Linux, with the ELF symbol table (.symtab) of /proc/self/exe.
bool resolve_program(std::uint64_t exe_start) {
    File file;
    file.fd = open("/proc/self/exe", O_RDONLY);
    if (!file.ok()) return false;
    bool done = false;
    static unsigned char head[64];
    if (file.read(0, head, 64) == 64 && std::memcmp(head, "\177ELF", 4) == 0 && head[4] == 2) {
        const std::uint16_t type = *reinterpret_cast<std::uint16_t *>(head + 16);
        const std::uint64_t section_table = *reinterpret_cast<std::uint64_t *>(head + 40);
        const std::uint16_t section_size = *reinterpret_cast<std::uint16_t *>(head + 58);
        const std::uint16_t section_count = *reinterpret_cast<std::uint16_t *>(head + 60);
        const std::uint64_t bias = type == 3 ? exe_start : 0;  // a position independent program is loaded at exe_start, a fixed one at its own addresses
        for (int i = 0; i < query_count; ++i) {
            queries[i].rva = queries[i].frame->pc - (i ? 1 : 0) - bias;
            queries[i].frame->offset = queries[i].frame->pc - bias;
        }
        static unsigned char sections[96 * 64];
        const unsigned count = section_count < 96 ? section_count : 96;
        if (section_size == 64 && file.read(section_table, sections, count * 64ull) == static_cast<long>(count * 64ull)) {
            for (unsigned i = 0; i < count; ++i) {
                const unsigned char *h = sections + i * 64;
                if (*reinterpret_cast<const std::uint32_t *>(h + 4) != 2) continue;  // SHT_SYMTAB
                const std::uint64_t offset = *reinterpret_cast<const std::uint64_t *>(h + 24), size = *reinterpret_cast<const std::uint64_t *>(h + 32);
                const std::uint32_t link = *reinterpret_cast<const std::uint32_t *>(h + 40);
                if (link >= count) break;
                const std::uint64_t strings = *reinterpret_cast<const std::uint64_t *>(sections + link * 64 + 24);
                constexpr std::size_t kEntries = 2730;
                static unsigned char chunk[kEntries * 24];
                const std::uint64_t total = size / 24;
                for (std::uint64_t first = 0; first < total; first += kEntries) {
                    const std::uint64_t want = total - first < kEntries ? total - first : kEntries;
                    if (file.read(offset + first * 24, chunk, want * 24) != static_cast<long>(want * 24)) break;
                    for (std::uint64_t k = 0; k < want; ++k) {
                        const unsigned char *e = chunk + k * 24;
                        const unsigned kind = e[4] & 15;
                        if ((kind != 2 && kind != 10) || *reinterpret_cast<const std::uint16_t *>(e + 6) == 0) continue;  // functions that exist in this file
                        consider(*reinterpret_cast<const std::uint64_t *>(e + 8), *reinterpret_cast<const std::uint32_t *>(e), nullptr);
                    }
                }
                finish_queries(file, strings);
                done = true;
                break;
            }
        }
    }
    file.close();
    return done;
}

// The module of an address outside the program: its line in /proc/self/maps (read here, one line at a time, no heap).
void module_from_maps(Frame &f) {
    copy_text(f.module, sizeof f.module, "?");
    f.offset = f.pc;
    const int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return;
    static char buffer[4096];
    char line[600];
    std::size_t length = 0;
    bool found = false;
    for (;;) {
        const ssize_t got = read(fd, buffer, sizeof buffer);
        if (got <= 0) break;
        for (ssize_t i = 0; i < got && !found; ++i) {
            if (buffer[i] != '\n') {
                if (length + 1 < sizeof line) line[length++] = buffer[i];
                continue;
            }
            line[length] = 0;
            length = 0;
            char *p = line;
            auto number = [&](char stop) {
                std::uint64_t v = 0;
                for (; *p && *p != stop; ++p) v = v * 16 + static_cast<std::uint64_t>(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
                if (*p) ++p;
                return v;
            };
            const std::uint64_t start = number('-'), end = number(' ');
            if (f.pc < start || f.pc >= end) continue;
            while (*p && *p != ' ') ++p;  // permissions
            ++p;
            const std::uint64_t file_offset = number(' ');
            for (int skip = 0; skip < 2; ++skip) {  // device, inode
                while (*p && *p != ' ') ++p;
                while (*p == ' ') ++p;
            }
            const char *path = p, *base = p;
            for (; *p; ++p)
                if (*p == '/') base = p + 1;
            if (*path) copy_text(f.module, sizeof f.module, base);
            f.offset = f.pc - start + file_offset;
            f.base = start - file_offset;  // where the file's first byte is: what dladdr calls the library's base
            found = true;
        }
        if (found) break;
    }
    ::close(fd);
}

void fill_module(Frame &f) {
    const std::uint64_t start = reinterpret_cast<std::uint64_t>(&__executable_start), end = reinterpret_cast<std::uint64_t>(&_end);
    f.name[0] = 0;
    f.name_at = 0;
    f.base = 0;
    f.live = nullptr;
    f.exe = f.pc >= start && f.pc < end;
    if (!f.exe) {
        module_from_maps(f);
        mark_live(f);
        // a library's exported functions (the live compilers export each translated function): dladdr knows them. It may take the
        // loader's lock; the alarm set in on_signal ends the program if that ever hangs.
        Dl_info info{};
        if (f.base && dladdr(reinterpret_cast<void *>(f.pc), &info) && info.dli_sname && info.dli_saddr) {
            copy_text(f.name, sizeof f.name, info.dli_sname);
            demangle(f.name);
            f.name_at = reinterpret_cast<std::uint64_t>(info.dli_saddr) - f.base;
        }
        return;
    }
    char path[256];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
    const char *base = path;
    if (length > 0) {
        path[length] = 0;
        for (const char *p = path; *p; ++p)
            if (*p == '/') base = p + 1;
    } else {
        path[0] = 0;
    }
    copy_text(f.module, sizeof f.module, length > 0 ? base : "program");
    f.offset = f.pc - start;
}

void resolve_names() {
    query_count = 0;
    for (int i = 0; i < s.count; ++i) {
        Frame &f = s.frames[i];
        if (!f.exe) continue;
        queries[query_count++] = Query{&f, 0, 0, false, 0, {}, false};
    }
    if (query_count) resolve_program(reinterpret_cast<std::uint64_t>(&__executable_start));
}

#else  // --------------------------------------------------------------------------------------------- macOS and other systems

// macOS: the system's dladdr gives the module and the nearest symbol it knows (exported ones; a stripped program shows module +
// offset). Not formally safe in a signal handler, but it takes no lock that the game's own code holds.
std::uint64_t module_base_of(const void *) { return 0; }

void fill_module(Frame &f) {
    f.name[0] = 0;
    f.name_at = 0;
    f.base = 0;
    f.live = nullptr;
    f.exe = false;
    f.offset = f.pc;
    copy_text(f.module, sizeof f.module, "?");
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(f.pc), &info)) return;
    const char *base = info.dli_fname ? info.dli_fname : "?";
    for (const char *p = base; *p; ++p)
        if (*p == '/') base = p + 1;
    copy_text(f.module, sizeof f.module, base);
    f.base = reinterpret_cast<std::uint64_t>(info.dli_fbase);
    f.offset = f.pc - f.base;
    if (info.dli_sname && info.dli_saddr) {
        copy_text(f.name, sizeof f.name, info.dli_sname);
        demangle(f.name);
        f.name_at = reinterpret_cast<std::uint64_t>(info.dli_saddr) - f.base;
    }
    mark_live(f);
}

void resolve_names() {}

#endif

// ------------------------------------------------------------------------------------------------------------------------ report

// The name of the calling thread: the one the program gave it, else the system's (Linux keeps one for every thread, e.g. SDL's).
void name_this_thread(char *to, std::size_t size) {
    if (const char *name = ps2::crash::thread_name()) {
        copy_text(to, size, name);
        return;
    }
#ifdef __linux__
    char system[17] = {};
    if (prctl(PR_GET_NAME, system, 0, 0, 0) == 0 && system[0]) {
        copy_text(to, size, "not named by us, system name \"");
        append_text(to, size, system);
        append_text(to, size, "\"");
        return;
    }
#endif
    copy_text(to, size, "not named by us");
}

void put_frame(Line &l, const Frame &f, bool return_address) {
    l.put(f.module).put("+0x").hex(f.offset);
    if (f.name[0]) {
        l.put(" ").put(f.name);
        const std::uint64_t at = f.offset - (return_address ? 1 : 0);  // a return address points behind its call
        if (f.name_at && at >= f.name_at) l.put("+0x").hex(f.offset - f.name_at);
    }
    if (f.live) l.put(" [").put(f.live).put("]");
}

void print_stack(int from) {
    for (int i = from; i < s.count; ++i) {
        Line l;
        l.put("[crash]   called from ");
        put_frame(l, s.frames[i], true);
        l.end();
    }
}

// head: how the first line starts ("the program crashed: "); s.what, s.thread, s.tid, s.where and the frames are filled in. A few lines:
//   [crash] the program crashed: access violation writing address 0x0 [exception code 0xc0000005]
//   [crash] thread: vector unit (VU1) (id 1234)
//   [crash] at: ps2run.exe+0x1a2b3c ps2::vu::interpret+0x4c
//   [crash] game: frame 56012, EE pc 0x0012a4c0 ra 0x0012a3f8 sp 0x01ffe2c0, game thread 3, last system call 0x3c from 0x00100a20
//   [crash]   called from ...   (up to seven)
// After abort() or an exception nobody caught, the innermost frames are the C library's own way to the end (raise, abort, terminate):
// they are dropped, so "at:" names the code that gave up. Nothing is dropped when every frame is such a frame.
bool system_module(const char *module) {
    static const char *const kSystem[] = {"libc.so", "libc-", "libstdc++", "libc++", "libgcc", "libsystem_", "ucrtbase", "msvcrt",
                                          "KERNELBASE", "kernelbase", "ntdll", "libunwind"};
    for (const char *prefix : kSystem)
        if (std::strncmp(module, prefix, std::strlen(prefix)) == 0) return true;
    return false;
}

void drop_system_frames() {
    int first = 0;
    while (first < s.count && (system_module(s.frames[first].module) || std::strncmp(s.frames[first].name, "crash::", 7) == 0 ||
                                 std::strncmp(s.frames[first].name, "_Unwind_", 8) == 0))
        ++first;
    if (first == 0 || first == s.count) return;
    for (int i = first; i < s.count; ++i) s.frames[i - first] = s.frames[i];
    s.count -= first;
}

void write_report(const char *head, bool stack) {
    Line l;
    l.put("[crash] ").put(head).put(s.what).end();
    l.put("[crash] thread: ").put(s.thread).put(" (id ").dec(s.tid).put(")").end();
    if (stack) resolve_names();
    if (stack && s.stopped) drop_system_frames();
    if (stack && s.count) {
        l.put("[crash] at: ");
        put_frame(l, s.frames[0], false);
        if (s.frames[0].live) l.put(" (game code compiled while the game ran)");
        l.end();
    }
    const ps2::crash::Where &w = s.where;
    if (w.frame || w.pc || w.ra) {
        l.put("[crash] game: frame ").dec(w.frame).put(", EE pc 0x").hex(w.pc, 8).put(" ra 0x").hex(w.ra, 8).put(" sp 0x").hex(w.sp, 8);
        l.put(", game thread ").dec(static_cast<std::uint64_t>(w.ee_thread < 0 ? 0 : w.ee_thread));
        if (w.syscall_known) l.put(", last system call 0x").hex(w.syscall, 2).put(" from 0x").hex(w.syscall_pc, 8);
        l.end();
    } else {
        l.put("[crash] game: not started yet").end();
    }
    if (stack) print_stack(1);
}

// A second crash while the report is written: what was found so far, as it is.
void report_failed() {
    Line l;
    l.put("[crash] the crash report itself failed; what it had found:").end();
    for (int i = 0; i < s.count; ++i) {
        l.put("[crash]   ").put(s.frames[i].module).put("+0x").hex(s.frames[i].offset).end();
    }
}

void begin(const char *what) {
    s.report_size = 0;
    s.stopped = what != nullptr;
    s.count = 0;
    name_this_thread(s.thread, sizeof s.thread);
    s.where = ps2::crash::where();
    if (what) copy_text(s.what, sizeof s.what, what);
}

#ifdef _WIN32

// The report is written by a helper thread started with the handlers: the crashing thread may have almost no stack left (a stack
// overflow leaves the few KB SetThreadStackGuarantee keeps), and a fault inside the report must not end the process before the log
// has anything. The crashing thread fills in what only it knows (its id, name, registers), wakes the helper and waits.
void write_on_this_thread() {
    s.reporter = GetCurrentThreadId();
    capture_stack(&s.context, s.skip);
    write_report(s.head, true);
    if (s.after) s.after(true, s.report, s.report_size);
}

DWORD WINAPI helper(void *) {
    for (;;) {
        if (WaitForSingleObject(s.ask, INFINITE) != WAIT_OBJECT_0) return 0;
        write_on_this_thread();
        SetEvent(s.done);
    }
}

// The first failure writes the report; a second thread that fails at the same time waits for it (the process must not end in the middle
// of the report); a failure inside the report itself (the reporting thread comes back here) only says what was found so far.
void run(const char *head) {
    s.head = head;
    if (s.ask && s.done) {
        SetEvent(s.ask);
        WaitForSingleObject(s.done, 15000);
    } else {
        write_on_this_thread();
    }
}

bool enter() {
    if (InterlockedExchange(&s.running, 1) != 0) {
        if (s.reporter == GetCurrentThreadId()) report_failed();
        else if (s.done) WaitForSingleObject(s.done, 15000);
        return false;
    }
    s.tid = GetCurrentThreadId();
    return true;
}

LONG WINAPI on_exception(EXCEPTION_POINTERS *info) {
    if (!enter()) return EXCEPTION_CONTINUE_SEARCH;
    begin(nullptr);
    describe(info->ExceptionRecord);
    s.stopped = info->ExceptionRecord->ExceptionCode == 0xE06D7363 || info->ExceptionRecord->ExceptionCode == 0x20474343;  // thrown, not faulted
    s.context = *info->ContextRecord;
    s.skip = 0;
    run(s.stopped ? "the program stopped: " : "the program crashed: ");
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort(), an exception nobody caught and a refused C runtime argument end the program without an exception the filter sees.
[[noreturn]] void report_text(const char *what) {
    if (enter()) {
        begin(what);
        RtlCaptureContext(&s.context);
        s.skip = 2;  // RtlCaptureContext's caller (this function) and the handler that called it
        run("the program stopped: ");
    }
    std::_Exit(3);
}

void on_abort(int) { report_text("abort() was called (a failed check, an exception nobody caught, or an allocation that failed)"); }

void on_terminate() {
    static char what[300];
    copy_text(what, sizeof what, "an exception nobody caught: ");
    try {
        if (const auto current = std::current_exception()) std::rethrow_exception(current);
    } catch (const std::exception &e) {
        append_text(what, sizeof what, e.what());
    } catch (...) {
        append_text(what, sizeof what, "(not a std::exception)");
    }
    report_text(what);
}

void on_invalid_parameter(const wchar_t *, const wchar_t *function, const wchar_t *, unsigned int, uintptr_t) {
    static char what[200];
    copy_text(what, sizeof what, "the C runtime refused an argument (in ");
    char name[120] = "?";
    if (function) WideCharToMultiByte(CP_UTF8, 0, function, -1, name, sizeof name - 1, nullptr, nullptr);
    append_text(what, sizeof what, name);
    append_text(what, sizeof what, ")");
    report_text(what);
}

void thread_hook() {
    ULONG guarantee = 128 * 1024;  // as reserve_crash_stack (host/big_stack_thread.hpp); stack kept in reserve so the exception filter can run after a stack overflow
    SetThreadStackGuarantee(&guarantee);
}

#else

void describe(int sig, const siginfo_t *info, const ucontext_t *context, std::uint64_t &fault_pc, const char *&head) {
    s.what[0] = 0;
    head = "the program crashed: ";
    std::uint64_t stack_pointer = 0;
    bool write_access = false, fetch = false, known_access = false;
#if defined(__linux__) && defined(__x86_64__)
    if (context) {
        fault_pc = static_cast<std::uint64_t>(context->uc_mcontext.gregs[REG_RIP]);
        stack_pointer = static_cast<std::uint64_t>(context->uc_mcontext.gregs[REG_RSP]);
        const std::uint64_t error = static_cast<std::uint64_t>(context->uc_mcontext.gregs[REG_ERR]);
        write_access = (error & 2) != 0;
        fetch = (error & 16) != 0;
        known_access = true;
    }
#elif defined(__linux__) && defined(__aarch64__)
    if (context) {
        fault_pc = static_cast<std::uint64_t>(context->uc_mcontext.pc);
        stack_pointer = static_cast<std::uint64_t>(context->uc_mcontext.sp);
    }
#elif defined(__APPLE__) && defined(__x86_64__)
    if (context) {
        fault_pc = static_cast<std::uint64_t>(context->uc_mcontext->__ss.__rip);
        stack_pointer = static_cast<std::uint64_t>(context->uc_mcontext->__ss.__rsp);
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    if (context) {
        fault_pc = static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss));
        stack_pointer = static_cast<std::uint64_t>(__darwin_arm_thread_state64_get_sp(context->uc_mcontext->__ss));
    }
#else
    (void)context;
#endif
    const std::uint64_t address = reinterpret_cast<std::uint64_t>(info ? info->si_addr : nullptr);
    switch (sig) {
    case SIGSEGV:
        if (stack_pointer && address + 65536 >= stack_pointer && address <= stack_pointer + 4096) {
            append_text(s.what, sizeof s.what, "stack overflow (a function called itself too deep or used too much stack) at stack address ");
        } else {
            append_text(s.what, sizeof s.what, "access violation ");
            append_text(s.what, sizeof s.what, !known_access ? "at" : fetch ? "executing" : write_access ? "writing" : "reading");
            append_text(s.what, sizeof s.what, info && info->si_code == SEGV_ACCERR ? " (no permission) address " : " address ");
        }
        append_hex(s.what, sizeof s.what, address, 1);
        break;
    case SIGBUS:
        append_text(s.what, sizeof s.what, "bus error (a memory-mapped file could not be read, or a misaligned access) at address ");
        append_hex(s.what, sizeof s.what, address, 1);
        break;
    case SIGILL: append_text(s.what, sizeof s.what, "illegal instruction (the processor cannot run this code)"); break;
    case SIGTRAP: append_text(s.what, sizeof s.what, "trap instruction"); break;
    case SIGFPE:
        append_text(s.what, sizeof s.what, !info ? "arithmetic error" : info->si_code == FPE_INTDIV ? "integer divide by zero" : info->si_code == FPE_INTOVF ? "integer overflow"
                                                         : info->si_code == FPE_FLTDIV ? "floating point divide by zero" : "arithmetic error");
        break;
    case SIGABRT:
        head = "the program stopped: ";
        append_text(s.what, sizeof s.what, "abort() was called (a failed check, an exception nobody caught, or an allocation that failed)");
        break;
    case SIGHUP:
        head = "the program was ended: ";
        append_text(s.what, sizeof s.what, "the terminal it ran in was closed");
        break;
    default: append_text(s.what, sizeof s.what, "a fatal signal"); break;
    }
    append_text(s.what, sizeof s.what, " [signal ");
    char number[8];
    std::snprintf(number, sizeof number, "%d", sig);
    append_text(s.what, sizeof s.what, number);
    append_text(s.what, sizeof s.what, "]");
}

unsigned long this_thread_id() {
#ifdef __linux__
    return static_cast<unsigned long>(syscall(SYS_gettid));
#else
    return reinterpret_cast<unsigned long>(pthread_self());
#endif
}

[[noreturn]] void end_with(int sig) {
    signal(sig, SIG_DFL);
    raise(sig);  // what the signal would have done: a core dump, if the system makes one
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);  // the signal is blocked while its handler runs: let it through now
    _exit(128 + sig);
}

void on_signal(int sig, siginfo_t *info, void *context) {
    if (__sync_lock_test_and_set(&s.running, 1) != 0) {
        // a second failure: in the report itself (say what was found), or in another thread at the same time (wait for the report)
        if (s.tid == this_thread_id()) {
            report_failed();
            end_with(sig);
        }
        for (;;) pause();  // the reporting thread ends the process (or the alarm it set)
    }
    s.tid = this_thread_id();
    alarm(10);  // the report never hangs the program: if anything in it waits for good, SIGALRM ends the process in 10 seconds
    begin(nullptr);
    std::uint64_t fault_pc = 0;
    const char *head = "";
    describe(sig, info, static_cast<const ucontext_t *>(context), fault_pc, head);
    s.stopped = sig == SIGABRT;
    const bool crashed = sig != SIGHUP;
#ifdef PS2_CRASH_BACKTRACE
    if (crashed) {
        void *pcs[kMaxFrames + 8];
        const int total = backtrace(pcs, kMaxFrames + 8);
        int first = 0;  // the frames of this handler and the signal return are not interesting: start at the instruction that failed
        for (int i = 0; i < total; ++i)
            if (reinterpret_cast<std::uint64_t>(pcs[i]) == fault_pc) {
                first = i;
                break;
            }
        if (fault_pc && (first == 0 || first >= total)) {  // not found (no unwind data through the signal frame): the place alone
            s.frames[0].pc = fault_pc;
            fill_module(s.frames[0]);
            s.count = 1;
            first = total;
        }
        for (int i = first; i < total && s.count < kMaxFrames; ++i) {
            s.frames[s.count].pc = reinterpret_cast<std::uint64_t>(pcs[i]);
            fill_module(s.frames[s.count]);
            ++s.count;
        }
    }
#else
    if (crashed && fault_pc) {
        s.frames[0].pc = fault_pc;
        fill_module(s.frames[0]);
        s.count = 1;
    }
#endif
    write_report(head, crashed);
    if (s.after) s.after(crashed, s.report, s.report_size);
    end_with(sig);
}

// Every named thread gets a second stack for the handler, so a stack overflow can still be reported.
void thread_hook() {
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) return;  // already has one
    constexpr std::size_t kSize = 256 * 1024;
    void *memory = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return;
    stack_t stack{};
    stack.ss_sp = memory;
    stack.ss_size = kSize;
    sigaltstack(&stack, nullptr);
}

#endif

}  // namespace

namespace crash {

void install(After after) {
    s.after = after;
    ps2::crash::set_thread_hook(thread_hook);
    thread_hook();  // the thread that installs (the main thread)
#ifdef _WIN32
    s.ask = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (s.ask && s.done) {
        if (const HANDLE h = CreateThread(nullptr, 512 * 1024, helper, nullptr, 0, nullptr)) CloseHandle(h);
        else s.ask = nullptr;
    }
    SetUnhandledExceptionFilter(on_exception);
    std::signal(SIGABRT, on_abort);
    std::set_terminate(on_terminate);
    _set_invalid_parameter_handler(on_invalid_parameter);
#else
#ifdef PS2_CRASH_BACKTRACE
    void *warm[2];
    backtrace(warm, 2);  // the first call loads the unwinder (and may allocate): not in the handler
#endif
    struct sigaction action{};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT, SIGHUP}) sigaction(sig, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // a log pipe whose reader went away makes a write fail instead of ending the run without a word
    std::set_terminate([] {
        // say what the exception was before abort() reports the rest (the text goes to the log the usual way: nothing is broken yet)
        try {
            if (const auto current = std::current_exception()) std::rethrow_exception(current);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "[crash] an exception nobody caught: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[crash] an exception nobody caught (not a std::exception)\n");
        }
        std::fflush(stderr);
        std::abort();
    });
#endif
}

}
