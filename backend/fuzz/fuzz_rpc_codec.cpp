// ============================================================
// backend/fuzz/fuzz_rpc_codec.cpp
//
// Fuzzer for the four peer-RPC decoders in net/RpcCodec. These parse
// request bodies that arrive over the network, so they are RaftKV's
// main untrusted-bytes surface.
//
// The first input byte picks the decoder; the rest is the message.
// A decoder may reject anything, but it must never crash, and
// whatever it accepts must survive a round trip: re-encoding the
// decoded message and decoding that again has to succeed and encode
// to the same bytes.
// ============================================================

#include "net/RpcCodec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

template <typename Decoder> void checkRoundTrip(Decoder decode, std::string_view bytes) {
    const auto first = decode(bytes);
    if (!first)
        return;

    const std::string canonical = RaftKV::Net::encode(*first);
    const auto second = decode(canonical);
    if (!second || RaftKV::Net::encode(*second) != canonical)
        __builtin_trap();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0)
        return 0;

    const std::uint8_t selector = data[0] % 4;
    const std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);

    switch (selector) {
    case 0:
        checkRoundTrip(RaftKV::Net::decodeRequestVoteArgs, bytes);
        break;
    case 1:
        checkRoundTrip(RaftKV::Net::decodeRequestVoteReply, bytes);
        break;
    case 2:
        checkRoundTrip(RaftKV::Net::decodeAppendEntriesArgs, bytes);
        break;
    default:
        checkRoundTrip(RaftKV::Net::decodeAppendEntriesReply, bytes);
        break;
    }
    return 0;
}
