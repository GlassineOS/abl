# The build environment: Ubuntu 20.04's Clang 10 and binutils, Python 2 for
# upstream's build scripts, and Python 3 with cryptography for qtestsign.
FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive TZ=UTC

RUN apt-get update && apt-get install -y \
        build-essential clang llvm binutils-aarch64-linux-gnu \
        python2 python-is-python2 python3 python3-pip \
        uuid-dev nasm iasl git \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 20.04's python3-cryptography is older than qtestsign needs (3.1).
RUN pip3 install cryptography

WORKDIR /workspace/src
