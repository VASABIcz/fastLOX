#pragma once

#include <cstddef>
#include <span>
#include <cassert>
#include <vector>

#include "tracking.hpp"

// In this file there are definitions of a some notion of allocation interface,
// supported by a `AllocatorBase` mixin and two concrete allocators,
// `ArenaAllocator` and `StackingAllocator`.
//
// First, why custom allocators, and why not use "general purpose" memory
// allocation through malloc and free. Of course we could "just" use the general
// purpose allocators, but there are actually parts of our runtime that have
// specific usage patterns that make it amanable to more customized memory
// management (e.g. the parser) and there are parts of our program which really
// call for a custom memory management (the Lox heap, i.e. the garbage collected
// memory). To better prepare for the Lox heap, the concept of custom memory
// allocation is shown on the example of the parser, which uses all the code
// introduced in this file.
//
// What let's us employ custom memory allocation is a better insight into our
// allocation and deallocation patterns. In our specific case we have big
// insights into _lifetimes_ of our allocations. Consider the parser, it goes
// through contents of a string buffer and produces a tree structure with
// individually allocated nodes. But for deallocation, we mostly only care about
// freeing the entire AST at once, e.g. because we compiled it into bytecode, or
// because we are done AST-interpreting it. In the whole AST, there is
// essentially just single lifetime, and we either deallocate nothing or
// everything.
//
// We can exploit this pattern by allocating a bigger space (say with malloc),
// from which we allocaate all the individual AST nodes, and after we are done
// with the AST, we just free the single memory chunk with free. A very easy
// implementation of this just starts giving out memory piece by piece from the
// start of the chunk. Implementations often maintain a pointer designating the
// start of the free space, which is "bumped" with each allocation request. Bump
// allocation, arena allocation, linear allocataion, stack allocation,
// region-based allocation, and other terms you might hear are mostly this exact
// idea, though different people give them different definitions and perhaps
// expect them to provide different features.
//
// One feature common to all arena allocators is that they are unable to either
// randomly free single allocations (since they don't track individual
// allocations), and that they don't ever run destructors on the deallocated
// objects (since they deallocate all objects just by freeing a big chunk of
// memory). For the AST allocation for example, we don't mind either -- we won't
// need individually free AST nodes, and destructors on AST nodes would just
// deallocate other AST nodes which already get freed as part of the big chunk.
// But allocating in the arena objects whose destructors free resources such as
// memory allocated with malloc, unlock locks or close file descriptors is not a
// good idea, since there will be leaks.
//
// Instead of deallocating the whole arena chunk, it is possible to "save" the
// current bump pointer value, and later "restore" it. This "deallocates" all
// objects allocated after the save, keeping all object allocated before it.
//
// Typically, the most differentiating feature between the "arena allocators" is
// how they handle the situation when they run out of space in the "big chunk of
// memory". There are a few common solutions:
//
// (1) Abort, since the memory is exhausted. Easy to do, makes sense e.g. for
//     microcontrollers with limited amount or memory, or programs with known
//     upper bound on memory consumption.
//
// (2) Allocate a new chunk of memory to be used in addition to the old one. Old
//     allocations are kept in old chunk(s), new allocations are  performed in
//     the new chunk. This has the advantage that all pointers to previously
//     allocated data are kept valid, the disadvantage is that the memory is no
//     longer contiguous.
//
// (3) Allocate a new (bigger) chunk memory and copy the contents of the old
//     (smaller) chunk to the new space. This implicitly moves all the previous
//     allocations, and new allocations will be made in the rest of the new
//     space. This is dual to the previous approach: pointers to previous
//     allocations are invalidated (potentially on every allocation), but the
//     memory is kept contiguous at all times.
//
// (4) Extend the current chunk by allocating the memory immediately following
//     it in the address space. This is a refinement that has advantages of both
//     of the previous approaches, while having none of their limitations. The
//     problem is achieving it. On modern systems it can be due to the use of
//     virtual memory (i.e. mmap/VirtualAlloc a big chunk of address space, but
//     _commit_ only the actually needed memory.)
//
// There are many great resources on Arenas and associated topics, I can
// recommend at the very least:
//
//  - https://www.rfleury.com/p/untangling-lifetimes-the-arena-allocator
//  - https://www.gingerbill.org/article/2019/02/01/memory-allocation-strategies-001/
//  - David R. Hanson, Fast Allocation and Deallocation of Memory Based on Object Lifetmes
//    - http://drhanson.s3.amazonaws.com/storage/documents/fastalloc.pdf
//
// As for Lox heap, an Arena allocator which frees all the heap allocations
// after the interpreter is finished is a correct (though memory inefficient)
// implementation of a heap. Though having the basis for custom memory
// allocation, and having insight on the allocation/deallocation strategy,
// memory layout are things that will be very much useful. Though the heap may
// need to provide things the Arena allocators don't, such as:
//
//  - Bookkeeping of allocation size (and/or type information),
//  - Heap parsability.
//
// Garbage collection may have similar constraints as the frees of our entire
// arenas. Depending on the implemented algorithm, it may not even visit all
// individual deallocated objects, thus counting on destruction of individually
// objects is not a good idea. There exists a notion of "finalization" (running
// "finalizers", equivalents of destructors, on garbage collected objects),
// which arguably is a bad idea, and instead something like the deterministic
// "dispose pattern" (https://en.wikipedia.org/wiki/Dispose_pattern) should be
// used by the language.



