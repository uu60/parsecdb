# Contributing to Parsec

Thank you for helping improve Parsec. Contributions to documentation, portability, testing, performance, MPC
primitives, and the database layer are welcome.

## Before opening a change

- Open an issue before making a large API, protocol, or architecture change.
- Do not report suspected vulnerabilities in a public issue; follow [SECURITY.md](SECURITY.md).
- Keep benchmark-only tuning separate from correctness and protocol changes.
- State any change to the trust model, observable metadata, communication pattern, randomness, or reconstruction
  behavior explicitly.

## Development setup

Install a C++17 compiler, CMake 3.22 or newer, Ninja, MPI, OpenSSL, and Boost. GoogleTest (`libgtest-dev` on
Debian/Ubuntu, `googletest` on Homebrew) is needed for the unit tests; without it they are skipped.

Build the core library and correctness tests:

```bash
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
```

Tests are organized in layers, from unit tests up to the database queries, and named
`<layer>.<group>.<case>.<backend>[.<bmt>]` with matching labels; [docs/testing.md](docs/testing.md) describes each
layer and how to add tests. Run one layer or variant with, for example:

```bash
ctest --preset ci -L unit
ctest --preset ci -L boolean
ctest --preset ci -L background
```

Benchmarks go in `benchmarks/` and are not run by CTest.

Build all developer targets:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run the fast correctness suite with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset ci-sanitizers
cmake --build --preset ci-sanitizers
ASAN_OPTIONS=detect_leaks=0 ctest --preset ci-sanitizers
```

### Full verification

`tools/verify.py` builds everything and runs every layer in order, one `name ........ OK` line per check and a summary
per layer; it also runs the queries under the paper configurations and further settings and smoke-runs the Figure 4
microbenchmarks. It exits non-zero on any failure and keeps the failed checks' logs in `<build-dir>/verify-logs/`.

```bash
tools/verify.py                 # full run, about 5 minutes on 12 cores
tools/verify.py --quick         # registered tests only
tools/verify.py --sanitizers    # also layers 1-8 under ASan/UBSan
```

Pass extra CMake arguments with `--cmake-arg`, for example `--cmake-arg=-DCMAKE_PREFIX_PATH=/opt/googletest`. Run it
before opening a pull request that touches protocols, operators or the database layer.

## Pull requests

Keep each pull request focused and include:

1. The problem and intended behavior.
2. Tests or a reproducible validation command.
3. Security and performance implications.
4. Documentation updates for public behavior or configuration changes.

Put library code in the `parsecdb` namespace; executables (benchmarks, experiments, tests) may use
`using namespace parsecdb;`. Format C++ changes with `tools/format.sh` (clang-format 21, the version CI checks with
`tools/format.sh --check`). After the one-time bulk reformat, run `git config blame.ignoreRevsFile
.git-blame-ignore-revs` so `git blame` skips it. New correctness tests
must return a non-zero exit status on failure or provide an unambiguous pass/fail result that CTest can validate.

## Commit messages

Use a short imperative subject, for example `Add CTest coverage for bitwise BMT generation`. Add context in the body
when a change affects protocol behavior or makes a non-obvious performance tradeoff.

By contributing, you agree that your contribution is licensed under the repository's Apache License 2.0.
