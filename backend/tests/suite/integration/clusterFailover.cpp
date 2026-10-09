// Integration Cluster Failover Test Suite
// Verifies the cluster survives and recovers from node failures: the headline
// property of the whole project.
//
// Covers:
// - Killing the leader elects a different leader and writes continue
// - A killed follower catches up on everything it missed after restarting
// - A killed leader rejoins as a follower and sees newer data
// - Without a majority, writes are never acknowledged
// - Losing quorum and regaining it restores service
// - A full cluster restart loses nothing

#include <support/framework.h>
#include <support/testCluster.h>

using namespace RaftKV::Node;
using namespace std::chrono_literals;

// Verifies leader death -> new leader -> writes keep working, data intact.
static void leader_failure_reelects() {
    TestCluster cluster(3, 18860);
    CHK(cluster.startAll());

    const int oldLeader = cluster.waitForLeader();
    CHK(oldLeader >= 0);
    if (oldLeader < 0)
        return;

    CHK(cluster.putViaLeader("before", "1") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("before", std::optional<std::string>("1")));

    cluster.kill(oldLeader);

    const int newLeader = cluster.waitForLeader(8000ms, oldLeader);
    CHK(newLeader >= 0);
    CHK(newLeader != oldLeader);

    CHK(cluster.putViaLeader("after", "2") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("after", std::optional<std::string>("2")));
    CHK(cluster.waitForValueEverywhere("before", std::optional<std::string>("1")));
}

// Verifies a follower that was down catches up on missed writes.
static void restarted_follower_catches_up() {
    TestCluster cluster(3, 18865);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    CHK(cluster.putViaLeader("early", "e") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("early", std::optional<std::string>("e")));

    const int victim = (leader + 1) % 3;
    cluster.kill(victim);

    for (int i = 0; i < 8; ++i)
        CHK(cluster.putViaLeader("missed-" + std::to_string(i), "m") == SubmitResult::Ok);

    CHK(cluster.start(victim));

    CHK(cluster.waitForValue(victim, "missed-7", std::optional<std::string>("m"), 8000ms));
    CHK(cluster.waitForValue(victim, "early", std::optional<std::string>("e")));
    CHK(cluster.node(victim).size() == 9);
}

// Verifies a deposed leader comes back as a follower with the new data.
static void old_leader_rejoins() {
    TestCluster cluster(3, 18870);
    CHK(cluster.startAll());

    const int oldLeader = cluster.waitForLeader();
    CHK(oldLeader >= 0);
    if (oldLeader < 0)
        return;

    CHK(cluster.putViaLeader("one", "1") == SubmitResult::Ok);
    const RaftCore::Term termBeforeCrash = cluster.node(oldLeader).term();
    cluster.kill(oldLeader);

    CHK(cluster.waitForLeader(8000ms, oldLeader) >= 0);
    CHK(cluster.putViaLeader("two", "2") == SubmitResult::Ok);

    CHK(cluster.start(oldLeader));

    // A restarted node always comes back as a Follower, never as Leader.
    CHK(cluster.node(oldLeader).role() == RaftCore::Role::Follower);

    CHK(cluster.waitForValue(oldLeader, "two", std::optional<std::string>("2"), 8000ms));
    CHK(cluster.waitForValue(oldLeader, "one", std::optional<std::string>("1")));

    // It adopted the newer leader's term (its own persisted term never went back).
    CHK(cluster.node(oldLeader).term() > termBeforeCrash);
    CHK(cluster.node(oldLeader).leaderId().has_value());
    CHK(cluster.putViaLeader("three", "3") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("three", std::optional<std::string>("3")));
}

// Verifies no write is acknowledged while a majority is down.
static void no_quorum_no_commit() {
    TestCluster cluster(3, 18875);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    CHK(cluster.putViaLeader("safe", "1") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("safe", std::optional<std::string>("1")));

    // Kill both other nodes, leaving the (former) leader alone.
    for (int i = 0; i < 3; ++i)
        if (i != leader)
            cluster.kill(i);

    const SubmitResult result = cluster.node(leader).put("doomed", "x", 400ms);
    CHK(result != SubmitResult::Ok);
    CHK(!cluster.node(leader).get("doomed").has_value()); // never applied.
}

// Verifies the cluster serves again once a majority is restored, and the
// write that couldn't commit never resurfaces as acknowledged data loss.
static void quorum_loss_then_recovery() {
    TestCluster cluster(3, 18880);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    CHK(cluster.putViaLeader("kept", "1") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("kept", std::optional<std::string>("1")));

    const int a = (leader + 1) % 3;
    const int b = (leader + 2) % 3;
    cluster.kill(a);
    cluster.kill(b);

    CHK(cluster.node(leader).put("limbo", "x", 300ms) != SubmitResult::Ok);

    CHK(cluster.start(a)); // majority restored: leader + a.

    CHK(cluster.putViaLeader("resumed", "2") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("resumed", std::optional<std::string>("2")));
    CHK(cluster.waitForValueEverywhere("kept", std::optional<std::string>("1")));
}

// Verifies stopping and restarting every node loses no acknowledged write.
static void full_restart_loses_nothing() {
    TestCluster cluster(3, 18885);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    for (int i = 0; i < 10; ++i)
        CHK(cluster.putViaLeader("p" + std::to_string(i), std::to_string(i)) == SubmitResult::Ok);
    CHK(cluster.putViaLeader("p3", "changed") == SubmitResult::Ok);
    CHK(cluster.delViaLeader("p5") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("p9", std::optional<std::string>("9")));

    for (int i = 0; i < 3; ++i)
        cluster.kill(i);
    for (int i = 0; i < 3; ++i)
        CHK(cluster.start(i));

    CHK(cluster.waitForLeader(8000ms) >= 0);
    CHK(cluster.waitForValueEverywhere("p9", std::optional<std::string>("9"), 8000ms));
    CHK(cluster.waitForValueEverywhere("p3", std::optional<std::string>("changed"), 8000ms));
    CHK(cluster.waitForValueEverywhere("p5", std::nullopt, 8000ms));
    CHK(cluster.waitForValueEverywhere("p0", std::optional<std::string>("0"), 8000ms));
}

static void run_tests() {
    RUN(leader_failure_reelects);
    RUN(restarted_follower_catches_up);
    RUN(old_leader_rejoins);
    RUN(no_quorum_no_commit);
    RUN(quorum_loss_then_recovery);
    RUN(full_restart_loses_nothing);
}

REGISTER_TEST_SUITE();
