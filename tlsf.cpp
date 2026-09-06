#include "tlsf.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

TLSFAllocator::TLSFAllocator(void* control_mem) noexcept {
    assert(control_mem);
    assert(reinterpret_cast<std::uintptr_t>(control_mem) % alignof(Control) == 0);
    control_ = new (control_mem) Control{};
    control_construct();
}

TLSFAllocator::~TLSFAllocator() {
    assert(control_);
    control_->~Control();
}

std::size_t TLSFAllocator::align_up(std::size_t x, std::size_t align) noexcept {
    return (x + align - 1) & ~(align - 1);
}

std::size_t TLSFAllocator::align_down(std::size_t x, std::size_t align) noexcept {
    return x - (x & (align - 1));
}

std::uintptr_t TLSFAllocator::align_ptr(std::uintptr_t x, std::size_t align) noexcept {
    return (x + align - 1) & ~(align - 1);
}

bool TLSFAllocator::is_power_of_two(std::size_t x) noexcept {
    return x && !(x & (x - 1));
}

std::size_t TLSFAllocator::block_size(const Block* b) noexcept {
    return b->size & ~FLAG_MASK;
}

void TLSFAllocator::block_set_size(Block* b, std::size_t size) noexcept {
    b->size = size | (b->size & FLAG_MASK);
}

bool TLSFAllocator::block_is_free(const Block* b) noexcept {
    return (b->size & FREE_BIT) != 0;
}

void TLSFAllocator::block_set_free(Block* b) noexcept {
    b->size |= FREE_BIT;
}

void TLSFAllocator::block_set_used(Block* b) noexcept {
    b->size &= ~FREE_BIT;
}

bool TLSFAllocator::block_is_prev_free(const Block* b) noexcept {
    return (b->size & PREV_FREE_BIT) != 0;
}

void TLSFAllocator::block_set_prev_free(Block* b) noexcept {
    b->size |= PREV_FREE_BIT;
}

void TLSFAllocator::block_set_prev_used(Block* b) noexcept {
    b->size &= ~PREV_FREE_BIT;
}

bool TLSFAllocator::block_is_last(const Block* b) noexcept {
    return block_size(b) == 0;
}

TLSFAllocator::Block* TLSFAllocator::block_from_ptr(void* ptr) noexcept {
    return reinterpret_cast<Block*>(reinterpret_cast<std::byte*>(ptr) - BLOCK_START_OFFSET);
}

const TLSFAllocator::Block* TLSFAllocator::block_from_ptr(const void* ptr) noexcept {
    return reinterpret_cast<const Block*>(reinterpret_cast<const std::byte*>(ptr) - BLOCK_START_OFFSET);
}

void* TLSFAllocator::block_to_ptr(Block* block) noexcept {
    return reinterpret_cast<void*>(reinterpret_cast<std::byte*>(block) + BLOCK_START_OFFSET);
}

const void* TLSFAllocator::block_to_ptr(const Block* block) noexcept {
    return reinterpret_cast<const void*>(reinterpret_cast<const std::byte*>(block) + BLOCK_START_OFFSET);
}

TLSFAllocator::Block* TLSFAllocator::block_next(Block* block) noexcept {
    return reinterpret_cast<Block*>(reinterpret_cast<std::byte*>(block) +
                                    BLOCK_HEADER_OVERHEAD + block_size(block));
}

const TLSFAllocator::Block* TLSFAllocator::block_next(const Block* block) noexcept {
    return reinterpret_cast<const Block*>(reinterpret_cast<const std::byte*>(block) +
                                          BLOCK_HEADER_OVERHEAD + block_size(block));
}

std::size_t* TLSFAllocator::block_footer(Block* block) noexcept {
    return reinterpret_cast<std::size_t*>(reinterpret_cast<std::byte*>(block_to_ptr(block)) +
                                          block_size(block) - sizeof(std::size_t));
}

const std::size_t* TLSFAllocator::block_footer(const Block* block) noexcept {
    return reinterpret_cast<const std::size_t*>(reinterpret_cast<const std::byte*>(block_to_ptr(block)) +
                                                block_size(block) - sizeof(std::size_t));
}

void TLSFAllocator::write_footer(Block* block) noexcept {
    assert(block_is_free(block));
    *block_footer(block) = block_size(block);
}

TLSFAllocator::Block* TLSFAllocator::block_prev(const Block* block) noexcept {
    assert(block_is_prev_free(block));
    const std::size_t previous_size = *reinterpret_cast<const std::size_t*>(
        reinterpret_cast<const std::byte*>(block) - sizeof(std::size_t));
    assert(previous_size >= BLOCK_SIZE_MIN);
    return reinterpret_cast<Block*>(reinterpret_cast<std::byte*>(const_cast<Block*>(block)) -
                                    BLOCK_HEADER_OVERHEAD - previous_size);
}

