# Changelog

All notable changes to this project are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/).
Until 1.0.0, minor versions may contain breaking API changes.

## [Unreleased]

### Changed

- Share widths must be powers of two from 1 to 64. Arithmetic and Boolean batch operators throw
  `std::invalid_argument` for other widths instead of silently producing wrong results (Boolean less-than was wrong
  for non-power-of-two widths, and Boolean AND threw when a field crossed a 64-bit word).
- `ArithSecret`, `BoolSecret` and `BitSecret` keep their API but run the batch operators on one-element batches.
- `View` and `Views` have one batched implementation per operator instead of separate single-batch and multi-batch
  copies; with multithreading disabled the whole input runs as one batch. `Secrets::sort` likewise has one
  implementation for both sharings.
- Background BMT queues take their capacity from `--max_bmts` (default 10000000), and the arithmetic queue is filled in
  batches. `ENABLE_IKNP_MULTITHREAD` defaults to false. The client rank no longer creates a worker pool.

### Security

- Base OT now uses the Chou-Orlandi "simplest OT" protocol over NIST P-256 (about 128-bit security) instead of
  RSA-2048 (about 112-bit). Received curve points are validated, and every execution uses a fresh sender key.
- The IKNP OT extension hash is now the tweakable correlation-robust hash `H(i, x) = pi(pi(x) XOR i) XOR pi(x)` of
  Guo et al. (IEEE S&P 2020), the construction used by MP-SPDZ and EMP, replacing a weaker fixed-key AES variant.
- Single-value two-input Boolean operators gave server 1 a constant share of 1 when the client shared the inputs, so
  server 0 learned the plaintext inputs. The shares are now uniformly random.

### Removed

- All single-value operators (`compute/single/*`, `AbstractSingleOperator`, `BaseOtOperator`, `RandOtOperator`,
  `AbstractOtOperator`, `BmtGenerator`, `BitwiseBmtGenerator`); use the batch operators, or the secret containers for
  single values.
- The `bmt_pipeline` BMT mode and `PipelineBitwiseBmtBatchGenerator`.
- Precise compaction (`--disable_precise_compaction=false`). It opened the exact number of valid rows to the servers
  and gave wrong results in several experiments. Compaction now always reveals only the bit length of that number.

- RSA key generation and the RSA helpers `Crypto::generateRsaKeys`, `Crypto::rsaEncrypt`, `Crypto::rsaDecrypt`,
  `RsaKeyContext` and the `Math` modular-arithmetic helpers built on it. Servers no longer exchange RSA keys at start-up.
- The undefined declarations `Crypto::hashMultiTileBatch` and `Crypto::hashMultiTileBatchPair`.

### Added

- A layered test suite named `<layer>.<group>.<case>.<backend>[.<bmt>]` with matching labels, described in
  `docs/testing.md`: unit, communication (new), OT, BMT generation, Boolean and arithmetic operators, share conversion
  and secret containers (each operator under JIT, fixed and background BMTs over MPI, and over TCP), database operators
  (new: every `View`/`Views` operator against plaintext in four execution modes) and queries. Redundant OT and BMT
  tests were merged; the arithmetic BMT generator is now tested directly.

- `tools/verify.py`: builds everything and runs every correctness check (all tests, the experiments under the paper
  configurations, BMT modes, threading and batch sizes over MPI and TCP, the Figure 4 microbenchmarks, and optionally a
  sanitizer build), printing one `name ........ OK` line per check and a summary.

- `Dockerfile` for a reproducible build and test environment.
- Unit tests that check the OT hash against a reference TCCR implementation.
- Protocol correctness tests for arithmetic add, multiply, less-than and mux, Boolean AND, XOR, less-than and mux, and
  A2B/B2A share conversion and the secret containers, over MPI and TCP, at every supported width with edge values.
- `tests/database/verify_exp.py --bmt-method=... --max-bmts=...` runs the experiment checks with JIT, fixed or
  background BMTs.

### Fixed
- MPI transfer compression (`--enable_transfer_compression=true`) sent widths 9..16 through `int` buffers typed
  `MPI_INT16_T`, garbling the data, and asynchronous compressed transfers freed their buffers twice.
- Arithmetic less-than and equality refused background BMTs, and arithmetic mux ignored BMTs passed with `setBmts()`
  (its two multiplications polled the queue themselves, concurrently with intra-operator parallelism). The arithmetic
  `Secrets::sort` now hands every parallel batch BMTs polled in order, as the Boolean sort does.
- `System::auxiliaryTask()` rejects task tags that would set the sign bit of the message tag, which MPI rejects, and
  `Comm::wait()` reports an asynchronous transfer that failed to start instead of dereferencing a null request.
- Grouped `max` over more rows than one batch wrote the new validity into the max column instead of `$valid`, and
  `count` after an outer join counted unmatched rows when the rows exceeded one batch. Both now behave like the
  single-batch path at every size; `tests/database/correctness_db_aggregates` checks them against plaintext.
- Background BMTs with `--disable_multi_thread=true` hung at start-up (the generator loop ran on the calling thread) and
  then crashed inside MPI (initialized without thread support). Generators now get their own threads and MPI is
  initialized with `MPI_THREAD_MULTIPLE` whenever background BMTs are used.
- `System::nextTask()` used the task-tag bit count as a mask; it now plainly returns the fixed query task after the
  background generators' tags. The filter, nested-loop-join and `exp_6` task tags, previously hard-coded as 3, 4 and 5,
  come from `System::auxiliaryTask()` so they move past the generators' tags with more `--bmt_queue_num` queues
  (unchanged values with the defaults) and fail loudly when they do not fit in `--task_tag_bits`.

- Background BMT mode (`--bmt_method=bmt_background --disable_arith=false`) works again, porting the fixes of the
  reviewed artifact release: message tags no longer drop the high bits of database message tags (which aliased
  database traffic with the BMT generators), generator threads stop in step on both servers and are joined before
  their queues are freed, operators run in parallel receive BMTs polled in a fixed order on one thread, arithmetic
  multiplication and A2B support background BMTs, and `Secrets::sort` no longer polls twice the BMTs a mux needs.
- Message-tag collisions between concurrent batches in experiments `exp_2` and `exp_6`.
- Batch XOR crashed on the client rank, and batch B2A crashed when the client shared its inputs.
  `Bool/ArithBatchOperator` leaked the input vectors received from the client.
- Undefined behavior in arithmetic `Secrets::sort` (a vector was inserted into itself).

- CMake configuration failed when TBB was installed, because the `PARSEC_HAS_TBB` definition named a non-existent
  target.

## [0.1.0]

Initial public version.
