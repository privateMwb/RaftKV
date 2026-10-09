// Unit KvStateMachine Test Suite
// Verifies the replicated map applies committed commands correctly.
//
// Covers:
// - Put then get, overwrite, delete, delete of an absent key
// - lastAppliedIndex() tracking and the onApplied hook
// - Malformed committed entries are counted and skipped, not fatal
// - size() tracks live keys

#include <support/framework.h>

#include "kv/KvCommand.h"
#include "kv/KvStateMachine.h"

using namespace RaftKV::Kv;

static void apply_put(KvStateMachine& machine, RaftCore::LogIndex index, const std::string& key,
                      const std::string& value) {
    machine.apply(index, encodeCommand({KvOp::Put, key, value}));
}

// Verifies a put is readable and an overwrite replaces it.
static void put_then_overwrite() {
    KvStateMachine machine;
    apply_put(machine, 1, "a", "1");
    CHK(machine.get("a") == std::optional<std::string>("1"));

    apply_put(machine, 2, "a", "2");
    CHK(machine.get("a") == std::optional<std::string>("2"));
    CHK(machine.size() == 1);
}

// Verifies delete removes a key and deleting a missing key is harmless.
static void delete_removes_key() {
    KvStateMachine machine;
    apply_put(machine, 1, "a", "1");
    apply_put(machine, 2, "b", "2");

    machine.apply(3, encodeCommand({KvOp::Delete, "a", ""}));
    machine.apply(4, encodeCommand({KvOp::Delete, "never-existed", ""}));

    CHK(!machine.get("a").has_value());
    CHK(machine.get("b") == std::optional<std::string>("2"));
    CHK(machine.size() == 1);
}

// Verifies the applied index advances and the hook fires per apply.
static void applied_index_and_hook() {
    KvStateMachine machine;
    RaftCore::LogIndex seen = 0;
    int calls = 0;
    machine.setOnApplied([&](RaftCore::LogIndex index) {
        seen = index;
        ++calls;
    });

    CHK(machine.lastAppliedIndex() == 0);
    apply_put(machine, 5, "k", "v");
    apply_put(machine, 6, "k", "w");

    CHK(machine.lastAppliedIndex() == 6);
    CHK(seen == 6);
    CHK(calls == 2);
}

// Verifies an undecodable committed entry is counted and skipped.
static void malformed_entry_skipped() {
    KvStateMachine machine;
    apply_put(machine, 1, "a", "1");

    VectorPro::Vector<std::uint8_t> garbage;
    garbage.push_back(99);
    machine.apply(2, garbage);
    apply_put(machine, 3, "b", "2");

    CHK(machine.malformedCount() == 1);
    CHK(machine.lastAppliedIndex() == 3); // the bad entry still consumed its index.
    CHK(machine.get("a").has_value());
    CHK(machine.get("b").has_value());
}

static void run_tests() {
    RUN(put_then_overwrite);
    RUN(delete_removes_key);
    RUN(applied_index_and_hook);
    RUN(malformed_entry_skipped);
}

REGISTER_TEST_SUITE();
