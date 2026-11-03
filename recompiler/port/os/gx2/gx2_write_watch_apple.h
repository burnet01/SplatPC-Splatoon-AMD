// gx2_write_watch_apple.h - Windows GetWriteWatch semantics on macOS.
//
// Included by gx2_write_watch.cpp only, which keeps its Windows logic
// (batch epochs, collection windows, range memoization) unchanged on top of
// this GetWriteWatch replacement.
//
// macOS has no dirty-page query. A page counts as written until a query
// resets it; the reset write-protects it, and the next CPU store takes a
// protection fault whose handler makes it writable again and marks it
// written. Measured on an M5 (2026-10-30): a fault plus its unprotect costs
// ~3 us per 16 KiB page, reprotecting ~0.6 us, comparing the page's bytes
// ~0.3 us. Write watch costs nothing per store on Windows, so protection must
// not be applied to memory the game writes all the time: a page found written
// at kHotCollections consecutive resets stays writable and is reported as
// written at every query (what Windows reports for a page written every
// frame); every kHotRetry resets it is protected once more to see whether it
// went quiet.
//
// Host pages are 16 KiB, guest write-watch pages 4 KiB. Resetting a host page
// for a query that covers only some of its guest pages keeps the others
// pending (s_pending), so no query loses a write another range needs.
#pragma once
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include "../../runtime/host_cpu.h"

using ULONG_PTR = uintptr_t;
using DWORD = uint32_t;
using SIZE_T = size_t;
constexpr DWORD WRITE_WATCH_FLAG_RESET = 1;

namespace gx2_apple_ww {
constexpr uint64_t kGuestPage = 4096;
constexpr uint8_t kHotCollections = 3, kHotRetry = 64;
enum : uint8_t { kInaccessible = 0, kWritten = 1, kProtected = 2 };

uint8_t* s_arena = nullptr;
uint64_t s_arena_size = 0;
uint64_t s_host_page = 0;     // bytes
uint64_t s_ratio = 1;         // guest write-watch pages per host page
std::unique_ptr<std::atomic<uint8_t>[]> s_state;   // per host page
std::unique_ptr<uint8_t[]> s_heat, s_age;          // per host page, under s_protect
std::unique_ptr<uint16_t[]> s_pins;                // per host page, under s_protect
std::unique_ptr<uint64_t[]> s_pending;             // per guest page bit, under s_protect
struct sigaction s_previous_bus{}, s_previous_segv{};
// Serializes protection changes between queries, the fault handler and
// host-write scopes. A spin lock: the fault handler cannot block on a mutex.
// No holder touches guest memory, so a holder never faults.
std::atomic_flag s_protect = ATOMIC_FLAG_INIT;
struct ProtectLock {
    ProtectLock() { while (s_protect.test_and_set(std::memory_order_acquire)) rbrew_cpu_relax(); }
    ~ProtectLock() { s_protect.clear(std::memory_order_release); }
};

bool pending(uint64_t guest_page) { return s_pending[guest_page / 64] >> (guest_page % 64) & 1; }
void set_pending(uint64_t guest_page, bool on) {
    const uint64_t bit = 1ull << (guest_page % 64);
    if (on) s_pending[guest_page / 64] |= bit; else s_pending[guest_page / 64] &= ~bit;
}

void chain(const struct sigaction& previous, int signal, siginfo_t* info, void* context) {
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) { previous.sa_sigaction(signal, info, context); return; }
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal); return;
    }
    // Not ours and nobody else's: restore the default action and let the
    // instruction fault again (crash report shows the real fault).
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
}
void on_fault(int signal, siginfo_t* info, void* context) {
    auto* at = static_cast<uint8_t*>(info->si_addr);
    if (s_arena && at >= s_arena && at < s_arena + s_arena_size) {
        const uint64_t page = uint64_t(at - s_arena) / s_host_page;
        if (s_state[page].load(std::memory_order_acquire) != kInaccessible) {
            // A store to a page a query protected (or one another thread just
            // unprotected): make it writable and retry the instruction.
            ProtectLock lock;
            if (s_state[page].load(std::memory_order_relaxed) == kProtected) {
                mprotect(s_arena + page * s_host_page, s_host_page, PROT_READ | PROT_WRITE);
                s_state[page].store(kWritten, std::memory_order_release);
            }
            return;
        }
    }
    chain(signal == SIGBUS ? s_previous_bus : s_previous_segv, signal, info, context);
}

void install(uint8_t* arena, uint64_t arena_size) {
    s_host_page = uint64_t(getpagesize());
    s_ratio = s_host_page / kGuestPage;
    const uint64_t host_pages = (arena_size + s_host_page - 1) / s_host_page;
    s_state.reset(new std::atomic<uint8_t>[host_pages]);
    for (uint64_t i = 0; i < host_pages; ++i) s_state[i].store(kInaccessible, std::memory_order_relaxed);
    s_heat.reset(new uint8_t[host_pages]());
    s_age.reset(new uint8_t[host_pages]());
    s_pins.reset(new uint16_t[host_pages]());
    s_pending.reset(new uint64_t[(host_pages * s_ratio + 63) / 64]());
    s_arena_size = arena_size;
    std::atomic_thread_fence(std::memory_order_release);
    s_arena = arena;
    struct sigaction action{};
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    // macOS reports a store to a read-only mapping as SIGBUS; take SIGSEGV
    // as well (other kernels, other page kinds).
    sigaction(SIGBUS, &action, &s_previous_bus);
    sigaction(SIGSEGV, &action, &s_previous_segv);
}