std::size_t TLSFAllocator::adjust_request_size(std::size_t size, std::size_t align) noexcept {
    if (!size) return 0;
    if (size > std::numeric_limits<std::size_t>::max() - (align - 1)) return 0;
    const std::size_t aligned = align_up(size, align);
    if (aligned >= BLOCK_SIZE_MAX) return 0;
    return std::max(aligned, BLOCK_SIZE_MIN);
}

int TLSFAllocator::ffs32(std::uint32_t word) noexcept {
    return word ? static_cast<int>(std::countr_zero(word)) : -1;
}

int TLSFAllocator::fls_size(std::size_t size) noexcept {
    return size ? static_cast<int>(std::bit_width(size) - 1) : -1;
}

void TLSFAllocator::mapping_insert(std::size_t size, int& fl, int& sl) noexcept {
    if (size < SMALL_BLOCK_SIZE) {
        fl = 0;
        sl = static_cast<int>(size / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT));
    } else {
        fl = fls_size(size);
        sl = static_cast<int>((size >> (fl - SL_INDEX_COUNT_LOG2)) ^ SL_INDEX_COUNT);
        fl -= (FL_INDEX_SHIFT - 1);
    }
}

void TLSFAllocator::mapping_search(std::size_t size, int& fl, int& sl) noexcept {
    if (size >= SMALL_BLOCK_SIZE) {
        const int shift = fls_size(size) - SL_INDEX_COUNT_LOG2;
        if (shift >= 0) {
            const std::size_t round = (std::size_t{1} << shift) - 1;
            if (size > std::numeric_limits<std::size_t>::max() - round) {
                fl = static_cast<int>(FL_INDEX_COUNT);
                sl = 0;
                return;
            }
            size += round;
        }
    }
    mapping_insert(size, fl, sl);
}

void TLSFAllocator::control_construct() noexcept {
    control_->block_null.next_free = &control_->block_null;
    control_->block_null.prev_free = &control_->block_null;
    control_->fl_bitmap = 0;
    for (auto& row : control_->blocks) row.fill(&control_->block_null);
    control_->sl_bitmap.fill(0);
}

void TLSFAllocator::remove_free(Block* block, int fl, int sl) noexcept {
    Block* prev = block->prev_free;
    Block* next = block->next_free;
    assert(prev && next);
    next->prev_free = prev;
    prev->next_free = next;

    if (control_->blocks[fl][sl] == block) {
        control_->blocks[fl][sl] = next;
        if (next == &control_->block_null) {
            control_->sl_bitmap[fl] &= ~(std::uint32_t{1} << sl);
            if (!control_->sl_bitmap[fl]) {
                control_->fl_bitmap &= ~(std::uint32_t{1} << fl);
            }
        }
    }
}

void TLSFAllocator::insert_free(Block* block) noexcept {
    assert(block_is_free(block));
    write_footer(block);
    int fl = 0, sl = 0;
    mapping_insert(block_size(block), fl, sl);
    Block* current = control_->blocks[fl][sl];
    block->next_free = current;
    block->prev_free = &control_->block_null;
    current->prev_free = block;
    control_->blocks[fl][sl] = block;
    control_->fl_bitmap |= (std::uint32_t{1} << fl);
    control_->sl_bitmap[fl] |= (std::uint32_t{1} << sl);
}

void TLSFAllocator::block_insert(Block* block) noexcept {
    insert_free(block);
}

void TLSFAllocator::block_remove(Block* block) noexcept {
    int fl = 0, sl = 0;
    mapping_insert(block_size(block), fl, sl);
    remove_free(block, fl, sl);
}

TLSFAllocator::Block* TLSFAllocator::search_suitable(int& fl, int& sl) noexcept {
    std::uint32_t sl_map = control_->sl_bitmap[fl] & (~std::uint32_t{0} << sl);
    if (!sl_map) {
        const std::uint32_t fl_map = control_->fl_bitmap &
                                      (~std::uint32_t{0} << (fl + 1));
        if (!fl_map) return nullptr;
        fl = ffs32(fl_map);
        sl_map = control_->sl_bitmap[fl];
    }
    sl = ffs32(sl_map);
    return control_->blocks[fl][sl];
}

