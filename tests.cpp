#include "tlsf.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

//allocator only has pointer to control struct
static_assert(sizeof(TLSFAllocator) == sizeof(void*),
              "Allocator must remain a non-owning pointer-sized handle");
static_assert(TLSFAllocator::control_size() % TLSFAllocator::control_alignment() == 0);

struct Buffer {
    std::byte* raw{};
    void* mem{};
    std::size_t bytes{};

    explicit Buffer(std::size_t n, std::size_t align = 8) : bytes(n) {
        raw = static_cast<std::byte*>(std::malloc(n + align));
        assert(raw);
        const auto base = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (base + align - 1) & ~(align - 1);
        mem = reinterpret_cast<void*>(aligned);
    }

    ~Buffer() { std::free(raw); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

struct Fixture {
    Buffer control;
    Buffer pool;
    TLSFAllocator allocator;
    TLSFAllocator::Pool handle;
    // do some init sanity checks
    explicit Fixture(std::size_t pool_bytes)
        : control(TLSFAllocator::control_size(), TLSFAllocator::control_alignment()),
          pool(pool_bytes, TLSFAllocator::alignment()),
          allocator(control.mem),
          handle(allocator.add_pool(pool.mem, pool.bytes)) {
        assert(handle.mem == pool.mem);
        assert(handle.bytes == pool.bytes);
        assert(allocator.check());
        assert(allocator.check_pool(handle));
    }
};

// check empty allocator state mapping
static void test_control_storage_and_no_pool_ownership() {
    Buffer control(TLSFAllocator::control_size(), TLSFAllocator::control_alignment());
    TLSFAllocator a(control.mem);
    Buffer p1(2ull << 20), p2(2ull << 20);
    auto h1 = a.add_pool(p1.mem, p1.bytes);
    auto h2 = a.add_pool(p2.mem, p2.bytes);
    assert(h1.mem == p1.mem && h2.mem == p2.mem);
    assert(sizeof(TLSFAllocator) == sizeof(void*));
    assert(a.check());
    assert(a.remove_pool(h1));
    assert(a.remove_pool(h2));
    assert(a.check());
}
// check alloc and dealloc with one pool 
static void test_basic_allocation_and_statistics() {
    Fixture f(8ull << 20);
    auto before = f.allocator.statistics(f.handle);
    assert(before.total_blocks == 1);
    assert(before.free_blocks == 1);
    assert(before.used_blocks == 0);

    void* p = f.allocator.allocate(1);
    void* q = f.allocator.allocate(1000);
    void* r = f.allocator.allocate(100000);
    assert(p && q && r);
    std::memset(p, 0x11, 1);
    std::memset(q, 0x22, 1000);
    std::memset(r, 0x33, 100000);
    assert(f.allocator.block_size(p) >= 1);
    assert(f.allocator.block_size(q) >= 1000);
    assert(f.allocator.block_size(r) >= 100000);
    assert(f.allocator.check_pool(f.handle));

    auto mid = f.allocator.statistics(f.handle);
    assert(mid.used_blocks == 3);
    assert(mid.free_blocks >= 1);

    f.allocator.deallocate(q);
    assert(f.allocator.check_pool(f.handle));
    f.allocator.deallocate(p);
    assert(f.allocator.check_pool(f.handle));
    f.allocator.deallocate(r);
    assert(f.allocator.check());
    auto after = f.allocator.statistics(f.handle);
    assert(after.free_blocks == 1);
    assert(after.used_blocks == 0);
    assert(after.largest_free_block == f.allocator.usable_bytes(f.handle));
    assert(f.allocator.remove_pool(f.handle));
}
// test case with minimum pool size
static void test_footer_minimum_and_full_coalescing() {
    // minimum pool: 8-byte first-block header + 24-byte free area + 8-byte sentinel.
    constexpr std::size_t bytes = 40;
    Buffer control(TLSFAllocator::control_size(), TLSFAllocator::control_alignment());
    TLSFAllocator a(control.mem);
    Buffer pool(bytes, TLSFAllocator::alignment());
    auto h = a.add_pool(pool.mem, pool.bytes);
    assert(h.mem);
    assert(a.usable_bytes(h) == 24);
    void* p = a.allocate(1);
    assert(p);
    assert(a.block_size(p) == 24);
    assert(a.check_pool(h));
    a.deallocate(p);
    assert(a.check_pool(h));
    assert(a.statistics(h).largest_free_block == 24);
    assert(a.remove_pool(h));
}

// test random allocs and deallocs
static void test_split_and_randomized_coalesce() {
    Fixture f(16ull << 20);
    std::vector<void*> ptrs;
    ptrs.reserve(2500);
    for (std::size_t i = 1; i <= 2500; ++i) {
        ptrs.push_back(f.allocator.allocate((i * 37) % 5000 + 1));
        assert(ptrs.back());
    }
    assert(f.allocator.check_pool(f.handle));

    std::mt19937_64 rng(42);
    std::shuffle(ptrs.begin(), ptrs.end(), rng);
    for (void* p : ptrs) {
        f.allocator.deallocate(p);
        assert(f.allocator.check_pool(f.handle));
        assert(f.allocator.check());
    }
    auto s = f.allocator.statistics(f.handle);
    assert(s.free_blocks == 1);
    assert(s.used_blocks == 0);
    assert(s.largest_free_block == f.allocator.usable_bytes(f.handle));
}

// test reallocs
static void test_realloc_copy_and_shrink() {
    Fixture f(8ull << 20);
    auto* p = static_cast<std::uint8_t*>(f.allocator.allocate(128));
    assert(p);
    for (int i = 0; i < 128; ++i) p[i] = static_cast<std::uint8_t>(i);

    // occupy a block next to current one
    void* blocker = f.allocator.allocate(1024);
    assert(blocker);

    // ask for expanding -> copying
    p = static_cast<std::uint8_t*>(f.allocator.reallocate(p, 4096));
    assert(p);
    for (int i = 0; i < 128; ++i) assert(p[i] == static_cast<std::uint8_t>(i));
    assert(f.allocator.check_pool(f.handle));

    // ask for shrinking
    p = static_cast<std::uint8_t*>(f.allocator.reallocate(p, 64));
    assert(p);
    for (int i = 0; i < 64; ++i) assert(p[i] == static_cast<std::uint8_t>(i));
    f.allocator.deallocate(p);
    f.allocator.deallocate(blocker);
    assert(f.allocator.check_pool(f.handle));
}

// check for various alignments
static void test_alignment() {
    Fixture f(32ull << 20);
    for (std::size_t align : {8u, 16u, 32u, 64u, 128u, 256u, 4096u, 65536u}) {
        void* p = f.allocator.allocate_aligned(777, align);
        assert(p);
        assert(reinterpret_cast<std::uintptr_t>(p) % align == 0);
        // check writable for 777 bytes
        std::memset(p, 0x5a, 777);
        f.allocator.deallocate(p);
        assert(f.allocator.check_pool(f.handle));
    }
}
// now check for multiple pools
static void test_multi_pool_interleaving_and_removal() {
    Buffer control(TLSFAllocator::control_size(), TLSFAllocator::control_alignment());
    TLSFAllocator a(control.mem);
    Buffer b1(2ull << 20), b2(2ull << 20), b3(2ull << 20);
    auto p1 = a.add_pool(b1.mem, b1.bytes);
    auto p2 = a.add_pool(b2.mem, b2.bytes);
    auto p3 = a.add_pool(b3.mem, b3.bytes);
    assert(a.check());

    std::vector<void*> live;
    for (int i = 0; i < 3000; ++i) {
        void* p = a.allocate(64 + (i % 2000));
        assert(p);
        live.push_back(p);
    }
    for (std::size_t i = 0; i < live.size(); i += 2) a.deallocate(live[i]);
    assert(a.check());
    for (std::size_t i = 1; i < live.size(); i += 2) a.deallocate(live[i]);
    assert(a.check());
    assert(a.check_pool(p1));
    assert(a.check_pool(p2));
    assert(a.check_pool(p3));

    assert(a.remove_pool(p2));
    assert(a.check());
    assert(a.remove_pool(p1));
    assert(a.remove_pool(p3));
    assert(a.check());
}

int main() {
    test_control_storage_and_no_pool_ownership();
    test_basic_allocation_and_statistics();
    test_footer_minimum_and_full_coalescing();
    test_split_and_randomized_coalesce();
    test_realloc_copy_and_shrink();
    test_alignment();
    test_multi_pool_interleaving_and_removal();
    std::cout << "ALL CORRECTNESS TESTS PASSED\n";
}
