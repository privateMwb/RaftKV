// Integration Client API Test Suite
// Verifies the /kv HTTP API against real 3-node clusters: success paths,
// redirects, validation, rate limiting, and the read-consistency guarantees.
//
// Covers:
// - PUT / GET / DELETE round trip on the leader (204 / 200 / 404, idempotent delete)
// - Followers answer 307 with a Location pointing at the leader, and following
//   it works for PUT, DELETE, and GET
// - ?stale=1 reads a follower's local map without redirecting
// - Binary keys and values survive percent-encoding end to end
// - Bad or empty keys (400), oversized values (413), wrong methods (405), no route (404)
// - A maximum-size value replicates without causing an election
// - No known leader is 503 with Retry-After
// - Read-your-writes across different nodes (linearizable GET)
// - Without a majority a linearizable GET is refused (503) while ?stale works
// - Reads and writes keep working after a leader change
// - Rate limiting hits only /kv/* (429), never /status or peer RPC

#include <support/framework.h>
#include <support/apiClient.h>
#include <support/testCluster.h>

#include "net/PercentCoding.h"

using FalconHTTP::HTTP::HttpMethod;
using RaftKV::Net::percentEncode;
using namespace std::chrono_literals;

static std::string kvPath(const std::string& key) {
    return "/kv/" + percentEncode(key);
}

// Verifies the basic verbs on the leader, including idempotent delete.
static void leader_put_get_delete() {
    TestCluster cluster(3, 18910);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;
    const uint16_t port = cluster.portOf(leader);

    auto put = api::call(HttpMethod::Put, port, kvPath("color"), "blue");
    CHK(put.has_value() && put->statusCode == 204);

    auto get = api::call(HttpMethod::Get, port, kvPath("color"));
    CHK(get.has_value() && get->statusCode == 200);
    if (get) {
        CHK(get->body == "blue");
        CHK(get->headers["x-raftkv-node"] == cluster.node(leader).nodeId());
    }

    auto del = api::call(HttpMethod::Delete, port, kvPath("color"));
    CHK(del.has_value() && del->statusCode == 204);

    auto gone = api::call(HttpMethod::Get, port, kvPath("color"));
    CHK(gone.has_value() && gone->statusCode == 404);

    auto again = api::call(HttpMethod::Delete, port, kvPath("color"));
    CHK(again.has_value() && again->statusCode == 204);
}

// Verifies followers redirect with 307 and that the redirect is followable.
static void follower_redirects_to_leader() {
    TestCluster cluster(3, 18915);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    // Make sure the follower has learned who leads before we ask it.
    const int follower = (leader + 1) % 3;
    for (int i = 0; i < 300 && !cluster.node(follower).leaderId(); ++i)
        std::this_thread::sleep_for(10ms);

    auto redirect = api::call(HttpMethod::Put, cluster.portOf(follower), kvPath("a"), "1");
    CHK(redirect.has_value() && redirect->statusCode == 307);
    if (redirect) {
        const std::string expected =
            "http://127.0.0.1:" + std::to_string(cluster.portOf(leader)) + "/kv/a";
        CHK(redirect->headers["location"] == expected);
    }

    // Following the redirect (same method and body) performs the write.
    auto put = api::callFollowing(HttpMethod::Put, cluster.portOf(follower), kvPath("a"), "1");
    CHK(put.has_value() && put->statusCode == 204);

    auto getRedirect = api::call(HttpMethod::Get, cluster.portOf(follower), kvPath("a"));
    CHK(getRedirect.has_value() && getRedirect->statusCode == 307);

    auto get = api::callFollowing(HttpMethod::Get, cluster.portOf(follower), kvPath("a"));
    CHK(get.has_value() && get->statusCode == 200 && get->body == "1");

    auto del = api::callFollowing(HttpMethod::Delete, cluster.portOf(follower), kvPath("a"));
    CHK(del.has_value() && del->statusCode == 204);
}