// With implementations of allocators we face an issue. Our bare allocators
// usually provide raw methods like `alloc` and `free` which work with raw
// sizes, alignments, pure bytes, etc. Though we would sometimes like to use
// functions on a higher level of abstraction, such as allocate an object or
// array of type T, etc., which would ultimately use the primitives. To prevent
// reimplementing these abstractions for each allocator, we will use a so called
// "Mixin class" using the "Curiously Recurring Template Pattern" (CRTP) idiom.
// Even though it sounds scary, it will allow us to just have an `AllocatorBase`
// class serving as a sort of interface with required methods which need to be
// implemented by classes which want to get the provided methods for free.
//
// This has been inspired by LLVM:
//
//  - https://llvm.org/doxygen/classllvm_1_1AllocatorBase.html
//
// See Bendersky's explanation and motivation of this sort of CRTP usageg:
//
//  - https://eli.thegreenplace.net/2011/05/17/the-curiously-recurring-template-pattern-in-c/
//
template <typename Derived>
class AllocatorBase {
public:
	// Required methods (`Derived` should implement these, to get the
	//                   provided methods)

	// Allocate `size` bytes, aligned to `align` alignment.
	void *alloc(size_t size, size_t align) {
		return static_cast<Derived *>(this)->alloc(size, align);
	}

	// Provided methods

	// Allocate space for a single value of type T.
	//
	// NOTE: You may not want to use this for Lox heap allocated values,
	// where you may want greater control over alignment.
	template <typename T>
	T *allocate() {
		return (T *) alloc(sizeof(T), alignof(T));
	}

	// Allocate and construct an object of type T by calling the constructor
	// with the specified arguments.
	//
	// NOTE: You may not want to use this for Lox heap allocated values,
	// where you may want greater control over alignment.
	template <typename T, typename... As>
	[[nodiscard]] inline T *create(As&&... args) {
		T *obj = allocate<T>();
		return new(obj) T(std::forward<As>(args)... );
	}

	// Allocate for a `count` elements of type T, returns a pointer+count
	// pair.
	template <typename T>
	std::span<T> allocate_array(size_t count) {
	 	size_t size = count * sizeof(T);
		assert(count == 0 || size >= sizeof(T));
		return std::span((T *) alloc(size, alignof(T)), count);
	}

	// Copy a `std::initializer_list` to space allocated by the allocator.
	// NOTE: This is just a dumb `memcpy`, no constructs will be used.
	template <typename T>
	std::span<T> allocate_array(std::initializer_list<T> init_list) {
		if (init_list.size() == 0) {
			return std::span<T>();
		}
		std::span<T> span = allocate_array<T>(init_list.size());
		memcpy(span.data(), data(init_list), init_list.size() * sizeof(T));
		return span;
	}

	// Copy a `std::vector` to space allocated by the allocator.
	// NOTE: This is just a dumb `memcpy`, no constructs will be used.
	template <typename T>
	std::span<T> allocate_array(std::vector<T> &&vec) {
		if (vec.empty()) {
			return std::span<T>();
		}
		std::span<T> span = allocate_array<T>(vec.size());
		memcpy(span.data(), data(vec), vec.size() * sizeof(T));
		return span;
	}

	// Align `position` to be aligned to `alignment`
	static size_t align(size_t position, size_t alignment) {
		return (position + (alignment - 1)) & ~(alignment - 1);
	}
};