TLSFAllocator::Block* TLSFAllocator::locate_free(std::size_t size) noexcept {
    int fl = 0, sl = 0;
    if (!size) return nullptr;
    mapping_search(size, fl, sl);
    if (fl >= static_cast<int>(FL_INDEX_COUNT)) return nullptr;
    Block* block = search_suitable(fl, sl);
    if (block) {
        assert(block_size(block) >= size);
        block_remove(block);
    }
    return block;
}

bool TLSFAllocator::block_can_split(const Block* block, std::size_t size) noexcept {
    return block_size(block) >= size + SPLIT_MIN_PHYSICAL;
}

TLSFAllocator::Block* TLSFAllocator::block_split(Block* block, std::size_t size) noexcept {
    const bool was_free = block_is_free(block);
    const std::size_t old_size = block_size(block);
    assert(old_size >= size + SPLIT_MIN_PHYSICAL);

    Block* remaining = reinterpret_cast<Block*>(
        reinterpret_cast<std::byte*>(block) + BLOCK_HEADER_OVERHEAD + size);
    const std::size_t remain_size = old_size - size - BLOCK_HEADER_OVERHEAD;

    block_set_size(remaining, remain_size);
    block_set_free(remaining);
    if (was_free) block_set_prev_free(remaining);
    else block_set_prev_used(remaining);
    write_footer(remaining);

    block_set_size(block, size);
    if (was_free) {
        block_set_free(block);
        write_footer(block);
    } else {
        block_set_used(block);
    }

    Block* next = block_next(remaining);
    block_set_prev_free(next);
    return remaining;
}

TLSFAllocator::Block* TLSFAllocator::block_absorb(Block* prev, Block* block) noexcept {
    assert(!block_is_last(prev));
    const bool result_free = block_is_free(prev);
    prev->size = (block_size(prev) + block_size(block) + BLOCK_HEADER_OVERHEAD) |
                 (prev->size & FLAG_MASK);
    if (result_free) write_footer(prev);
    return prev;
}

TLSFAllocator::Block* TLSFAllocator::merge_prev(Block* block) noexcept {
    if (block_is_prev_free(block)) {
        Block* prev = block_prev(block);
        assert(prev && block_is_free(prev));
        block_remove(prev);
        block = block_absorb(prev, block);
    }
    return block;
}

TLSFAllocator::Block* TLSFAllocator::merge_next(Block* block) noexcept {
    Block* next = block_next(block);
    if (block_is_free(next)) {
        block_remove(next);
        block = block_absorb(block, next);
    }
    return block;
}

void TLSFAllocator::trim_free(Block* block, std::size_t size) noexcept {
    assert(block_is_free(block));
    if (block_can_split(block, size)) {
        Block* remaining = block_split(block, size);
        block_set_prev_free(remaining);
        block_insert(remaining);
    }
}

void TLSFAllocator::trim_used(Block* block, std::size_t size) noexcept {
    assert(!block_is_free(block));
    if (block_can_split(block, size)) {
        Block* remaining = block_split(block, size);
        block_set_prev_used(remaining);
        remaining = merge_next(remaining);
        block_insert(remaining);
    }
}

TLSFAllocator::Block* TLSFAllocator::trim_free_leading(Block* block, std::size_t size) noexcept {
    Block* remaining = block;
    if (block_can_split(block, size)) {
        remaining = block_split(block, size - BLOCK_HEADER_OVERHEAD);
        block_set_prev_used(block);
        block_set_prev_free(remaining);
        block_insert(block);
    }
    return remaining;
}

void* TLSFAllocator::prepare_used(Block* block, std::size_t size) noexcept {
    if (!block) return nullptr;
    assert(size);
    trim_free(block, size);
    Block* next = block_next(block);
    block_set_prev_used(next);
    block_set_used(block);
    return block_to_ptr(block);
}

TLSFAllocator::Pool TLSFAllocator::add_pool(void* mem, std::size_t bytes) noexcept {
    if (!mem) return {};
    if (reinterpret_cast<std::uintptr_t>(mem) % ALIGN_SIZE) return {};
    if (bytes < pool_overhead()) return {};

    const std::size_t pool_bytes = align_down(bytes - pool_overhead(), ALIGN_SIZE);
    if (pool_bytes < BLOCK_SIZE_MIN || pool_bytes > BLOCK_SIZE_MAX) return {};

    Block* block = reinterpret_cast<Block*>(mem);
    block->size = pool_bytes;
    block_set_prev_used(block);
    block_set_free(block);
    write_footer(block);
    block_insert(block);

    Block* sentinel = block_next(block);
    sentinel->size = 0;
    block_set_used(sentinel);
    block_set_prev_free(sentinel);

    return Pool{static_cast<std::byte*>(mem), bytes};
}

