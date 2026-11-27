# ============================================================
# .clusterfuzzlite/Dockerfile
# Base image for building the fuzz harnesses under ClusterFuzzLite.
# Dependabot keeps the pinned digest current.
# ============================================================

FROM gcr.io/oss-fuzz-base/base-builder@sha256:8954f7ece3f9e0731a748ffd91ee966bc032b3340725e773aef3d7e82eb5d8b6

COPY . $SRC/RaftKV
COPY .clusterfuzzlite/build.sh $SRC/

WORKDIR $SRC/RaftKV
