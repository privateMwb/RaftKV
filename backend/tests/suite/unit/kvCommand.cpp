// Unit KvCommand Test Suite
// Verifies the replicated command codec: what goes into the Raft log
// payload comes back out unchanged, and garbage never decodes.
//
// Covers:
// - Put and Delete round-trip
// - Binary-safe keys and values (NUL, newlines, high bytes)
// - Empty value and the value-contains-delimiter cases
// - Encoded payloads are never empty (RaftCore reserves empty for no-ops)
// - Truncated, unknown-op, and over-long-key payloads are rejected

#include <support/framework.h>

#include "kv/KvCommand.h"

using namespace RaftKV::Kv;

// Verifies a Put survives encode then decode.
static void put_round_trips() {
    const auto decoded = decodeCommand(encodeCommand({KvOp::Put, "name", "raft"}));

    CHK(decoded.has_value());
    CHK(decoded->op == KvOp::Put);
    CHK(decoded->key == "name");
    CHK(decoded->value == "raft");
}

// Verifies a Delete survives encode then decode with an empty value.
static void delete_round_trips() {
    const auto decoded = decodeCommand(encodeCommand({KvOp::Delete, "name", ""}));

    CHK(decoded.has_value());
    CHK(decoded->op == KvOp::Delete);
    CHK(decoded->key == "name");
    CHK(decoded->value.empty());
}

// Verifies NULs, newlines, and high bytes in keys and values survive intact.
static void binary_bytes_preserved() {
    const std::string key("a\0b\n c", 6);
    const std::string value("\xff\x00\x01\r\n", 5);

    const auto decoded = decodeCommand(encodeCommand({KvOp::Put, key, value}));

    CHK(decoded.has_value());
    CHK(decoded->key == key);
    CHK(decoded->value == value);
}

// Verifies an empty value and a key-length-looking value both decode correctly.
static void empty_and_tricky_values() {
    const auto empty = decodeCommand(encodeCommand({KvOp::Put, "k", ""}));
    CHK(empty.has_value() && empty->key == "k" && empty->value.empty());

    const std::string tricky("\x01\x00\x00\x00zz", 6);
    const auto trick = decodeCommand(encodeCommand({KvOp::Put, "k", tricky}));
    CHK(trick.has_value() && trick->key == "k" && trick->value == tricky);
}

// Verifies even the smallest command encodes to a non-empty payload.
static void payload_never_empty() {
    CHK(encodeCommand({KvOp::Delete, "", ""}).size() == 5);
    CHK(encodeCommand({KvOp::Put, "k", "v"}).size() == 7);
}

// Verifies malformed payloads decode to nullopt instead of misreading.
static void malformed_payloads_rejected() {
    CHK(!decodeCommand(VectorPro::Vector<std::uint8_t>{}).has_value());

    VectorPro::Vector<std::uint8_t> tooShort;
    tooShort.push_back(1);
    tooShort.push_back(0);
    CHK(!decodeCommand(tooShort).has_value());

    auto unknownOp = encodeCommand({KvOp::Put, "k", "v"});
    unknownOp[0] = 9;
    CHK(!decodeCommand(unknownOp).has_value());

    auto longKey = encodeCommand({KvOp::Put, "k", "v"});
    longKey[1] = 200; // claims a 200-byte key in a 7-byte payload.
    CHK(!decodeCommand(longKey).has_value());
}

static void run_tests() {
    RUN(put_round_trips);
    RUN(delete_round_trips);
    RUN(binary_bytes_preserved);
    RUN(empty_and_tricky_values);
    RUN(payload_never_empty);
    RUN(malformed_payloads_rejected);
}

REGISTER_TEST_SUITE();