bool TLSFAllocator::remove_pool(Pool pool) noexcept {
    if (!pool.mem) return false;
    Block* first = reinterpret_cast<Block*>(pool.mem);
    Block* sentinel = block_next(first);
    if (!block_is_free(first) || !block_is_last(sentinel) || block_is_free(sentinel)) return false;
    block_remove(first);
    return true;
}

void* TLSFAllocator::allocate(std::size_t bytes) noexcept {
    const std::size_t adjust = adjust_request_size(bytes, ALIGN_SIZE);
    return prepare_used(locate_free(adjust), adjust);
}

void* TLSFAllocator::allocate_aligned(std::size_t bytes, std::size_t alignment) noexcept {
    if (!is_power_of_two(alignment) || alignment < ALIGN_SIZE) return nullptr;
    const std::size_t adjust = adjust_request_size(bytes, ALIGN_SIZE);
    if (!adjust) return nullptr;

    const std::size_t max = std::numeric_limits<std::size_t>::max();
    if (alignment > max - FREE_LEADING_GAP_MIN) return nullptr;
    if (adjust > max - alignment - FREE_LEADING_GAP_MIN) return nullptr;

    const std::size_t size_with_gap =
        adjust_request_size(adjust + alignment + FREE_LEADING_GAP_MIN, alignment);
    if (!size_with_gap) return nullptr;

    const std::size_t aligned_size = alignment > ALIGN_SIZE ? size_with_gap : adjust;
    Block* block = locate_free(aligned_size);
    if (!block) return nullptr;

    void* ptr = block_to_ptr(block);
    std::uintptr_t aligned_addr = align_ptr(reinterpret_cast<std::uintptr_t>(ptr), alignment);
    std::size_t gap = static_cast<std::size_t>(aligned_addr - reinterpret_cast<std::uintptr_t>(ptr));

    if (gap && gap < FREE_LEADING_GAP_MIN) {
        const std::size_t gap_remain = FREE_LEADING_GAP_MIN - gap;
        const std::size_t offset = std::max(gap_remain, alignment);
        aligned_addr = align_ptr(aligned_addr + offset, alignment);
        gap = static_cast<std::size_t>(aligned_addr - reinterpret_cast<std::uintptr_t>(ptr));
    }

    if (gap) {
        assert(gap >= FREE_LEADING_GAP_MIN);
        block = trim_free_leading(block, gap);
    }

    return prepare_used(block, adjust);
}

void TLSFAllocator::deallocate(void* ptr) noexcept {
    if (!ptr) return;
    Block* block = block_from_ptr(ptr);
    assert(!block_is_free(block));
    block_set_free(block);
    write_footer(block);
    Block* next = block_next(block);
    block_set_prev_free(next);
    block = merge_prev(block);
    block = merge_next(block);
    block_insert(block);
}

void* TLSFAllocator::reallocate(void* ptr, std::size_t size) noexcept {
    if (ptr && size == 0) {
        deallocate(ptr);
        return nullptr;
    }
    if (!ptr) return allocate(size);

    Block* block = block_from_ptr(ptr);
    assert(!block_is_free(block));
    Block* next = block_next(block);
    const std::size_t cursize = block_size(block);
    const std::size_t next_size = block_size(next);
    if (cursize > std::numeric_limits<std::size_t>::max() - next_size - BLOCK_HEADER_OVERHEAD) {
        return nullptr;
    }
    const std::size_t combined = cursize + next_size + BLOCK_HEADER_OVERHEAD;
    const std::size_t adjust = adjust_request_size(size, ALIGN_SIZE);
    if (!adjust) return nullptr;

    if (adjust > cursize && (!block_is_free(next) || adjust > combined)) {
        void* p = allocate(size);
        if (p) {
            std::memcpy(p, ptr, std::min(cursize, size));
            deallocate(ptr);
        }
        return p;
    }

    if (adjust > cursize) {
        merge_next(block);
        // The successor used to have PREV_FREE_BIT set because the absorbed
        // block was free. The enlarged block is now used.
        block_set_prev_used(block_next(block));
        block_set_used(block);
    }

    trim_used(block, adjust);
    return ptr;
}

std::size_t TLSFAllocator::block_size(const void* ptr) noexcept {
    return ptr ? TLSFAllocator::block_size(block_from_ptr(ptr)) : 0;
}

std::size_t TLSFAllocator::usable_bytes(Pool pool) const noexcept {
    if (!pool.mem || pool.bytes < pool_overhead()) return 0;
    return align_down(pool.bytes - pool_overhead(), ALIGN_SIZE);
}

