#pragma once

#if defined(__unix__) || defined(__APPLE__)
#define UNIX_LIKE
#endif

namespace arch {
    enum class OsType {
        UNIX,
        MACOS,
        WINDOWS
    };

    constexpr OsType CURRENT_OS =
#ifdef __unix__
            OsType::UNIX;
#elif __APPLE__
    OsType::MACOS
#elif __windows__
    OsType::WINDOWS
#endif

    constexpr bool isUnixLike() {
        return CURRENT_OS == OsType::UNIX or CURRENT_OS == OsType::MACOS;
    }

    constexpr bool isWindows() {
        return CURRENT_OS == OsType::WINDOWS;
    }

    enum class CpuType {
        ARM64,
        X86_64
    };

    constexpr CpuType CURRENT_CPU = CpuType::X86_64;
}