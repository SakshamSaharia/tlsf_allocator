#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// TLSF allocator using caller-provided control storage and independent
// caller-owned pools. The free-block physical predecessor is recovered from
// a footer that exists only while the predecessor is free.
class TLSFAllocator {
public:
    struct Pool {
        std::byte* mem{};
        std::size_t bytes{};
        friend constexpr bool operator==(Pool a, Pool b) noexcept {
            return a.mem == b.mem && a.bytes == b.bytes;
        }
    };

    struct Statistics {
        std::size_t total_blocks{};
        std::size_t used_blocks{};
        std::size_t free_blocks{};
        std::size_t used_bytes{};
        std::size_t free_bytes{};
        std::size_t largest_free_block{};
    };

    // control_mem must point to at least control_size() bytes aligned to
    // control_alignment(). The allocator does not own this storage.
    explicit TLSFAllocator(void* control_mem) noexcept;
    ~TLSFAllocator();

    TLSFAllocator(const TLSFAllocator&) = delete;
    TLSFAllocator& operator=(const TLSFAllocator&) = delete;
    TLSFAllocator(TLSFAllocator&&) = delete;
    TLSFAllocator& operator=(TLSFAllocator&&) = delete;

    constexpr std::size_t TLSFAllocator::control_size() noexcept {
        return sizeof(Control);
    }

    constexpr std::size_t TLSFAllocator::control_alignment() noexcept {
        return alignof(Control);
    }

    // pools are caller-owned. mem must be ALIGN_SIZE-aligned
    // The allocator neither allocates nor frees pool memory.
    Pool add_pool(void* mem, std::size_t bytes) noexcept;
    bool remove_pool(Pool pool) noexcept;

    void* allocate(std::size_t bytes) noexcept;
    void* allocate_aligned(std::size_t bytes, std::size_t alignment) noexcept;
    void deallocate(void* ptr) noexcept;
    void* reallocate(void* ptr, std::size_t size) noexcept;

    // diagnostic only
    bool check_pool(Pool pool) const noexcept;
    Statistics statistics(Pool pool) const noexcept;
    bool check() const noexcept;

    std::size_t usable_bytes(Pool pool) const noexcept;
    static std::size_t block_size(const void* ptr) noexcept;

    static constexpr std::size_t alignment() noexcept { return ALIGN_SIZE; }
    static constexpr std::size_t block_size_minimum() noexcept { return MIN_FREE_BLOCK_BODY_SIZE; }
    static constexpr std::size_t block_size_maximum() noexcept { return BLOCK_SIZE_MAX; }
    static constexpr std::size_t allocation_overhead() noexcept { return BLOCK_HEADER_SIZE; }
    static constexpr std::size_t pool_overhead() noexcept { return POOL_OVERHEAD; }

private:
    // only size is part of the permanently exposed header. next_free and
    // prev_free occupy the start of the user area while the block is free.
    struct Block {
        std::size_t size{};
        Block* next_free{};
        Block* prev_free{};
    };


    static_assert(sizeof(Block) == 3 * sizeof(void*));
    static_assert(offsetof(Block, size) == 0);
    static_assert(offsetof(Block, next_free) == sizeof(std::size_t));
    static_assert(sizeof(Block) % alignof(Block) == 0);

    static constexpr std::size_t ALIGN_LOG2 = (sizeof(void*) == 8) ? 3 : 2;
    static constexpr std::size_t ALIGN_SIZE = std::size_t{1} << ALIGN_LOG2;
    static constexpr std::size_t SL_INDEX_COUNT_LOG2 = 5;
    static constexpr std::size_t SL_INDEX_COUNT = std::size_t{1} << SL_INDEX_COUNT_LOG2;
    static constexpr std::size_t FL_INDEX_SHIFT = SL_INDEX_COUNT_LOG2 + ALIGN_LOG2;
    static constexpr std::size_t FL_INDEX_MAX = (sizeof(void*) == 8) ? 32 : 30;
    static constexpr std::size_t FL_INDEX_COUNT = FL_INDEX_MAX - FL_INDEX_SHIFT + 1;
    static constexpr std::size_t SMALL_BLOCK_SIZE = std::size_t{1} << FL_INDEX_SHIFT;

    // physical blocks contain a one-word size field plus block_size bytes.
    static constexpr std::size_t BLOCK_HEADER_SIZE = sizeof(std::size_t);
    static constexpr std::size_t BLOCK_START_OFFSET = BLOCK_HEADER_SIZE;

