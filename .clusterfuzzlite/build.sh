#!/bin/bash -eu
# ============================================================
# .clusterfuzzlite/build.sh
#
# Builds every backend/fuzz/fuzz_*.cpp harness for ClusterFuzzLite.
# Only the few RaftKV sources the harnesses actually need are
# compiled (not the whole app), so the fuzz build doesn't pull in
# FalconHTTP or ThrottlePro. Sanitizer flags come from $CXXFLAGS.
#
# Harnesses are discovered by glob: drop a new
# backend/fuzz/fuzz_<target>.cpp in place and it becomes its own
# $OUT binary with no edit here.
#
# Seed inputs: backend/fuzz/corpus/<harness-name>/ is zipped into
# $OUT/<harness-name>_seed_corpus.zip when present.
# ============================================================

cd "${SRC}/RaftKV/backend"

INCLUDES=(-Isrc -Iinclude -Iexternal/raftcore/include)
while IFS= read -r dir; do
  INCLUDES+=(-I"${dir}")
done < <(find external/raftcore/libs -type d -name include 2>/dev/null)

STD=-std=c++23

# RaftKV sources under test, compiled once and shared by every harness.
OBJECTS=()
for source in src/net/RpcCodec.cpp src/net/PercentCoding.cpp src/raft/FilePersistentState.cpp; do
  object="${WORK}/$(basename "${source}" .cpp).o"
  $CXX $CXXFLAGS ${STD} "${INCLUDES[@]}" -c "${source}" -o "${object}"
  OBJECTS+=("${object}")
done

# One binary per harness.
for harness in fuzz/fuzz_*.cpp; do
  name="$(basename "${harness}" .cpp)"
  $CXX $CXXFLAGS ${STD} "${INCLUDES[@]}" \
    "${harness}" \
    "${OBJECTS[@]}" \
    $LIB_FUZZING_ENGINE \
    -o "${OUT}/${name}"

  if [ -d "fuzz/corpus/${name}" ]; then
    (cd "fuzz/corpus/${name}" && zip -q -r "${OUT}/${name}_seed_corpus.zip" .)
  fi
done