// Verifies ?stale=1 answers from the follower's own map, no redirect.
static void stale_read_stays_local() {
    TestCluster cluster(3, 18920);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;
    const int follower = (leader + 1) % 3;

    CHK(cluster.putViaLeader("seen", "yes") == RaftKV::Node::SubmitResult::Ok);
    CHK(cluster.waitForValue(follower, "seen", std::optional<std::string>("yes")));

    auto hit = api::call(HttpMethod::Get, cluster.portOf(follower), kvPath("seen") + "?stale=1");
    CHK(hit.has_value() && hit->statusCode == 200);
    if (hit) {
        CHK(hit->body == "yes");
        CHK(hit->headers["x-raftkv-node"] == cluster.node(follower).nodeId());
        CHK(hit->headers["x-raftkv-stale"] == "true");
    }

    auto miss = api::call(HttpMethod::Get, cluster.portOf(follower), kvPath("absent") + "?stale=1");
    CHK(miss.has_value() && miss->statusCode == 404);
}

// Verifies awkward keys and binary values round-trip through HTTP.
static void binary_keys_and_values() {
    TestCluster cluster(3, 18925);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    const std::string key("user/42 name\0%+", 15);
    const std::string value("\xff\x00\x80\r\n", 5);

    // Right after startup a follower may not have heard from the new leader yet
    // and answers 503 + Retry-After; a real client retries, so does this.
    auto put = api::callPatiently(HttpMethod::Put, cluster.portOf(0), kvPath(key), value);
    CHK(put.has_value() && put->statusCode == 204);

    auto get = api::callPatiently(HttpMethod::Get, cluster.portOf(2), kvPath(key));
    CHK(get.has_value() && get->statusCode == 200);
    if (get)
        CHK(get->body == value);

    // The server really stored the decoded key, not the escaped text.
    CHK(cluster.waitForValueEverywhere(key, std::optional<std::string>(value)));
}

// Verifies validation failures and wrong methods.
static void invalid_requests_rejected() {
    TestCluster cluster(3, 18930);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;
    const uint16_t port = cluster.portOf(leader);

    auto badEscape = api::call(HttpMethod::Put, port, "/kv/%zz", "x");
    CHK(badEscape.has_value() && badEscape->statusCode == 400);

    auto truncatedEscape = api::call(HttpMethod::Get, port, "/kv/abc%4");
    CHK(truncatedEscape.has_value() && truncatedEscape->statusCode == 400);

    auto longKey = api::call(HttpMethod::Put, port, "/kv/" + std::string(1025, 'k'), "x");
    CHK(longKey.has_value() && longKey->statusCode == 400);

    const std::size_t cap = RaftKV::Net::ClientApiConfig().maxValueBytes;

    auto tooBig = api::call(HttpMethod::Put, port, kvPath("big"), std::string(cap + 1, 'v'));
    CHK(tooBig.has_value() && tooBig->statusCode == 413);

    auto largest = api::call(HttpMethod::Put, port, kvPath("big"), std::string(cap, 'v'));
    CHK(largest.has_value() && largest->statusCode == 204);

    auto wrongMethod = api::call(HttpMethod::Post, port, kvPath("x"), "x");
    CHK(wrongMethod.has_value() && wrongMethod->statusCode == 405);

    // "/kv/" matches the route with an empty key segment: refused as an invalid
    // key. Bare "/kv" matches no route at all.
    auto emptyKey = api::call(HttpMethod::Put, port, "/kv/", "x");
    CHK(emptyKey.has_value() && emptyKey->statusCode == 400);

    auto noRoute = api::call(HttpMethod::Get, port, "/kv");
    CHK(noRoute.has_value() && noRoute->statusCode == 404);
}

