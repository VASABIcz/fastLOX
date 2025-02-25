# NI-RUN C++ template repository

This is a C++ template for student projects for the course [NI-RUN][NI-RUN].
The contents of this repository are available as a starting point, place to
copy files or pieces of code from, or just a showcase of various things useful
for the course (parsing, AST representation, custom memory allocation, memory
tracking, CI setup, etc.).

The implementation in this repository is written in C++20 and is explicitly
compiled with the GNU extensions ("GNU++20"). This is a deliberate decision,
since even though the extensions may not be used, some of them may and will be
useful for implementation of the interpreter and VM.

## Building

This repository uses the [Meson](https://mesonbuild.com/) build system. A
release build can be compiled to the `build` directory with:

```console
meson setup build --buildtype release
meson compile -C build
```

For debug build, instead use:

```console
meson setup build
```

For debug build with sanitizers, instead use:

```console
meson setup build -D b_sanitize=address,undefined
```

After compilation, the `lox` binary will be available at `build/lox`. All build
artifacts are contained in the `builddir` directory (you are free to choose any
other name) and can be safely deleted.

More simply, you can just use `g++`, e.g.:

```console
g++ -g3 -std=gnu++20 -Wall -Wextra -fsanitize=address,undefined src/*.cpp -o lox
```

Customize compile flags as you like, but note that `-std=gnu++20` is needed to
build the sources. (You don't want to use `-Wpedantic` since it is in contrast
to using the implementation define GNU extensions.)

### IDE integration

Meson generates `compile_commands.json` file in the build directory. To be able
to use it with your IDE, you may need to put it into the root of the project.
You can do this with a symlink. Suppose the build directory is in the project
root and is called `build`, then you can do:

```console
ln -s build/compile_commands.json compile_commands.json
```

The clangd LSP should be able to find it in the `build` directory.

## CI

CI is defined in `.gitlab-ci.yml`. It uses the [lox-test] image, builds the
project with meson, checks the sources with `cppcheck` and runs the test suite.

Normally we would run with sanitizers in the CI, but there seems to be
non-detereministic bug in the sanitizers, so it is currently disabled.

You can run the CI locally with `gitlab-runner`. For the first time, you need
to login to have access to the NI-RUN gitlab container registry:

```console
docker login gitlab.fit.cvut.cz:5050
```

Then you can run the tests with:

```console
gitlab-runner exec docker --env CI_REGISTRY=gitlab.fit.cvut.cz:5050 --docker-volumes=$PWD/output:/lox-test/output test
```

Additionally the command above mounts the `output` directory to the docker
container, which allows to get the outputs of the failed tests.



[lox]: https://craftinginterpreters.com/the-lox-language.html
[NI-RUN]: https://courses.fit.cvut.cz/NI-RUN/
[lox-test]: https://gitlab.fit.cvut.cz/NI-RUN/lox-test