TLSFAllocator::Statistics TLSFAllocator::statistics(Pool pool) const noexcept {
    Statistics stats{};
    if (!pool.mem || pool.bytes < pool_overhead()) return stats;

    const std::byte* begin = pool.mem;
    const std::uintptr_t end_addr = reinterpret_cast<std::uintptr_t>(pool.mem) + pool.bytes;
    const Block* block = reinterpret_cast<const Block*>(begin);

    while (reinterpret_cast<std::uintptr_t>(block) + BLOCK_HEADER_OVERHEAD <= end_addr) {
        if (block_is_last(block)) break;
        ++stats.total_blocks;
        const std::size_t sz = block_size(block);
        if (block_is_free(block)) {
            ++stats.free_blocks;
            stats.free_bytes += sz;
            stats.largest_free_block = std::max(stats.largest_free_block, sz);
        } else {
            ++stats.used_blocks;
            stats.used_bytes += sz;
        }
        block = block_next(block);
    }
    return stats;
}

bool TLSFAllocator::check_pool(Pool pool) const noexcept {
    if (!pool.mem || pool.bytes < pool_overhead()) return false;
    const std::uintptr_t begin_addr = reinterpret_cast<std::uintptr_t>(pool.mem);
    const std::uintptr_t end_addr = begin_addr + pool.bytes;
    if (end_addr < begin_addr) return false;
    const Block* block = reinterpret_cast<const Block*>(pool.mem);
    bool previous_free = false;
    std::size_t guard = 0;

    while (true) {
        const std::uintptr_t block_addr = reinterpret_cast<std::uintptr_t>(block);
        if (block_addr < begin_addr || block_addr + BLOCK_HEADER_OVERHEAD > end_addr) return false;
        if (++guard > pool.bytes / ALIGN_SIZE + 2) return false;
        if (reinterpret_cast<std::uintptr_t>(block_to_ptr(block)) % ALIGN_SIZE) return false;
        if (block_is_last(block)) {
            if (block_is_free(block)) return false;
            return block_addr + BLOCK_HEADER_OVERHEAD <= end_addr &&
                   block_is_prev_free(block) == previous_free;
        }

        const std::size_t sz = block_size(block);
        if (sz < BLOCK_SIZE_MIN || (sz % ALIGN_SIZE) != 0) return false;
        if (block_is_prev_free(block) != previous_free) return false;
        if (block_addr + BLOCK_HEADER_OVERHEAD + sz > end_addr) return false;

        const Block* next = block_next(block);
        const std::uintptr_t next_addr = reinterpret_cast<std::uintptr_t>(next);
        if (next_addr <= block_addr || next_addr + BLOCK_HEADER_OVERHEAD > end_addr) return false;

        if (block_is_free(block)) {
            if (block_is_free(next)) return false;
            if (*block_footer(block) != sz) return false;
            if (!block_is_prev_free(next)) return false;
        } else {
            // A used block has no valid footer. Its successor must agree with
            // the used state through PREV_FREE_BIT.
            if (block_is_prev_free(next)) return false;
        }

        previous_free = block_is_free(block);
        block = next;
    }
}

bool TLSFAllocator::check() const noexcept {
    for (std::size_t fl = 0; fl < FL_INDEX_COUNT; ++fl) {
        const bool fl_set = (control_->fl_bitmap & (std::uint32_t{1} << fl)) != 0;
        if (fl_set != (control_->sl_bitmap[fl] != 0)) return false;

        for (std::size_t sl = 0; sl < SL_INDEX_COUNT; ++sl) {
            const bool sl_set = (control_->sl_bitmap[fl] & (std::uint32_t{1} << sl)) != 0;
            const Block* b = control_->blocks[fl][sl];
            if (!sl_set) {
                if (b != &control_->block_null) return false;
                continue;
            }
            if (b == &control_->block_null) return false;

            const Block* prev = &control_->block_null;
            std::size_t guard = 0;
            while (b != &control_->block_null) {
                if (++guard > (std::size_t{1} << 26)) return false;
                if (!block_is_free(b) || block_is_prev_free(b)) return false;
                if (block_is_free(block_next(b))) return false;
                if (b->prev_free != prev) return false;
                if (b->next_free != &control_->block_null && b->next_free->prev_free != b) return false;
                int f = 0, s = 0;
                mapping_insert(block_size(b), f, s);
                if (f != static_cast<int>(fl) || s != static_cast<int>(sl)) return false;
                if (*block_footer(b) != block_size(b)) return false;
                prev = b;
                b = b->next_free;
            }
        }
    }
    return true;
}
