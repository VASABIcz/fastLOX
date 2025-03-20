
add_library(gen64 OBJECT
        $<TARGET_OBJECTS:utils>
        X86mc.h
        definitions.h
        Register.h
        definitions.cpp
        X86mc.cpp
)