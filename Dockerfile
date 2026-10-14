# syntax=docker/dockerfile:1

# ============================================================
# RaftKV node image
#
# Builds the backend in backend/ and ships one small runtime
# image. The same image runs every node; docker-compose.yml
# gives each container its own id, port, and peer list.
#
# Nested git submodules under backend/external/ are cloned by
# init-nested-submodules.sh before configuring, since a plain
# `COPY . .` brings in the tracked .gitmodules files but not the
# submodule contents. NOTE: that script clones each library's
# default branch (--depth 1), not the commit this repo pins, so
# an image build tracks the libraries' latest main.
# ============================================================

FROM ubuntu:24.04 AS build

RUN apt-get update && \
    apt-get install -y \
        build-essential \
        ca-certificates \
        cmake \
        git \
        && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

RUN chmod +x init-nested-submodules.sh && ./init-nested-submodules.sh

WORKDIR /app/backend

# RAFTKV_BUILD_TESTS=OFF: the node image doesn't need the test binary (it is
# ON by default). BUILD_* switch off the submodules' own tests/examples.
RUN cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DRAFTKV_BUILD_TESTS=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_REGRESSION=OFF \
    -DBUILD_EXAMPLES=OFF

RUN cmake --build build --config Release --target raftkv_backend -j$(nproc)


FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y \
        libstdc++6 \
        && \
    rm -rf /var/lib/apt/lists/*

# The executable's name; must match the target in backend/CMakeLists.txt.
# ARG + ENV (not ARG alone): ARG values don't survive into the running container.
ARG BINARY_NAME=raftkv_backend
ENV BINARY_NAME=${BINARY_NAME}

WORKDIR /app

COPY --from=build /app/backend/build/${BINARY_NAME} /app/${BINARY_NAME}

# Run as an unprivileged user. /app/data holds the node's log and term/vote file;
# mount a volume there (docker-compose.yml does) so a node survives being recreated.
RUN useradd --system --no-create-home --shell /usr/sbin/nologin raftkv && \
    mkdir -p /app/data && \
    chown raftkv /app/data
USER raftkv
VOLUME ["/app/data"]

EXPOSE 8101

# The command line (docker-compose.yml supplies it) is passed straight to the node;
# with no arguments it runs a standalone single node on :8101 with data in /app/data.
ENTRYPOINT ["sh", "-c", "exec /app/$BINARY_NAME \"$@\"", "--"]
CMD ["--id", "node-1", "--port", "8101", "--data", "/app/data", "--no-repl"]
