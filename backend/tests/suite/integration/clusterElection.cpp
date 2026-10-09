// Integration Cluster Election Test Suite
// Verifies a real 3-node cluster (loopback HTTP, real WAL and term files)
// elects a leader and that the peer endpoints behave on the wire.
//
// Covers:
// - Exactly one leader emerges; no two nodes lead the same term
// - Every follower learns who the leader is
// - GET /status reports node state as JSON
// - Garbage peer-RPC bodies are 400, wrong methods are rejected
// - A cluster of one with a port behaves like Phase 1 (no peers needed)

#include <support/framework.h>
#include <support/testCluster.h>

#include <FalconHTTP/Client/HttpClient.h>

using namespace std::chrono_literals;

// Verifies one leader, one term of leadership, and everyone agreeing on who.
static void three_nodes_elect_leader() {
    TestCluster cluster(3, 18810);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    // No other node may be a leader in the same term.
    const RaftCore::Term leaderTerm = cluster.node(leader).term();
    int sameTermLeaders = 0;
    for (int i = 0; i < cluster.size(); ++i)
        if (cluster.node(i).role() == RaftCore::Role::Leader && cluster.node(i).term() == leaderTerm)
            ++sameTermLeaders;
    CHK(sameTermLeaders == 1);

    // Every node eventually names the same leader.
    const std::string expected = cluster.node(leader).nodeId();
    bool agreed = false;
    for (int attempt = 0; attempt < 300 && !agreed; ++attempt) {
        agreed = true;
        for (int i = 0; i < cluster.size(); ++i)
            if (cluster.node(i).leaderId() != std::optional<std::string>(expected))
                agreed = false;
        if (!agreed)
            std::this_thread::sleep_for(10ms);
    }
    CHK(agreed);
}

// Verifies GET /status returns this node's state as JSON.
static void status_endpoint_reports_state() {
    TestCluster cluster(3, 18815);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    FalconHTTP::Client::HttpClient client;
    auto response = client.get("127.0.0.1", cluster.portOf(1), "/status");

    CHK(response.has_value());
    if (!response)
        return;

    CHK(response->statusCode == 200);
    CHK(response->headers["content-type"] == "application/json");
    CHK(response->body.find("\"id\":\"node-2\"") != std::string::npos);
    CHK(response->body.find("\"role\":") != std::string::npos);
    CHK(response->body.find("\"term\":") != std::string::npos);
}

// Verifies malformed or wrongly-routed peer RPCs are refused, not crashed on.
static void bad_rpc_requests_rejected() {
    TestCluster cluster(3, 18820);
    CHK(cluster.startAll());

    FalconHTTP::Client::HttpClient client;

    const auto garbage =
        client.post("127.0.0.1", cluster.portOf(0), "/raft/request-vote", "definitely not binary");
    CHK(garbage.has_value() && garbage->statusCode == 400);

    const auto truncated =
        client.post("127.0.0.1", cluster.portOf(0), "/raft/append-entries", std::string("\x01\x05", 2));
    CHK(truncated.has_value() && truncated->statusCode == 400);

    const auto wrongMethod = client.get("127.0.0.1", cluster.portOf(0), "/raft/append-entries");
    CHK(wrongMethod.has_value() && wrongMethod->statusCode != 200);

    // The node is still healthy afterwards.
    CHK(cluster.waitForLeader() >= 0);
}

// Verifies a one-node "cluster" that also serves HTTP still elects itself.
static void single_node_with_server() {
    TestCluster cluster(1, 18825);
    CHK(cluster.startAll());

    CHK(cluster.waitForLeader() == 0);
    CHK(cluster.putViaLeader("k", "v") == RaftKV::Node::SubmitResult::Ok);
    CHK(cluster.node(0).get("k") == std::optional<std::string>("v"));
}

static void run_tests() {
    RUN(three_nodes_elect_leader);
    RUN(status_endpoint_reports_state);
    RUN(bad_rpc_requests_rejected);
    RUN(single_node_with_server);
}

REGISTER_TEST_SUITE();
