// ============================================================
// backend/fuzz/fuzz_persistent_state.cpp
//
// Fuzzer for FilePersistentState::open(), which parses the file that
// holds a node's currentTerm and votedFor after a restart. A crash
// can leave that file in any state, and a parse error must be
// reported (never swallowed: a lost vote could allow a double vote).
// Arbitrary bytes are written as the state file and open()ed; it may
// return any Status but must not crash or hang.
// ============================================================

#include "raft/FilePersistentState.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // One path per process; libFuzzer feeds inputs sequentially.
    static const std::string path =
        "/tmp/fuzz_persistent_state_" + std::to_string(::getpid()) + ".state";

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    RaftKV::Raft::FilePersistentState state(path);
    if (state.open() == RaftCore::Status::OK) {
        (void)state.currentTerm();
        (void)state.votedFor();
    }
    return 0;
}
