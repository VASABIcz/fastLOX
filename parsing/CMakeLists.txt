

add_subdirectory(../utils build)

add_library(parser OBJECT
        $<TARGET_OBJECTS:utils>
        TokenProvider.h
        Parser.h
        ParsingUnit.h
        exceptions.h)

set_target_properties(parser PROPERTIES LINKER_LANGUAGE CXX)