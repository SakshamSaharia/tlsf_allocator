# TLSF Memory Allocator

An implementation of the Two-Level Segregated Fit (TLSF) memory allocation algorithm.

This implementation targets a 64-bit ABI directly: pointers, `std::size_t`, and
`std::uintptr_t` are expected to be 8 bytes. The allocator's minimum alignment is 8 bytes.

The allocator uses caller-provided control storage and caller-owned memory pools. It supports
allocation, deallocation, aligned allocation, reallocation, multiple pools, and basic
consistency/statistics checks. Free blocks use segregated free lists and bitmaps for fast lookup,
while coalescing is performed when blocks are freed.

## API

Include the allocator with:

```cpp
#include "tlsf.h"
```

### Create an allocator

The control storage is supplied by the caller:

```cpp
Buffer control(TLSFAllocator::control_size(),
               TLSFAllocator::control_alignment());

TLSFAllocator allocator(control.mem);
```

The allocator does not own this storage.

### Add and remove pools

Pools are also supplied and owned by the caller:

```cpp
auto pool = allocator.add_pool(memory, size);

allocator.remove_pool(pool);
```

The pool memory must be aligned to `TLSFAllocator::alignment()`.

### Allocate and deallocate

```cpp
void* ptr = allocator.allocate(bytes);

allocator.deallocate(ptr);
```

### Aligned allocation

```cpp
void* ptr = allocator.allocate_aligned(bytes, alignment);
```

The alignment must be a power of two and at least the allocator's minimum alignment.

### Reallocate

```cpp
ptr = allocator.reallocate(ptr, new_size);
```

The allocator can resize a block in place when possible and otherwise allocates a new
block and copies the existing contents.

### Query allocation size

```cpp
std::size_t bytes = TLSFAllocator::block_size(ptr);
```

### Pool statistics

```cpp
auto stats = allocator.statistics(pool);

stats.total_blocks;
stats.used_blocks;
stats.free_blocks;
stats.used_bytes;
stats.free_bytes;
stats.largest_free_block;
```

### Consistency checks

```cpp
bool ok = allocator.check();
bool pool_ok = allocator.check_pool(pool);
```

These are diagnostic helpers for validating allocator and pool metadata.

## Build

```bash
make
```

Run the correctness tests:

```bash
./tests
```

Run the benchmark:

```bash
./benchmark
```
## Side Notes
I saw an average of about 5% speedup than malloc/free, when run over 10 million ops.

The implementation is mostly c++17, although I used some C++20 features for bit functionalities 

Here the memory pools are user specified which violates RAII, but makes per-pool testing more comprehensive and easier to reason about.

Why TLSF against simple segregated list: with segregated list, if we follow the same
index for fl, then in the worst case, there might be a block nearly double of the requested size but we might reject the request. But with TLSF that near 100% margin reduces to about 6% of the request size since the next range is (1/2^5) extra of the current fl range. Which makes this a "good-fit" allocator 

One addition I thought of -> we can make a fast allocator for fixed small block sizes of 8,16 bytes where we come up with a mechanism so that we dont spend metadata overhead of storing size, like a fixed size slab allocator.

Disclamer: This is primarly an adapdation of @mattconte 's implementation, with some changes ofcourse
