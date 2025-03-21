cmake_minimum_required(VERSION 3.22)
project(LOX)
set(CMAKE_CXX_STANDARD 23)

include(FetchContent)

set(USE_OPENGL OFF)
set(USE_SANITIZER OFF)

set(BASE_FLAGS "-fno-exceptions --time-report -Wall -Wextra -Werror -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-unused-function -Wno-nonnull-compare -Wno-missing-field-initializers -Wno-changes-meaning -Wno-invalid-offsetof -m64 -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${BASE_FLAGS} -O3 -fno-exceptions -flto=1 -march=native") # -march=native # -march=native -fsanitize=undefined -fsanitize=address -fsanitize=leak  # -s -ggdb -fsanitize=undefined
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${BASE_FLAGS} -g -ggdb") # -D_GLIBCXX_DEBUG
# -D_GLIBCXX_ASSERTIONS

if(NOT APPLE AND NOT WINDOWS AND NOT MINGW AND USE_SANITIZER)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address,undefined")
endif()

if (WINDOWS OR MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --static -Wa,-mbig-obj")
    set(USE_OPENGL OFF)
endif()

add_subdirectory(utils)

add_subdirectory(parsing)
add_subdirectory(lexing)
add_subdirectory(codegen)

add_executable(lox
        $<TARGET_OBJECTS:lexer>
        $<TARGET_OBJECTS:parser>
        $<TARGET_OBJECTS:utils>
        $<TARGET_OBJECTS:gen64>
        $<TARGET_OBJECTS:vi_codegen_x86>
        $<TARGET_OBJECTS:vi_codegen>

        lox/lox.cpp
)

# Set global property (all targets are impacted)
set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_COMMAND} -E time")