// ============================================================
// backend/fuzz/fuzz_kv_command.cpp
//
// Fuzzer for the KvCommand log-payload format. Every replicated
// entry is decoded with decodeCommand() on every node, so a payload
// that crashes it would take down the whole cluster.
//
// Two checks:
//   1. Arbitrary bytes: decodeCommand() must not crash, and anything
//      it accepts must re-encode to exactly the same bytes.
//   2. Arbitrary commands built from the input: encode then decode
//      must give back the same command.
// ============================================================

#include "kv/KvCommand.h"

#include <cstddef>
#include <cstdint>
#include <string>

using RaftKV::Kv::KvCommand;
using RaftKV::Kv::KvOp;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // 1. Raw bytes as a payload.
    VectorPro::Vector<std::uint8_t> payload;
    for (std::size_t i = 0; i < size; ++i)
        payload.push_back(data[i]);

    if (const auto command = RaftKV::Kv::decodeCommand(payload)) {
        const auto again = RaftKV::Kv::encodeCommand(*command);
        if (again.size() != payload.size())
            __builtin_trap();
        for (std::size_t i = 0; i < payload.size(); ++i)
            if (again[i] != payload[i])
                __builtin_trap();
    }

    // 2. Structured command: byte 0 = op, byte 1 = key length, rest = key then value.
    if (size >= 2) {
        KvCommand command;
        command.op = (data[0] & 1) ? KvOp::Put : KvOp::Delete;

        const std::size_t keyLength = data[1] % (size - 1);
        command.key.assign(reinterpret_cast<const char*>(data) + 2, keyLength);
        if (command.op == KvOp::Put)
            command.value.assign(reinterpret_cast<const char*>(data) + 2 + keyLength,
                                 size - 2 - keyLength);

        const auto decoded = RaftKV::Kv::decodeCommand(RaftKV::Kv::encodeCommand(command));
        if (!decoded || decoded->op != command.op || decoded->key != command.key ||
            decoded->value != command.value)
            __builtin_trap();
    }
    return 0;
}
