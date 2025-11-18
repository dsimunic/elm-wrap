# ============================================================
FROM debian:bookworm
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git build-essential python3 pkg-config \
    cmake ninja-build clang lld zlib1g-dev libxml2-dev \
    libcurl4-openssl-dev libnghttp2-dev libidn2-dev \
    librtmp-dev libssh2-1-dev libpsl-dev libssl-dev \
    libkrb5-dev libldap2-dev libzstd-dev libbrotli-dev \
    gosu \
 && rm -rf /var/lib/apt/lists/*

# Workspace
WORKDIR /work

# Add entrypoint script
COPY --chown=root:root entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Helpful defaults for downstream builds; entrypoint also exports these.
ENV CMAKE_PREFIX_PATH=/opt/llvm-mlir
ENV LD_LIBRARY_PATH=/opt/llvm-mlir/lib
ENV PATH=/opt/llvm-mlir/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ENV CC=clang
ENV CXX=clang++

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
