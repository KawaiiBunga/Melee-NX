# melee-nx build image: devkitA64 (GCC) + LLVM Clang 19.
#
# Why both compilers: the game's C sources need GCC's
# `__attribute__((scalar_storage_order("big-endian")))`, which Clang does not
# implement, while Dawn's C++ needs a compiler new enough for C++20/23 against
# devkitA64's libstdc++-15 headers — devkitPro's bundled Clang is too old and
# fails several hundred Dawn/Tint translation units on std::ranges, consteval
# and <format>. So: GCC for C, Clang 19 for C++. See switch/cmake/SwitchGCC.cmake.
#
# The base image carries the devkitA64 sysroot (libnx + libstdc++-15) at
# /opt/devkitpro.
#
# Build it with:  bash builder/docker.sh image
# or directly:    docker build -t melee-nx-build switch/docker
#
# This is the same recipe as KartPad-NX's `kartpad-dawn` image; either works,
# and builder/docker.sh accepts an existing one via MELEE_DOCKER_IMAGE.

FROM devkitpro/devkita64:latest

ARG LLVM_VERSION=19
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg lsb-release software-properties-common \
        ninja-build cmake git python3 python3-pip pkg-config \
    && curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh ${LLVM_VERSION} \
    && ln -sf /usr/bin/clang-${LLVM_VERSION}   /usr/local/bin/clang \
    && ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/local/bin/clang++ \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# Dawn's code generators run at build time on host python with jinja2 and
# markupsafe, both already vendored under ref/dawn/third_party — nothing further
# to install here.

ENV DEVKITPRO=/opt/devkitpro
ENV PATH="/usr/local/bin:${PATH}"
