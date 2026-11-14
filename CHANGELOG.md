# Changelog

All notable changes to this project are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/).
Until 1.0.0, minor versions may contain breaking API changes.

## [Unreleased]

### Security

- Base OT now uses the Chou-Orlandi "simplest OT" protocol over NIST P-256 (about 128-bit security) instead of
  RSA-2048 (about 112-bit). Received curve points are validated, and every execution uses a fresh sender key.
- The IKNP OT extension hash is now the tweakable correlation-robust hash `H(i, x) = pi(pi(x) XOR i) XOR pi(x)` of
  Guo et al. (IEEE S&P 2020), the construction used by MP-SPDZ and EMP, replacing a weaker fixed-key AES variant.
- Single-value two-input Boolean operators gave server 1 a constant share of 1 when the client shared the inputs, so
  server 0 learned the plaintext inputs. The shares are now uniformly random.

### Removed

- RSA key generation and the RSA helpers `Crypto::generateRsaKeys`, `Crypto::rsaEncrypt`, `Crypto::rsaDecrypt`,
  `RsaKeyContext` and the `Math` modular-arithmetic helpers built on it. Servers no longer exchange RSA keys at start-up.
- The undefined declarations `Crypto::hashMultiTileBatch` and `Crypto::hashMultiTileBatchPair`.

### Added

- `Dockerfile` for a reproducible build and test environment.
- Unit tests that check the OT hash against a reference TCCR implementation.
- Protocol correctness tests for arithmetic add, multiply, less-than and mux, Boolean AND, XOR, less-than and mux, and
  A2B/B2A share conversion, over MPI and TCP, at several bit widths and with edge values. Known operator bugs are
  covered by `known_bug.*` tests that are expected to fail until the bugs are fixed.

### Fixed

- CMake configuration failed when TBB was installed, because the `PARSEC_HAS_TBB` definition named a non-existent
  target.

## [0.1.0]

Initial public version.