// Verifies a node that knows no leader says 503 + Retry-After, not a bad redirect.
static void no_leader_is_503() {
    TestCluster cluster(3, 18935);
    CHK(cluster.start(0)); // The other two never start, so nobody can win.

    auto put = api::call(HttpMethod::Put, cluster.portOf(0), kvPath("k"), "v");
    CHK(put.has_value() && put->statusCode == 503);
    if (put)
        CHK(put->headers["retry-after"] == "1");

    auto get = api::call(HttpMethod::Get, cluster.portOf(0), kvPath("k"));
    CHK(get.has_value() && get->statusCode == 503);

    // Local reads don't need a leader.
    auto stale = api::call(HttpMethod::Get, cluster.portOf(0), kvPath("k") + "?stale=1");
    CHK(stale.has_value() && stale->statusCode == 404);
}

// Verifies a write acknowledged through one node is read back through another.
// Transient unavailability (503, or no answer during an election) is retried:
// the property under test is that a successful read never contradicts an
// acknowledged write, not that an election can never happen mid-test.
static void read_your_writes_across_nodes() {
    TestCluster cluster(3, 18940);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    // Retries while the cluster is briefly unavailable; returns the final answer.
    auto settled = [](HttpMethod method, uint16_t port, const std::string& path, const std::string& body) {
        std::optional<FalconHTTP::Client::ClientResponse> response;
        for (int attempt = 0; attempt < 60; ++attempt) {
            response = api::callFollowing(method, port, path, body);
            if (response && response->statusCode != 503)
                return response;
            std::this_thread::sleep_for(50ms);
        }
        return response;
    };

    int violations = 0;
    int unavailable = 0;
    for (int i = 0; i < 30; ++i) {
        const std::string value = "v" + std::to_string(i);
        const int writer = i % 3;
        const int reader = (i + 1) % 3;

        auto put = settled(HttpMethod::Put, cluster.portOf(writer), kvPath("rw"), value);
        if (!put || put->statusCode != 204) {
            ++unavailable;
            continue;
        }

        auto get = settled(HttpMethod::Get, cluster.portOf(reader), kvPath("rw"), "");
        if (!get || get->statusCode == 503) {
            ++unavailable;
        } else if (get->statusCode != 200 || get->body != value) {
            ++violations; // An acknowledged write must be visible to a linearizable read.
        }
    }

    CHK(violations == 0);
    CHK(unavailable <= 2); // Elections may cost a round or two, never most of them.
}

// Verifies a maximum-size write neither fails nor triggers an election: the
// leader and term must be the same afterwards (a slow fsync must not look like
// a dead leader).
static void max_size_write_keeps_leader() {
    TestCluster cluster(3, 18960);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;
    const RaftCore::Term term = cluster.node(leader).term();

    const std::size_t cap = RaftKV::Net::ClientApiConfig().maxValueBytes;
    for (int i = 0; i < 3; ++i) {
        auto put = api::call(HttpMethod::Put, cluster.portOf(leader), kvPath("max"),
                             std::string(cap, static_cast<char>('a' + i)));
        CHK(put.has_value() && put->statusCode == 204);
    }

    CHK(cluster.waitForValueEverywhere("max", std::optional<std::string>(std::string(cap, 'c'))));
    CHK(cluster.leader() == leader);
    CHK(cluster.node(leader).term() == term);
}

// Verifies a leader that can't reach a majority refuses linearizable reads.
static void read_needs_quorum() {
    RaftKV::Net::ClientApiConfig api = TestCluster::defaultApi();
    api.readTimeout = 400ms;
    TestCluster cluster(3, 18945, api);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    CHK(cluster.putViaLeader("kept", "1") == RaftKV::Node::SubmitResult::Ok);
    CHK(cluster.waitForValueEverywhere("kept", std::optional<std::string>("1")));

    for (int i = 0; i < 3; ++i)
        if (i != leader)
            cluster.kill(i);

    // The safety property: a lone survivor must never serve a linearizable
    // read. If it is still Leader it cannot prove it any more (503); if an
    // election slipped in before the kills it is a follower and redirects (307).
    auto plain = ::api::call(HttpMethod::Get, cluster.portOf(leader), kvPath("kept"));
    CHK(plain.has_value() && plain->statusCode != 200);
    if (cluster.node(leader).role() == RaftCore::Role::Leader)
        CHK(plain.has_value() && plain->statusCode == 503);

    auto stale = ::api::call(HttpMethod::Get, cluster.portOf(leader), kvPath("kept") + "?stale=1");
    CHK(stale.has_value() && stale->statusCode == 200 && stale->body == "1");
}

