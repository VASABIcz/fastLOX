cmake_minimum_required(VERSION 3.22)
project(LOX)
set(CMAKE_CXX_STANDARD 26)

include(FetchContent)

set(USE_SANITIZER OFF)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)


# set(CMAKE_CXX_FLAGS_DEBUG "-fno-exceptions --time-report -ggdb")
set(BASE_FLAGS "-fno-exceptions -Wall -Wextra -Werror -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-unused-function -m64 -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${BASE_FLAGS} -O2 -fno-exceptions") # -march=native # -march=native -fsanitize=undefined -fsanitize=address -fsanitize=leak  # -s -ggdb -fsanitize=undefined
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${BASE_FLAGS} -g -ggdb") # -D_GLIBCXX_DEBUG
# -D_GLIBCXX_ASSERTIONS
# -flto=1 --time-report -Wno-nonnull-compare

if(NOT APPLE AND NOT WINDOWS AND NOT MINGW AND USE_SANITIZER)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address,undefined")
endif()

if (WINDOWS OR MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --static -Wa,-mbig-obj")
endif()

add_subdirectory(ccutils)

add_executable(lox
        $<TARGET_OBJECTS:lexer>
        $<TARGET_OBJECTS:parser>
        $<TARGET_OBJECTS:utils>
        $<TARGET_OBJECTS:gen64>
        $<TARGET_OBJECTS:vi_codegen_x86>
        $<TARGET_OBJECTS:vi_codegen>

        lox/lox.cpp
)

target_link_libraries(lox PRIVATE ccutils)

# Set global property (all targets are impacted)
set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_COMMAND} -E time")