// `ArenaAllocator` is an implementation of an arena allocator that is not
// limited to a single chunk of memory, and uses the approach (2) that is
// explained above, i.e. after a chunk fills up a new one is allocated to host
// the new allocations, old allocations are kept in the old chunk.
//
// The memory is allocated with `malloc`, freed with `free`, the chunk metadata
// is "embedded" at the start of each chunk. A newly constructed
// `ArenaAllocator` doesn't allocate any chunk, only at the first allocation is
// a chunk allocated. A special "sentinel" zero-initialized chunk is used to
// prevent on branching, since at times we may have no chunks allocated.
//
// The interface is mostly `alloc` to allocate, the `alloc` wrappers from
// `AllocatorBase`, `save` and `restore` to provide "checkpoints" and destructor
// which frees all allocated chunks (and is equivalent to restoring to a
// position saved immediately after construction).
//
// To nudge towards keeping memory usage statistics, the `ArenaAllocator`
// integrates with VMLogger from `tracking.hpp` and is always associated with a
// memory kind. As a kind of trade-off the memory usage statistics emitted by
// the `ArenaAllocator` include _internal fragmentation_, i.e. also the memory
// wasted on alignment and unused space at the end of chunks. Imagine if we have
// a chunk with 1 KiB left, but we ask for 2 KiB -- in this case the allocator
// will increase the memory usage by 3 KiB. This makes the implementation easier
// and the statistics consistent, even though it may not be what you want. In
// general, this allocator is most suited towards allocations of many small
// objects with largely similar alignment. In these cases internal fragmentation
// will stay low.
//
// (Whether you would like to account for internal fragmentation in your
// statistics is not straightforward in general. You may be interested in just
// the cummulative size of all made allocations, which doesn't include the
// internal fragmentation, neither does it reflect the actual memory use
// incurred by the application, since allocations from the operating system are
// usually based on pages (usually 4 KiB sized and aligned chunks). The
// application's "actual" memory usage is better reflected by metrics like the
// "Resident set size" (often shortened to "RSS", or called also "RES" like in
// top).
class ArenaAllocator : public AllocatorBase<ArenaAllocator> {
public:
	explicit ArenaAllocator(MemoryKind kind) : current(&sentinel), prev_size_sum(0), kind(kind) {}
	~ArenaAllocator();

	void *alloc(size_t size, size_t align);

	size_t save();
	void restore(size_t pos);

private:

	struct Chunk {
		size_t size;
		size_t pos;
		Chunk *prev;
	};

	static Chunk sentinel;

	Chunk *current;
	size_t prev_size_sum;
	MemoryKind kind;
};

inline void *ArenaAllocator::alloc(size_t size, size_t align) {
	size_t pos = AllocatorBase::align(current->pos, align);
	size_t current_size = current->size;
	if (pos + size > current_size) {
		prev_size_sum += current_size;
		size_t new_size = size + (current_size ? current_size * 2 : 1024);
		auto *new_chunk = (Chunk *) malloc(new_size);
		new_chunk->size = new_size;
		new_chunk->prev = current;
		pos = AllocatorBase::align(sizeof(Chunk), align);
		ASAN_POISON_MEMORY_REGION(((unsigned char *) new_chunk) + pos, new_size - pos);
		VMLogger::get().increase_memory_usage(kind, (current_size - current->pos) + (pos + size));
		current = new_chunk;
	} else {
		VMLogger::get().increase_memory_usage(kind, pos + size - current->pos);
	}
	current->pos = pos + size;
	ASAN_UNPOISON_MEMORY_REGION(((unsigned char *) current) + pos, size);
	return ((unsigned char *) current) + pos;
}

inline size_t ArenaAllocator::save() {
	return prev_size_sum + current->pos;
}

inline void ArenaAllocator::restore(size_t pos) {
	Chunk *chunk = current;
	size_t starting_size = prev_size_sum + chunk->pos;
	VMLogger::get().decrease_memory_usage(kind, starting_size - pos);
	while (pos < prev_size_sum) {
		Chunk *prev = chunk->prev;
		free(chunk);
		chunk = prev;
		prev_size_sum -= chunk->size;
	}
	chunk->pos = pos - prev_size_sum;
	size_t mem_start = chunk->pos == 0 && chunk != &sentinel ? sizeof(Chunk) : chunk->pos;
	ASAN_POISON_MEMORY_REGION(((unsigned char *) chunk) + mem_start, chunk->size - mem_start);
	current = chunk;
}

inline ArenaAllocator::Chunk ArenaAllocator::sentinel = {};

inline ArenaAllocator::~ArenaAllocator() {
	restore(0);
	if (current != &sentinel) {
		free(current);
		current = &sentinel;
	}
}