// Verifies reads and writes keep working, with data intact, after failover.
static void api_survives_leader_change() {
    TestCluster cluster(3, 18950);
    CHK(cluster.startAll());

    const int oldLeader = cluster.waitForLeader();
    CHK(oldLeader >= 0);
    if (oldLeader < 0)
        return;

    auto before = api::callFollowing(HttpMethod::Put, cluster.portOf(oldLeader), kvPath("durable"), "yes");
    CHK(before.has_value() && before->statusCode == 204);

    cluster.kill(oldLeader);
    const int newLeader = cluster.waitForLeader(8000ms, oldLeader);
    CHK(newLeader >= 0);
    if (newLeader < 0)
        return;

    // The new leader serves the old write, and accepts new ones.
    auto read = api::call(HttpMethod::Get, cluster.portOf(newLeader), kvPath("durable"));
    for (int i = 0; i < 100 && read && read->statusCode == 503; ++i) { // own-term commit pending
        std::this_thread::sleep_for(20ms);
        read = api::call(HttpMethod::Get, cluster.portOf(newLeader), kvPath("durable"));
    }
    CHK(read.has_value() && read->statusCode == 200 && read->body == "yes");

    auto after = api::call(HttpMethod::Put, cluster.portOf(newLeader), kvPath("later"), "2");
    CHK(after.has_value() && after->statusCode == 204);
}

// Verifies the limiter 429s /kv/* but leaves /status and peer RPC alone.
static void rate_limit_covers_kv_only() {
    RaftKV::Net::ClientApiConfig limited = TestCluster::defaultApi();
    limited.rateLimitRequests = 5;
    limited.rateLimitWindow = 60s;
    TestCluster cluster(3, 18955, limited);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    int allowed = 0;
    int limitedCount = 0;
    std::string retryAfter;
    for (int i = 0; i < 12; ++i) {
        auto response = api::call(HttpMethod::Get, cluster.portOf(0), kvPath("x") + "?stale=1");
        if (!response)
            continue;
        if (response->statusCode == 429) {
            ++limitedCount;
            retryAfter = response->headers["retry-after"];
        } else {
            ++allowed;
        }
    }

    CHK(allowed == 5);
    CHK(limitedCount == 7);
    CHK(retryAfter == "1");

    // Not covered: /status and the peer RPC paths.
    for (int i = 0; i < 25; ++i) {
        auto status = api::call(HttpMethod::Get, cluster.portOf(0), "/status");
        CHK(status.has_value() && status->statusCode == 200);

        auto rpc = api::call(HttpMethod::Post, cluster.portOf(0), "/raft/request-vote", "garbage");
        CHK(rpc.has_value() && rpc->statusCode == 400); // reached the handler; not 429.
    }

    // The cluster itself is unaffected: it still elects and replicates.
    CHK(cluster.waitForLeader() >= 0);
}

static void run_tests() {
    RUN(leader_put_get_delete);
    RUN(follower_redirects_to_leader);
    RUN(stale_read_stays_local);
    RUN(binary_keys_and_values);
    RUN(invalid_requests_rejected);
    RUN(no_leader_is_503);
    RUN(read_your_writes_across_nodes);
    RUN(max_size_write_keeps_leader);
    RUN(read_needs_quorum);
    RUN(api_survives_leader_change);
    RUN(rate_limit_covers_kv_only);
}

REGISTER_TEST_SUITE();
