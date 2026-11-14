# Reproducible build and run environment for Parsec.
#
#   docker build -t parsecdb .
#   docker run --rm parsecdb                                   # runs the correctness tests
#   docker run --rm parsecdb mpirun -np 3 build/release/benchmarks/database/db_sort
#
# The image builds the release preset (all libraries, benchmarks and experiments) plus the tests.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build python3 \
        libopenmpi-dev openmpi-bin \
        libssl-dev libboost-dev libtbb-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# Open MPI refuses to run as root; build and run as an unprivileged user.
RUN useradd --create-home parsec
USER parsec
WORKDIR /home/parsec/parsec

COPY --chown=parsec:parsec . .

RUN cmake --preset release -DPARSEC_BUILD_TESTS=ON \
    && cmake --build --preset release

# Three ranks may exceed the container's CPU count.
ENV OMPI_MCA_rmaps_base_oversubscribe=1 \
    PRTE_MCA_rmaps_default_mapping_policy=:oversubscribe

CMD ["ctest", "--test-dir", "build/release", "--output-on-failure"]
