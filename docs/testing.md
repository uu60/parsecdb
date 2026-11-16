# Testing

Parsec's correctness checks are organized in layers. Each layer builds on the ones before it, so when something
breaks, the lowest failing layer is the place to look.

| # | Layer | CTest names | What it checks | Variants |
|---|---|---|---|---|
| 1 | Unit | `unit.<Suite>.<Case>` | Single-process GoogleTest cases: crypto (PRG, OT hash, transposes), ring arithmetic, randomness, SIMD, strings | — |
| 2 | Communication | `protocol.comm.*` | Messages between every pair of ranks: scalars and vectors at every width, empty and large vectors, strings, asynchronous, concurrent and interleaved transfers, with and without transfer compression | MPI, TCP |
| 3 | Oblivious transfer | `protocol.ot.*` | Base OT (batches of one and many, 128-bit messages) and IKNP OT extension (word and packed-bit modes), each server as sender; IKNP set-up invariants | MPI, TCP |
| 4 | BMT generation | `protocol.bmt.*` | Bitwise (a & b = c) and arithmetic (a · b = c mod 2^w) Beaver triples generated from OT, at several widths | MPI, TCP |
| 5 | Boolean operators | `protocol.boolean.*` | XOR, AND, equality, less-than and mux, batched and through the secret containers, at every width with edge values | MPI × {JIT, fixed, background BMTs}, TCP |
| 6 | Arithmetic operators | `protocol.arithmetic.*` | Add, multiply, less-than and mux, same coverage | same |
| 7 | Share conversion | `protocol.conversion.*` | A2B and B2A, both OT directions, round trips | same |
| 8 | Secret containers | `protocol.secret.*` | `Secrets::sort` over Boolean and arithmetic shares | same |
| 9 | Database operators | `db.operators.*` | Every `View`/`Views` operator against a plaintext evaluation, in four execution modes (one batch, batches of two, batches of two without compaction, baseline) | MPI × {JIT, fixed, background, background single-threaded}, TCP |
| 10 | Queries | `query.exp_<N>.*` | `exp_1`..`exp_8` in check mode against their expected results | MPI × {JIT, background}, TCP |

Test names follow `<layer>.<group>.<case>.<backend>[.<bmt>]`, and every test carries the matching labels, so a layer or
variant can be selected directly:

```bash
ctest --test-dir build/dev -L unit           # one layer
ctest --test-dir build/dev -L boolean        # one group
ctest --test-dir build/dev -L background     # every test that uses background BMTs
ctest --test-dir build/dev -L tcp            # every TCP run
ctest --test-dir build/dev -LE query         # everything except the queries
```

## Full verification

`tools/verify.py` builds every target and runs all layers in order, printing one line per check and a summary per
layer:

```text
[ 6] Boolean operators
     and xor [mpi, background] .......................................... OK
     and xor [mpi, fixed] ............................................... OK
     ...
Summary
     Boolean operators (16/16) .......................................... OK
```

Beyond the registered tests it runs the queries under the configurations the paper evaluates (`parsec`,
`parsec_base`, `parsec_noncompact`) and under further BMT, threading, batch-size and backend settings, smoke-runs the
Figure 4 microbenchmarks, and, with `--sanitizers`, repeats layers 1-8 under AddressSanitizer and UBSan.

```bash
tools/verify.py                  # everything except sanitizers
tools/verify.py --quick          # registered tests only
tools/verify.py --sanitizers     # also the sanitizer build
tools/verify.py --skip-build --build-dir build/dev --only db --only queries
```

Logs of failed checks are written to `<build-dir>/verify-logs/`, and the exit status is non-zero when anything
failed. Run it before opening a pull request that touches protocols, operators or the database layer.

## Adding tests

- Logic without communication goes into a GoogleTest case under `tests/unit/`.
- A protocol test is a program under `tests/protocol/` that prints `FAIL` or `MISMATCH` and exits non-zero on any
  mismatch. Register it in `tests/protocol/CMakeLists.txt` under the right group; operators that consume BMTs should
  run in all BMT variants.
- Database operators are checked in `tests/database/correctness_db_operators.cpp`: add the query and its plaintext
  evaluation there, so it runs in every execution mode.
- Only power-of-two share widths from 1 to 64 are supported; tests must not use other widths.
