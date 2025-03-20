

add_library(vi_codegen OBJECT
        SSARegister.h
        IRInstructions.h
        CodeBlock.h
        IRGen.h
        ControlFlowGraph.h
        forward.h
        CodeGen.h
        IRInstruction.h
        SSARegisterHandle.h
        IRGenCtx.h
        SSARegisterHandle.cpp
        Assembler.h
        StackAllocator.h
        BinaryOutput.cpp
        BinaryOutput.h
        optimizations.h
)

add_subdirectory(../gen64 build)

add_library(vi_codegen_x86 OBJECT
        $<TARGET_OBJECTS:vi_codegen>
        $<TARGET_OBJECTS:gen64>
        x86/X86Assembler.h
        x86/X86Assembler.cpp
        x86/RegAlloc.h
        x86/RegAlloc.cpp
)