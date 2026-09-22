# Reusable Switch toolchain. GCC handles scalar_storage_order; Clang builds Dawn.
# See README.md for toolchain and Mesa SDK image usage.

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

# Dawn supplies its Python generator dependencies in its source tree.

ENV DEVKITPRO=/opt/devkitpro
ENV PATH="/usr/local/bin:${PATH}"
