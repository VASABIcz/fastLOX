#pragma once

#include <cstddef>
#include <cstring>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#define LINUX
#else
#include "memoryapi.h"
#endif

namespace cg {
    inline void* allocateJIT(size_t size) {
#ifdef LINUX
        auto* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
#else
        auto* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
#endif

        // println("ALLOCATED EXECTUABLE PAGE {}", ptr);
        assert(ptr != nullptr);
        return ptr;
    }

    inline void* allocateJIT(span<const u8> code) {
        auto* ptr = allocateJIT(code.size());
        std::memcpy(ptr, code.data(), code.size());

        return ptr;
    }

    inline void makeRW(void* ptr, size_t size) {
        assert(ptr != nullptr);
#ifdef LINUX
        mprotect(ptr, size, PROT_WRITE | PROT_READ);
#else
        DWORD dummy;
        VirtualProtect(ptr, size, PAGE_READWRITE, &dummy);
#endif
    }

    inline void makeRX(void* ptr, size_t size) {
        assert(ptr != nullptr);
#ifdef LINUX
        mprotect(ptr, size, PROT_READ | PROT_EXEC);
#else
        DWORD dummy;
        VirtualProtect(ptr, size, PAGE_EXECUTE_READ, &dummy);
#endif
    }

    inline void release(void* ptr, size_t size) {
        assert(ptr != nullptr);
#ifdef LINUX
        munmap(ptr, size);
#else
        VirtualFree(ptr, size, MEM_RELEASE);
#endif
    }
}