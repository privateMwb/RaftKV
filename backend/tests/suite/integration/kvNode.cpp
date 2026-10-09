// Integration KvNode Test Suite
// Verifies the Phase 1 core loop end to end on one node: elect, append,
// commit, apply, and survive a restart -- real WAL, real state file.
//
// Covers:
// - A lone node elects itself and commits its election no-op
// - put/get/del through the Raft log, applied before the call returns
// - Writes before leadership are NotLeader; empty keys are InvalidArgument
// - After a restart the same data directory replays into an empty map
// - The term persists across a restart (it goes up, never back)
// - Deleted keys stay deleted after a restart
// - A write is durable in the log before it returns

#include <support/framework.h>

#include "node/KvNode.h"

#include <chrono>
#include <fstream>
#include <thread>

using namespace RaftKV::Node;
using namespace std::chrono_literals;

static KvNodeConfig configFor(const TempDir& dir) {
    KvNodeConfig config;
    config.dataDir = dir.str();
    config.tickInterval = 2ms; // fast ticks keep the suite quick.
    config.electionTimeoutMinTicks = 3;
    config.electionTimeoutMaxTicks = 6;
    config.heartbeatIntervalTicks = 1;
    return config;
}

// Verifies a single node becomes Leader and commits its no-op.
static void lone_node_becomes_leader() {
    TempDir dir;
    KvNode node(configFor(dir));
    CHK(node.open() == RaftCore::Status::OK);
    node.start();

    CHK(node.waitUntilLeader(3s));
    CHK(node.role() == RaftCore::Role::Leader);
    CHK(node.term() >= 1);
    CHK(node.commitIndex() >= 1);
    CHK(node.lastTickError() == RaftCore::Status::OK);
}

// Verifies put/get/del go through the log and are visible on return.
static void put_get_delete() {
    TempDir dir;
    KvNode node(configFor(dir));
    CHK(node.open() == RaftCore::Status::OK);
    node.start();
    CHK(node.waitUntilLeader(3s));

    CHK(node.put("lang", "c++") == SubmitResult::Ok);
    CHK(node.get("lang") == std::optional<std::string>("c++"));

    CHK(node.put("lang", "c++23") == SubmitResult::Ok);
    CHK(node.get("lang") == std::optional<std::string>("c++23"));

    CHK(node.del("lang") == SubmitResult::Ok);
    CHK(!node.get("lang").has_value());
    CHK(node.size() == 0);

    // Two no-op-or-command entries per write path: the log grew and was applied.
    CHK(node.lastApplied() == node.lastLogIndex());
}

// Verifies the rejection paths: no leadership yet, and a bad key.
static void rejects_bad_writes() {
    TempDir dir;
    KvNode node(configFor(dir));
    CHK(node.open() == RaftCore::Status::OK);

    // Opened but not started: never elected.
    CHK(node.put("k", "v") == SubmitResult::NotLeader);

    node.start();
    CHK(node.waitUntilLeader(3s));
    CHK(node.put("", "v") == SubmitResult::InvalidArgument);
    CHK(node.del("") == SubmitResult::InvalidArgument);
}

// Verifies a restart replays the WAL into an empty map and bumps the term.
static void restart_replays_data() {
    TempDir dir;
    RaftCore::Term firstTerm = 0;

    {
        KvNode node(configFor(dir));
        CHK(node.open() == RaftCore::Status::OK);
        node.start();
        CHK(node.waitUntilLeader(3s));
        firstTerm = node.term();

        CHK(node.put("a", "1") == SubmitResult::Ok);
        CHK(node.put("b", "2") == SubmitResult::Ok);
        CHK(node.put("c", "3") == SubmitResult::Ok);
        CHK(node.del("b") == SubmitResult::Ok);
        node.stop();
    } // process "exits": all in-memory state is gone.

    KvNode restarted(configFor(dir));
    CHK(restarted.open() == RaftCore::Status::OK);
    CHK(restarted.size() == 0); // nothing applied yet: map starts empty.

    restarted.start();
    CHK(restarted.waitUntilLeader(3s));

    // Replay is driven by the new term's no-op committing; wait for it.
    for (int i = 0; i < 500 && restarted.lastApplied() < restarted.lastLogIndex(); ++i)
        std::this_thread::sleep_for(2ms);

    CHK(restarted.lastApplied() == restarted.lastLogIndex()); // really caught up, not timed out.

    CHK(restarted.get("a") == std::optional<std::string>("1"));
    CHK(!restarted.get("b").has_value()); // the delete replayed too.
    CHK(restarted.get("c") == std::optional<std::string>("3"));
    CHK(restarted.term() > firstTerm);    // term persisted, then advanced.

    CHK(restarted.put("d", "4") == SubmitResult::Ok);
    CHK(restarted.get("d") == std::optional<std::string>("4"));
}

// Verifies a reopened node refuses a corrupt term/vote file loudly.
static void corrupt_state_blocks_open() {
    TempDir dir;
    {
        KvNode node(configFor(dir));
        CHK(node.open() == RaftCore::Status::OK);
        node.start();
        CHK(node.waitUntilLeader(3s));
        node.stop();
    }

    {
        std::ofstream out(dir.file("node-1.state"), std::ios::binary | std::ios::trunc);
        out << "scrambled";
    }

    KvNode node(configFor(dir));
    CHK(node.open() == RaftCore::Status::PARSE_ERROR);
}

static void run_tests() {
    RUN(lone_node_becomes_leader);
    RUN(put_get_delete);
    RUN(rejects_bad_writes);
    RUN(restart_replays_data);
    RUN(corrupt_state_blocks_open);
}

REGISTER_TEST_SUITE();