// GetWriteWatch over [base, base + size): 4 KiB guest pages written since
// their last reset. Fails (non-zero) outside the registered arena, which
// then reads as untracked like memory without MEM_WRITE_WATCH on Windows.
unsigned get_write_watch(DWORD flags, void* base, SIZE_T size, void** addresses, ULONG_PTR* count, DWORD* granularity) {
    auto* begin = static_cast<uint8_t*>(base);
    if (!s_arena || begin < s_arena || begin + size > s_arena + s_arena_size || !size ||
        uint64_t(begin - s_arena) % kGuestPage || size % kGuestPage)
        return 1;
    const bool reset = (flags & WRITE_WATCH_FLAG_RESET) != 0;
    const uint64_t first = uint64_t(begin - s_arena) / kGuestPage, last = first + size / kGuestPage - 1;
    const ULONG_PTR capacity = *count;
    ULONG_PTR found = 0;
    if (granularity) *granularity = DWORD(kGuestPage);
    ProtectLock lock;
    uint64_t run_begin = 0, run_end = 0;   // host pages to protect, [begin, end)
    auto flush = [&] {
        if (run_end > run_begin) {
            mprotect(s_arena + run_begin * s_host_page, (run_end - run_begin) * s_host_page, PROT_READ);
            for (uint64_t page = run_begin; page < run_end; ++page) s_state[page].store(kProtected, std::memory_order_release);
        }
        run_begin = run_end = 0;
    };
    for (uint64_t host = first / s_ratio; host <= last / s_ratio; ++host) {
        const uint8_t state = s_state[host].load(std::memory_order_acquire);
        if (state == kInaccessible) { flush(); continue; }   // nothing can have written it
        const uint64_t lo = std::max(first, host * s_ratio), hi = std::min(last, host * s_ratio + s_ratio - 1);
        const bool written = state == kWritten;
        for (uint64_t guest = lo; guest <= hi; ++guest) {
            if (!written && !pending(guest)) continue;
            if (found == capacity) { flush(); *count = found; return 0; }
            addresses[found++] = s_arena + guest * kGuestPage;
            if (reset) set_pending(guest, false);
        }
        if (!reset) { flush(); continue; }
        if (!written) { s_heat[host] = 0; flush(); continue; }   // quiet since its last reset
        // Guest pages of this host page outside the query keep their write.
        for (uint64_t guest = host * s_ratio; guest < host * s_ratio + s_ratio; ++guest)
            if (guest < lo || guest > hi) set_pending(guest, true);
        if (s_pins[host]) { flush(); continue; }                 // a kernel write is in progress
        if (s_heat[host] >= kHotCollections && ++s_age[host] % kHotRetry) { flush(); continue; }
        if (s_heat[host] < kHotCollections) ++s_heat[host];
        else s_heat[host] = 0;                                    // retry: protect once more
        if (run_end != host) { flush(); run_begin = host; }
        run_end = host + 1;
    }
    flush();
    *count = found;
    return 0;
}

void track(uint8_t* arena, uint64_t arena_size, uint64_t offset, uint64_t size, bool accessible) {
    static std::once_flag once;
    std::call_once(once, [&] { install(arena, arena_size); });
    if (arena != s_arena || offset >= s_arena_size || !size) return;
    const uint64_t end = std::min(s_arena_size, offset + size);
    ProtectLock lock;
    for (uint64_t page = offset / s_host_page; page * s_host_page < end; ++page) {
        // Newly accessible pages count as written: their contents are unknown.
        s_state[page].store(accessible ? kWritten : kInaccessible, std::memory_order_release);
        s_heat[page] = s_age[page] = 0;
    }
}
void begin_host_write(uint8_t* begin, uint64_t size) {
    if (!s_arena || begin < s_arena || begin >= s_arena + s_arena_size || !size) return;
    const uint64_t first = uint64_t(begin - s_arena) / s_host_page;
    const uint64_t last = (std::min<uint64_t>(uint64_t(begin - s_arena) + size, s_arena_size) - 1) / s_host_page;
    ProtectLock lock;
    for (uint64_t page = first; page <= last; ++page) {
        ++s_pins[page];
        if (s_state[page].load(std::memory_order_relaxed) == kProtected) {
            mprotect(s_arena + page * s_host_page, s_host_page, PROT_READ | PROT_WRITE);
            s_state[page].store(kWritten, std::memory_order_release);
        }
    }
}
void end_host_write(uint8_t* begin, uint64_t size) {
    if (!s_arena || begin < s_arena || begin >= s_arena + s_arena_size || !size) return;
    const uint64_t first = uint64_t(begin - s_arena) / s_host_page;
    const uint64_t last = (std::min<uint64_t>(uint64_t(begin - s_arena) + size, s_arena_size) - 1) / s_host_page;
    ProtectLock lock;
    for (uint64_t page = first; page <= last; ++page) if (s_pins[page]) --s_pins[page];
}
}  // namespace gx2_apple_ww

inline unsigned GetWriteWatch(DWORD flags, void* base, SIZE_T size, void** addresses, ULONG_PTR* count, DWORD* granularity) {
    return gx2_apple_ww::get_write_watch(flags, base, size, addresses, count, granularity);
}

void gx2_write_watch_track(uint8_t* arena, uint64_t arena_size, uint64_t offset, uint64_t size, bool accessible) {
    gx2_apple_ww::track(arena, arena_size, offset, size, accessible);
}
Gx2HostWriteScope::Gx2HostWriteScope(uint8_t* mem, uint32_t addr, uint64_t size) {
    if (!mem || !size) return;
    begin_ = mem + addr; size_ = size;
    gx2_apple_ww::begin_host_write(begin_, size_);
}
Gx2HostWriteScope::~Gx2HostWriteScope() {
    if (size_) gx2_apple_ww::end_host_write(begin_, size_);
}