// `StackingAllocator` is essentially an arena allocator, which keeps a single
// chunk of memory, though it is able to _reallocate_ it. Thus it is mostly
// implementation of the variant (3) mentioned above. The allocator is not
// limited to a some chunk size, and instead grows the single chunk it manages
// exponentially. Thus the overhead of copying the data from smaller to larger
// chunks is amortized.
//
// If this sounds like a vector (dynamic array), then yes it! To quote Ryan
// Fleury's excellent article:
//
// > Depending on the exact growth strategy, this either makes arenas a suitable
// > replacement for dynamic arrays, or a perfect implementation of one.
//
// This class is thus mostly about fitting into our allocator scheme: i.e.
// having `alloc` method, abstractions from `AllocatorBase`, ability to
// save/restore just like `ArenaAllocator`, etc.
//
// The name `Stacking` is mostly from the specific usage in the parser, where
// from this allocator we branch off `StackedArray`s (implemented below), which
// are then moved to the `ArenaAllocator`. In a way, this allocator provides
// scratch space for allocation of _nested_ or "stacked" arrays, whose size is
// not known ahead of time. Its use similar to one in the parser, may arise even
// in things like the bytecode compiler.
//
// In general we expect this allocator to settle to some reasonable chunk size
// as it gets used.
class StackingAllocator : public AllocatorBase<StackingAllocator> {
public:
	StackingAllocator() : mem(nullptr), position(0), capacity(0) {}
	~StackingAllocator() {
		restore(0);
		free(mem);
	}

	template <typename T>
	void push_back(T &&value) {
		*allocate<T>() = value;
	}

	void *alloc(size_t size, size_t alignment) {
		size_t start = align(position, alignment);
		if (start + size > capacity) {
			capacity = capacity ? capacity * 2 : size * 8;
			auto *new_mem = (unsigned char *) realloc(mem, capacity);
			assert(new_mem);
			mem = new_mem;
			ASAN_POISON_MEMORY_REGION(mem, capacity);
		}
		position = start + size;
		ASAN_UNPOISON_MEMORY_REGION(mem, position);
		return &mem[start];
	}

	[[nodiscard]] size_t save() const {
		return position;
	}

	void restore(size_t pos) {
		position = pos;
		ASAN_POISON_MEMORY_REGION(mem + pos, capacity - pos);
	}

	template <typename T>
	std::span<T> from(size_t pos) {
		size_t start = AllocatorBase::align(pos, alignof(T));
		size_t count = (position - start) / sizeof(T);
		return std::span((T *) &mem[start], count);
	}

private:
	unsigned char *mem;
	size_t position;
	size_t capacity;
};

// Consider if we are to parse the following structure, such that we allocate
// arrays of elements in parenthesis into the Arena Allocator:
//
//     ( a ( b c d e ( f g ) ( h i ) ( j k ) l ) m n)
//
// We don't want to impose any limits on the nesting level or the sizes of these
// parenthesised structures. The arrays we need to ultimitaly allocate in the
// arena are (ignoring the references to contained arrays):
//
//     ( f g )
//     ( h i )
//     ( j k )
//     ( b c d e l )
//     ( a m n)
//
// Instead of allocating a new std::vector at each '(' we are able to just have
// a single vector, from which we can remember where each nesting level had
// started, and at each ')' we can move the "pending array" to the arena. This
// is because the parenthesised structures follow the stack principle. E.g. at
// the point of finishing ( f g ) we would have:
//
//     a b c d e f g
//
// so we would copy f g to arena, and continue with next elements, until we need
// to finish ( h i ):
//
//     a b c d e h i
//
// and so on. This saves us memory use. The only problem is saving the positions
// where the subarrays started. In general, this is a stack, but with an
// implementation based on recursion we may get away with something like a
// "start" variable on the system call stack used for the recursion.
//
// In our case, we replace `std::vector` with `StackingAllocator` and the
// `start` variable with `StackedArray`, which is a nice handle allowing us to
// remember the starting position, `push_back` new elements by offloading to the
// underlying `StackingAllocator`, and moving the space marked by the `start`
// position and the current position to the `ArenaAllocator` with the `get`
// method.
//
// Naturally this is very useful for the parser, where we have to parse these
// nested structures of size not known ahead of time, but may also come hande
// for the compiler, which e.g. for all the nested functions has pending byte
// code arrays, and follows a very similar recursive, stack based memory
// allocation pattern.
//
// NOTE: You DON'T have to use this yourself. This is just an example of a case
// where we are able to exploit memory usage patterns.
//
template <typename T>
class StackedArray {
public:
	explicit StackedArray(StackingAllocator *stack) : stack(stack), start(stack->save()) {}

	void push_back(T &&value) {
		stack->push_back(std::move(value));
	}

	[[nodiscard]] size_t size() const {
		return stack->from<T>(start).size();
	}

	std::span<T> get(ArenaAllocator *arena) {
		std::span<T> src = stack->from<T>(start);
		if (src.empty()) {
			return std::span<T>();
		}
		std::span<T> dest = arena->allocate_array<T>(src.size());
		memcpy(dest.data(), src.data(), src.size() * sizeof(T));
		stack->restore(start);
		return dest;
	}

private:
	StackingAllocator *stack;
	size_t start;
};
