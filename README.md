# ⚠️ Reader Discretion is Advised This code is DIRTY ⚠️

# fastLOX

built as part of [NI-RUN CVUT course](https://bilakniha.cvut.cz/en/predmet6114506.html)

implementation of [LOX](https://craftinginterpreters.com/the-lox-language.html)

from scratch implementation C++23/26 + libc/c++

## features:
### value clasification
values are clasified in 3 categories `Linerizer`
- global
  - stored in static array `GLOBALS_TABLE`
- local
  - stored in stack frames
- caputured
  - stored in `FunctionRef` objects
  - linked list of `FunctionRef`
  - those can be const `LoxValue` / mutable `LoxValue*`
### value representation
values are represented with 8B NaN boxing
- 3bit tag => 8 types `ValueType2`
- tags are placed so some operations can be more performant
  - falsy values
  - callable values
  - addable values
  - 1 bit diff between true/false
- older version used 16B enum taging
### dynamic code gen
program is lowered into custom SSA IR `Compiler` which is then compiled into x86_64 `X86MilaAssembler`
- generated code calls backs into runtime for more complex operation `callBuiltin` & `namespace builtin`
- most simpler operations are implemented directly in assembly (arithmetics, type guards)
### more complex runtime operations are sped up using monomorphic ICs
- readField
- writeField
- callMethod
### WIP
- bit of static analysis to deduce types, remove guards, speculate globals
### TODO
- proper GC
- speculative optimizations, use ICs feedback to generate more optimal code
  - would require deopt into generic code?
  - tracking of values registers/stack

### usage
- clone ccutils