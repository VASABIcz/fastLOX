# NI-RUN C++ template repository

### differences from the template
- source in `lox/lox.cpp`
- custom lexer/parser reused from my other projects
- NaN boxed LoxValue
- O(1) locals lookup, no need to traverse environment linked list, storage for locals is determined statically:
  - global scope - stored in globals table
  - global scope inside block - acts as local/up val
  - local variable - standard local variable stored on stack
  - "up val" - local that is captured by function, allocated on heap
- closures have:
  - up value table - utilizing Flexible array member
  - parent - pointer to enclosing closure
- "up values" - are accesed by dereferencing parent N times + offset in up value table

### TODO
- ability to check types? `if v is Number { ... }`
- continue + break statements with support for labeled return `while@a (true) { while@b (true) { break@a } }`
- symbol table? dont allocate new string all the time
- arrays?
- anonymous objects? JS notation? var v = {foo: "stuff", boo: "123"}
- simple JIT support? no speculative optimizations just 1:1 to machine code?
  - maybe we could convert it to SSA and do some simple optimizations + register allocation?
  - some sort of function specialization + caching?