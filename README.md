<img src="https://sdmntprwestus3.oaiusercontent.com/files/00000000-5b4c-61fd-9c4e-873d0ed222b2/raw?se=2025-08-11T10%3A20%3A09Z&sp=r&sv=2024-08-04&sr=b&scid=e3a263c7-bbb7-5bac-a91d-5ddd8f9d97e6&skoid=24a7dec3-38fc-4904-b888-8abe0855c442&sktid=a48cca56-e6da-484e-a814-9c849652bcb3&skt=2025-08-10T16%3A02%3A03Z&ske=2025-08-11T16%3A02%3A03Z&sks=b&skv=2024-08-04&sig=%2BbqDHl067KwfeiT7Vub3f5hhS9dFYiPiLrp%2BZlOhIQg%3D" alt="DIRTY CODE" width="200"/>

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