// host_cpu.h - busy-wait hint and scheduling class of the current host thread.
#pragma once
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>
#endif
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

// One iteration of a spin-wait loop. The spin windows in the renderer are
// counted in iterations calibrated against x86 PAUSE. On ARM64, YIELD retires
// in about one cycle on Apple cores and would shrink every window to a few
// nanoseconds; ISB drains the pipeline and takes about as long as PAUSE.
inline void rbrew_cpu_relax() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __isb(_ARM64_BARRIER_SY);
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("isb sy" ::: "memory");
#endif
}

// Scheduling class of the calling thread. Windows keeps its explicit
// SetThreadPriority calls at each site. On macOS the QoS class decides which
// cores a thread may run on and how fast: UTILITY and BACKGROUND threads are
// kept on the efficiency cores. Background work runs at DEFAULT instead:
// below the guest threads, but on any core at full speed, like a
// below-normal thread on Windows.
enum class RbrewThreadClass {
    Interactive,  // the frame's critical path: render thread, command recorder
    Normal,       // guest CPU threads, demand shader compiles
    Background,   // warm-up compiles, cache files: lower priority, full speed
};
inline void rbrew_set_thread_class(RbrewThreadClass c) {
#if defined(__APPLE__)
    const qos_class_t qos = c == RbrewThreadClass::Interactive ? QOS_CLASS_USER_INTERACTIVE :
                            c == RbrewThreadClass::Normal ? QOS_CLASS_USER_INITIATED : QOS_CLASS_DEFAULT;
    pthread_set_qos_class_self_np(qos, 0);
#else
    (void)c;
#endif
}
