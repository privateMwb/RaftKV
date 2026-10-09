// Integration Cluster Replication Test Suite
// Verifies writes made on the leader reach every node's map over real HTTP.
//
// Covers:
// - A write on the leader appears on all three nodes
// - Many writes arrive complete and in order
// - Followers refuse writes with NotLeader (and name the leader)
// - Deletes replicate
// - Binary keys and values replicate byte for byte
// - Every node ends with the same log length

#include <support/framework.h>
#include <support/testCluster.h>

using namespace RaftKV::Node;
using namespace std::chrono_literals;

// Verifies one write is visible everywhere.
static void write_reaches_all_nodes() {
    TestCluster cluster(3, 18830);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    CHK(cluster.putViaLeader("city", "lagos") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("city", std::optional<std::string>("lagos")));
}

// Verifies 40 sequential writes all land, with the last overwrite winning.
static void many_writes_in_order() {
    TestCluster cluster(3, 18835);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    for (int i = 0; i < 40; ++i)
        CHK(cluster.putViaLeader("key-" + std::to_string(i), "value-" + std::to_string(i)) ==
            SubmitResult::Ok);
    for (int i = 0; i < 10; ++i)
        CHK(cluster.putViaLeader("counter", std::to_string(i)) == SubmitResult::Ok);

    CHK(cluster.waitForValueEverywhere("counter", std::optional<std::string>("9")));
    CHK(cluster.waitForValueEverywhere("key-39", std::optional<std::string>("value-39")));

    for (int i = 0; i < 3; ++i)
        CHK(cluster.node(i).size() == 41);
}

// Verifies a follower says NotLeader and knows who the leader is.
static void follower_rejects_writes() {
    TestCluster cluster(3, 18840);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    CHK(cluster.putViaLeader("seed", "1") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("seed", std::optional<std::string>("1")));

    const int follower = (leader + 1) % 3;
    CHK(cluster.node(follower).put("x", "y", 500ms) == SubmitResult::NotLeader);
    CHK(cluster.node(follower).del("seed", 500ms) == SubmitResult::NotLeader);
    CHK(!cluster.node(follower).get("x").has_value());

    // The rejection is actionable: the follower can point at the leader.
    CHK(cluster.node(follower).leaderId() ==
        std::optional<std::string>(cluster.node(leader).nodeId()));
}

// Verifies a delete removes the key on every node.
static void delete_replicates() {
    TestCluster cluster(3, 18845);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    CHK(cluster.putViaLeader("temp", "1") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("temp", std::optional<std::string>("1")));

    CHK(cluster.delViaLeader("temp") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("temp", std::nullopt));
}

// Verifies keys and values with NULs and high bytes survive the whole path.
static void binary_data_replicates() {
    TestCluster cluster(3, 18850);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    const std::string key("k\0ey\n", 5);
    const std::string value("\xff\x00\x80\r\n\x01", 6);
    CHK(cluster.putViaLeader(key, value) == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere(key, std::optional<std::string>(value)));
}

// Verifies all nodes converge on an identical log length.
static void logs_converge() {
    TestCluster cluster(3, 18855);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    for (int i = 0; i < 15; ++i)
        CHK(cluster.putViaLeader("k" + std::to_string(i), "v") == SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("k14", std::optional<std::string>("v")));

    bool converged = false;
    for (int attempt = 0; attempt < 300 && !converged; ++attempt) {
        converged = cluster.node(0).lastLogIndex() == cluster.node(1).lastLogIndex() &&
                    cluster.node(1).lastLogIndex() == cluster.node(2).lastLogIndex();
        if (!converged)
            std::this_thread::sleep_for(10ms);
    }
    CHK(converged);
}

static void run_tests() {
    RUN(write_reaches_all_nodes);
    RUN(many_writes_in_order);
    RUN(follower_rejects_writes);
    RUN(delete_replicates);
    RUN(binary_data_replicates);
    RUN(logs_converge);
}

REGISTER_TEST_SUITE();
