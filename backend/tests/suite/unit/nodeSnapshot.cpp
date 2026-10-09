// Unit NodeSnapshot Test Suite
// Verifies the node-state value type and its JSON form, which is both what
// the dashboard reads and what peers parse when polling each other's /status.
//
// Covers:
// - Round trip with and without a known leader
// - Quotes, backslashes, and control characters in ids survive
// - Role names and parsing
// - Equality detects any single-field change (that is how "something changed" is decided)
// - Malformed JSON, missing fields, a bad role, and non-integer or negative
//   counters are rejected rather than half-parsed
// - Unknown extra fields are tolerated (forward compatibility)

#include <support/framework.h>

#include "node/NodeSnapshot.h"

using namespace RaftKV::Node;

static NodeSnapshot sample() {
    NodeSnapshot snapshot;
    snapshot.id = "node-2";
    snapshot.role = RaftCore::Role::Leader;
    snapshot.term = 7;
    snapshot.leader = "node-2";
    snapshot.commitIndex = 41;
    snapshot.lastApplied = 40;
    snapshot.lastLogIndex = 42;
    snapshot.keys = 9;
    return snapshot;
}

// Verifies a full snapshot survives toJson then parseSnapshot.
static void round_trips_with_leader() {
    const NodeSnapshot original = sample();
    const auto parsed = parseSnapshot(toJson(original));

    CHK(parsed.has_value());
    CHK(parsed.has_value() && *parsed == original);
}

// Verifies an unknown leader is JSON null and parses back to nullopt.
static void round_trips_without_leader() {
    NodeSnapshot original = sample();
    original.role = RaftCore::Role::Candidate;
    original.leader.reset();

    const std::string json = toJson(original);
    CHK(json.find("\"leader\":null") != std::string::npos);

    const auto parsed = parseSnapshot(json);
    CHK(parsed.has_value() && *parsed == original);
    CHK(parsed.has_value() && !parsed->leader.has_value());
}

// Verifies awkward characters in node ids are escaped and restored.
static void awkward_ids_round_trip() {
    NodeSnapshot original = sample();
    original.id = "n\"o\\de\n\t-\x01";
    original.leader = original.id;

    const auto parsed = parseSnapshot(toJson(original));
    CHK(parsed.has_value() && parsed->id == original.id);
    CHK(parsed.has_value() && parsed->leader == original.leader);
}

// Verifies role names and parsing agree, and junk is refused.
static void role_names() {
    CHK(std::string(roleName(RaftCore::Role::Leader)) == "leader");
    CHK(std::string(roleName(RaftCore::Role::Candidate)) == "candidate");
    CHK(std::string(roleName(RaftCore::Role::Follower)) == "follower");

    CHK(parseRole("leader") == std::optional<RaftCore::Role>(RaftCore::Role::Leader));
    CHK(parseRole("candidate") == std::optional<RaftCore::Role>(RaftCore::Role::Candidate));
    CHK(parseRole("follower") == std::optional<RaftCore::Role>(RaftCore::Role::Follower));
    CHK(!parseRole("Leader").has_value());
    CHK(!parseRole("").has_value());
}

// Verifies each field participates in equality.
static void equality_sees_every_field() {
    const NodeSnapshot base = sample();
    CHK(base == sample());

    NodeSnapshot changed = base;
    changed.id = "x";
    CHK(!(changed == base));
    changed = base;
    changed.role = RaftCore::Role::Follower;
    CHK(!(changed == base));
    changed = base;
    changed.term = 8;
    CHK(!(changed == base));
    changed = base;
    changed.leader = "node-3";
    CHK(!(changed == base));
    changed = base;
    changed.commitIndex = 0;
    CHK(!(changed == base));
    changed = base;
    changed.lastApplied = 0;
    CHK(!(changed == base));
    changed = base;
    changed.lastLogIndex = 0;
    CHK(!(changed == base));
    changed = base;
    changed.keys = 0;
    CHK(!(changed == base));
}

// Verifies broken input is rejected as a whole.
static void malformed_input_rejected() {
    CHK(!parseSnapshot("").has_value());
    CHK(!parseSnapshot("not json").has_value());
    CHK(!parseSnapshot("[]").has_value());
    CHK(!parseSnapshot("{").has_value());

    const std::string good = toJson(sample());
    CHK(parseSnapshot(good).has_value());

    // Missing field.
    std::string missing = good;
    missing.replace(missing.find("\"keys\":9"), 9, "\"zzz\":9");
    CHK(!parseSnapshot(missing).has_value());

    // Unknown role.
    std::string badRole = good;
    badRole.replace(badRole.find("leader\",\"term"), 6, "wizard");
    CHK(!parseSnapshot(badRole).has_value());

    // Counters must be non-negative integers.
    std::string negative = good;
    negative.replace(negative.find("\"term\":7"), 9, "\"term\":-1");
    CHK(!parseSnapshot(negative).has_value());

    std::string fractional = good;
    fractional.replace(fractional.find("\"term\":7"), 9, "\"term\":7.5");
    CHK(!parseSnapshot(fractional).has_value());

    std::string wrongType = good;
    wrongType.replace(wrongType.find("\"term\":7"), 9, "\"term\":\"7\"");
    CHK(!parseSnapshot(wrongType).has_value());
}

// Verifies fields a future version adds don't break an older reader.
static void extra_fields_tolerated() {
    std::string json = toJson(sample());
    json.insert(json.size() - 1, ",\"newField\":[1,2,3],\"other\":{\"a\":true}");

    const auto parsed = parseSnapshot(json);
    CHK(parsed.has_value() && *parsed == sample());
}

static void run_tests() {
    RUN(round_trips_with_leader);
    RUN(round_trips_without_leader);
    RUN(awkward_ids_round_trip);
    RUN(role_names);
    RUN(equality_sees_every_field);
    RUN(malformed_input_rejected);
    RUN(extra_fields_tolerated);
}

REGISTER_TEST_SUITE();
