#include "tlsf.h"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct Buffer {
    std::byte* raw{};
    void* mem{};
    explicit Buffer(std::size_t n, std::size_t align = 8) {
        raw = static_cast<std::byte*>(std::malloc(n + align));
        if (!raw) std::abort();
        auto base = reinterpret_cast<std::uintptr_t>(raw);
        auto aligned = (base + align - 1) & ~(align - 1);
        mem = reinterpret_cast<void*>(aligned);
    }
    ~Buffer() { std::free(raw); }
};

// make a random generator from seed which is same across runs
static std::uint64_t state;
static inline std::uint64_t rng64() { state ^= state << 7; state ^= state >> 9; state ^= state << 8; return state; }

static double run_tlsf(int ops, std::uint64_t seed) {
    state = seed;
    std::vector<void*> live;
    live.reserve(ops / 2);
    auto t0 = std::chrono::steady_clock::now();
    Buffer control(TLSFAllocator::control_size(), TLSFAllocator::control_alignment());
    Buffer pool(256ull << 20);
    TLSFAllocator a(control.mem);
    auto p = a.add_pool(pool.mem, 256ull << 20);
    if (!p.mem) std::abort();
    
    
    for (int i = 0; i < ops; ++i) {
        if (live.empty() || (rng64() % 100) < 50) {
            auto n = 1 + rng64() % 4096;
            void* q = a.allocate(n);
            if (!q) std::abort();
            live.push_back(q);
        } else {
            auto j = rng64() % live.size();
            a.deallocate(live[j]);
            live[j] = live.back();
            live.pop_back();
        }
    }
    for (void* q : live) a.deallocate(q);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

static double run_malloc(int ops, std::uint64_t seed) {
    state = seed;
    std::vector<void*> live;
    live.reserve(ops / 2);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ops; ++i) {
        if (live.empty() || (rng64() % 100) < 50) {
            auto n = 1 + rng64() % 4096;
            void* q = std::malloc(n);
            if (!q) std::abort();
            live.push_back(q);
        } else {
            auto j = rng64() % live.size();
            std::free(live[j]);
            live[j] = live.back();
            live.pop_back();
        }
    }
    for (void* q : live) std::free(q);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
    constexpr int ops = 10'000'000;
    constexpr std::uint64_t seed = 0xC0FFEEULL;
    for (int r = 0; r < 5; ++r) {
        const double tlsf = run_tlsf(ops, seed + r);
        const double libc = run_malloc(ops, seed + r);
        std::printf("run %d: tlsf %.3f ms, malloc/free %.3f ms\n", r + 1, tlsf, libc);
    }
}
