include_guard(GLOBAL)

add_library(ccutils pepa.cpp)
add_executable(anotator anotator.cpp)

add_executable(gen
        $<TARGET_OBJECTS:utils>
        $<TARGET_OBJECTS:gen64>
        genTest.cpp
)

add_subdirectory(codegen)
add_subdirectory(gen64)
add_subdirectory(lexing)
add_subdirectory(parsing)
add_subdirectory(utils)

target_include_directories(ccutils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})