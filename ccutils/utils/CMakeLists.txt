include_guard(GLOBAL)

add_library(utils OBJECT
        logging.h
        logger.h
        InitVar.h
        StringLiteral.h
        VirtualCopy.h
        CopyPtr.h
        utils.h
        Errorable.h
        stringify.h
        Variant.h
        code_gen.h
        string_hash.h
        Result.h
        Debuggable.h
        pdo_utils.h
)

set_target_properties(utils PROPERTIES LINKER_LANGUAGE CXX)