    // a free block must hold next_free, prev_free and a footer.
    static constexpr std::size_t FREE_BLOCK_BODY_METADATA =
        2 * sizeof(void*) + sizeof(std::size_t);
    static constexpr std::size_t MIN_FREE_BLOCK_BODY_SIZE = FREE_BLOCK_BODY_METADATA;
    static constexpr std::size_t BLOCK_SIZE_MAX = std::size_t{1} << FL_INDEX_MAX;
    static constexpr std::size_t MIN_FREE_BLOCK_SIZE =
        BLOCK_HEADER_SIZE + MIN_FREE_BLOCK_BODY_SIZE;
    static constexpr std::size_t SENTINEL_SIZE = BLOCK_HEADER_SIZE;
    static constexpr std::size_t POOL_OVERHEAD = BLOCK_HEADER_SIZE + SENTINEL_SIZE;
    static constexpr std::size_t MIN_POOL_SIZE = MIN_FREE_BLOCK_SIZE + SENTINEL_SIZE;

    static constexpr std::size_t FREE_BIT = 1;
    static constexpr std::size_t PREV_FREE_BIT = 2;
    static constexpr std::size_t FLAG_MASK = FREE_BIT | PREV_FREE_BIT;

    struct Control {
        Block block_null{};
        std::uint32_t fl_bitmap{};
        std::array<std::uint32_t, FL_INDEX_COUNT> sl_bitmap{};
        std::array<std::array<Block*, SL_INDEX_COUNT>, FL_INDEX_COUNT> blocks{};
    };

    Control* control_{};

    static std::size_t align_up(std::size_t x, std::size_t align) noexcept;
    static std::size_t align_down(std::size_t x, std::size_t align) noexcept;
    static std::uintptr_t align_ptr(std::uintptr_t x, std::size_t align) noexcept;
    static bool is_power_of_two(std::size_t x) noexcept;

    static std::size_t block_size(const Block* b) noexcept;
    static void block_set_size(Block* b, std::size_t size) noexcept;
    static bool block_is_free(const Block* b) noexcept;
    static void block_set_free(Block* b) noexcept;
    static void block_set_used(Block* b) noexcept;
    static bool block_is_prev_free(const Block* b) noexcept;
    static void block_set_prev_free(Block* b) noexcept;
    static void block_set_prev_used(Block* b) noexcept;
    static bool block_is_last(const Block* b) noexcept;

    static Block* block_from_ptr(void* ptr) noexcept;
    static const Block* block_from_ptr(const void* ptr) noexcept;
    static void* block_to_ptr(Block* block) noexcept;
    static const void* block_to_ptr(const Block* block) noexcept;
    static Block* block_next(Block* block) noexcept;
    static const Block* block_next(const Block* block) noexcept;

    static std::size_t* block_footer(Block* block) noexcept;
    static const std::size_t* block_footer(const Block* block) noexcept;
    static void write_footer(Block* block) noexcept;
    static Block* block_prev(const Block* block) noexcept;

    static std::size_t adjust_request_size(std::size_t size, std::size_t align) noexcept;
    static void map_size_to_bucket(std::size_t size, int& fl, int& sl) noexcept;
    static void map_rounded_size_to_bucket(std::size_t size, int& fl, int& sl) noexcept;
    static int first_set_bit(std::uint32_t word) noexcept;
    static int highest_set_bit(std::size_t size) noexcept;

    void control_construct() noexcept;
    void block_insert_free(Block* block) noexcept;
    void block_remove_free(Block* block) noexcept;
    Block* find_next_nonempty_bucket(int& fl, int& sl) noexcept;
    Block* find_free_block(std::size_t size) noexcept;

    static bool block_can_split(const Block* block, std::size_t size) noexcept;
    static Block* block_split(Block* block, std::size_t size) noexcept;
    static Block* block_absorb(Block* prev, Block* block) noexcept;
    Block* merge_prev(Block* block) noexcept;
    Block* merge_next(Block* block) noexcept;
    void trim_free(Block* block, std::size_t size) noexcept;
    void trim_used(Block* block, std::size_t size) noexcept;
    Block* trim_free_leading(Block* block, std::size_t size) noexcept;
    void* prepare_used(Block* block, std::size_t size) noexcept;
};


