

add_library(lexer OBJECT
        $<TARGET_OBJECTS:utils>
        forward.h
        Token.h
        Position.h
        lexerExceptions.h
        SourceProvider.h
        tokenize.h
        lexingUnits.h
)

set_target_properties(lexer PROPERTIES LINKER_LANGUAGE CXX)
