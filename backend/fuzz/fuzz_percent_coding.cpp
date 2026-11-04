// ============================================================
// backend/fuzz/fuzz_percent_coding.cpp
//
// Fuzzer for percentDecode()/percentEncode(), which handle the
// client-supplied `:key` segment of /kv/:key.
//
//   - percentDecode() of arbitrary text must not crash, and anything
//     it accepts must survive encode -> decode unchanged.
//   - percentEncode() of arbitrary bytes must always decode back to
//     exactly the input.
// ============================================================

#include "net/PercentCoding.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);

    if (const auto decoded = RaftKV::Net::percentDecode(text)) {
        const auto again = RaftKV::Net::percentDecode(RaftKV::Net::percentEncode(*decoded));
        if (!again || *again != *decoded)
            __builtin_trap();
    }

    const auto roundTrip = RaftKV::Net::percentDecode(RaftKV::Net::percentEncode(text));
    if (!roundTrip || *roundTrip != text)
        __builtin_trap();

    return 0;
}
