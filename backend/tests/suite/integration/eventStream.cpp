// Integration Event Stream Test Suite
// Verifies the live observability endpoints against real 3-node clusters:
// the per-node SSE stream, the aggregated cluster stream, and /cluster.
//
// Covers:
// - /events sends the current state first, then pushes real changes only
// - An idle stream sends ping events (keep-alive)
// - /cluster/events reports every member up with a leader and quorum
// - GET /cluster returns the same view as JSON
// - Killing a node shows it DOWN (quorum kept), a second kill drops quorum,
//   and restarting brings the member back up
// - A leader change shows up: new leader, higher term
// - The stream cap: extra streams get 503, and a closed stream frees its slot
// - CORS headers on plain responses, on a stream's preamble, and on preflight
// - Stopping a node with streams open neither hangs nor leaks (clients see EOF)

#include <support/framework.h>
#include <support/apiClient.h>
#include <support/sseReader.h>
#include <support/testCluster.h>

#include <JsonPro/Json.h>

#include <memory>
#include <vector>

using FalconHTTP::HTTP::HttpMethod;
using RaftKV::Node::parseSnapshot;
using namespace std::chrono_literals;

namespace {

// The bits of a /cluster JSON document the tests care about.
struct View {
    std::optional<std::string> leader;
    RaftCore::Term term = 0;
    bool quorum = false;
    struct Member {
        std::string id;
        bool up = false;
        std::string role; // "" when down
    };
    std::vector<Member> members;

    [[nodiscard]] const Member* find(const std::string& id) const {
        for (const Member& member : members)
            if (member.id == id)
                return &member;
        return nullptr;
    }

    [[nodiscard]] bool allUp() const {
        if (members.empty())
            return false;
        for (const Member& member : members)
            if (!member.up)
                return false;
        return true;
    }
};

std::optional<View> parseView(const std::string& json) {
    try {
        const JsonPro::Json root = JsonPro::Json::parse(json);
        View view;
        if (root.at("leader").isString())
            view.leader = root.at("leader").asString();
        view.term = static_cast<RaftCore::Term>(root.at("term").asNumber());
        view.quorum = root.at("quorum").asBool();

        const auto& members = root.at("members");
        for (std::size_t i = 0; i < members.size(); ++i) {
            View::Member member;
            member.id = members.at(i).at("id").asString();
            member.up = members.at(i).at("up").asBool();
            if (members.at(i).at("state").isObject())
                member.role = members.at(i).at("state").at("role").asString();
            view.members.push_back(std::move(member));
        }
        return view;
    } catch (const JsonPro::JsonException&) {
        return std::nullopt;
    }
}

// Reads events from a /cluster/events stream until one satisfies @p wanted.
template <typename Predicate>
bool waitForView(SseReader& reader, Predicate wanted, std::chrono::milliseconds timeout,
                 View* out = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto event = reader.next(left);
        if (!event)
            return false;
        if (event->name != "cluster")
            continue;

        const auto view = parseView(event->data);
        if (view && wanted(*view)) {
            if (out)
                *out = *view;
            return true;
        }
    }
    return false;
}

} // namespace

// Verifies a new /events stream starts with the current state of that node.
static void node_stream_sends_state_first() {
    TestCluster cluster(3, 19010);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    SseReader reader(cluster.portOf(0), "/events");
    CHK(reader.connected());
    CHK(reader.status() == 200);
    CHK(reader.header("content-type") == "text/event-stream");

    const auto first = reader.next(2000ms);
    CHK(first.has_value());
    if (!first)
        return;

    CHK(first->name == "state");
    const auto snapshot = parseSnapshot(first->data);
    CHK(snapshot.has_value());
    if (snapshot) {
        CHK(snapshot->id == "node-1");
        CHK(snapshot->term >= 1);
    }
}

// Verifies a write on the leader is pushed as state events: log, commit, key count.
static void node_stream_pushes_changes() {
    TestCluster cluster(3, 19015);
    CHK(cluster.startAll());

    const int leader = cluster.waitForLeader();
    CHK(leader >= 0);
    if (leader < 0)
        return;

    SseReader reader(cluster.portOf(leader), "/events");
    CHK(reader.connected());

    const auto initial = reader.next(2000ms);
    CHK(initial.has_value());
    const auto start = initial ? parseSnapshot(initial->data) : std::nullopt;
    CHK(start.has_value());
    if (!start)
        return;

    CHK(cluster.putViaLeader("pushed", "v") == RaftKV::Node::SubmitResult::Ok);

    // Events keep arriving until the snapshot shows the write committed and applied.
    bool sawWrite = false;
    for (int i = 0; i < 40 && !sawWrite; ++i) {
        const auto event = reader.next(3000ms);
        if (!event)
            break;
        if (event->name != "state")
            continue;

        const auto snapshot = parseSnapshot(event->data);
        sawWrite = snapshot && snapshot->keys == 1 && snapshot->lastApplied > start->lastApplied &&
                   snapshot->commitIndex >= snapshot->lastApplied;
    }
    CHK(sawWrite);
}

// Verifies an idle stream keeps itself alive with ping events.
static void idle_stream_sends_ping() {
    TestCluster cluster(3, 19020);
    cluster.serverOptions().streamKeepAlive = 150ms;
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    SseReader reader(cluster.portOf(1), "/events");
    CHK(reader.connected());

    bool sawPing = false;
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    while (!sawPing && std::chrono::steady_clock::now() < deadline) {
        const auto event = reader.next(1000ms);
        if (event && event->name == "ping")
            sawPing = true;
    }
    CHK(sawPing);
}

// Verifies the cluster stream reports all three nodes up, with a leader and quorum.
static void cluster_stream_shows_all_up() {
    TestCluster cluster(3, 19025);
    CHK(cluster.startAll());

    SseReader reader(cluster.portOf(0), "/cluster/events");
    CHK(reader.connected());
    CHK(reader.status() == 200);

    View view;
    CHK(waitForView(reader,
                    [](const View& v) { return v.allUp() && v.leader.has_value() && v.quorum; }, 6000ms,
                    &view));

    CHK(view.members.size() == 3);
    CHK(view.members[0].id == "node-1"); // The observer is always first.
    CHK(view.term >= 1);

    int leaders = 0;
    for (const View::Member& member : view.members)
        if (member.role == "leader")
            ++leaders;
    CHK(leaders == 1);
}

// Verifies GET /cluster is the same view without streaming.
static void cluster_endpoint_returns_view() {
    TestCluster cluster(3, 19030);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    bool complete = false;
    for (int i = 0; i < 100 && !complete; ++i) {
        const auto response = api::call(HttpMethod::Get, cluster.portOf(2), "/cluster");
        CHK(response.has_value() && response->statusCode == 200);
        if (!response)
            return;

        const auto view = parseView(response->body);
        CHK(view.has_value());
        complete = view && view->allUp() && view->leader && view->members[0].id == "node-3";
        if (!complete)
            std::this_thread::sleep_for(50ms);
    }
    CHK(complete);
}

// Verifies deaths and returns are pushed, and that quorum tracks them.
static void cluster_stream_sees_node_death() {
    TestCluster cluster(3, 19035);
    CHK(cluster.startAll());

    SseReader reader(cluster.portOf(0), "/cluster/events");
    CHK(reader.connected());
    CHK(waitForView(reader, [](const View& v) { return v.allUp() && v.leader && v.quorum; }, 6000ms));

    // One node dies: shown DOWN, quorum intact.
    cluster.kill(2);
    View view;
    CHK(waitForView(reader,
                    [](const View& v) {
                        const View::Member* m = v.find("node-3");
                        return m && !m->up;
                    },
                    4000ms, &view));
    CHK(view.quorum);

    // A second dies: the survivor can no longer form a majority.
    cluster.kill(1);
    CHK(waitForView(reader, [](const View& v) { return !v.quorum; }, 4000ms, &view));
    const View::Member* second = view.find("node-2");
    CHK(second != nullptr && !second->up);
    const View::Member* self = view.find("node-1");
    CHK(self != nullptr && self->up); // The observer always knows it is itself alive.

    // One comes back: up again, quorum restored.
    CHK(cluster.start(1));
    CHK(waitForView(reader,
                    [](const View& v) {
                        const View::Member* m = v.find("node-2");
                        return m && m->up && v.quorum;
                    },
                    6000ms));
}

// Verifies a leader change is pushed: a different leader in a higher term.
static void cluster_stream_sees_leader_change() {
    TestCluster cluster(3, 19040);
    CHK(cluster.startAll());

    const int oldLeader = cluster.waitForLeader();
    CHK(oldLeader >= 0);
    if (oldLeader < 0)
        return;

    const int observer = (oldLeader + 1) % 3;
    SseReader reader(cluster.portOf(observer), "/cluster/events");
    CHK(reader.connected());

    View before;
    const std::string oldId = cluster.node(oldLeader).nodeId();
    CHK(waitForView(reader, [&](const View& v) { return v.allUp() && v.leader == oldId; }, 6000ms,
                    &before));

    cluster.kill(oldLeader);

    CHK(waitForView(reader,
                    [&](const View& v) { return v.leader && *v.leader != oldId && v.term > before.term; },
                    8000ms));
}

// Verifies the stream cap returns 503 and a closed stream frees its slot.
static void stream_cap_enforced() {
    TestCluster cluster(3, 19045);
    cluster.serverOptions().streamKeepAlive = 100ms; // frees a slot quickly after a client leaves
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    // threadCount 8 -> at most 4 streams on one node.
    std::vector<std::unique_ptr<SseReader>> streams;
    for (int i = 0; i < 4; ++i) {
        streams.push_back(std::make_unique<SseReader>(cluster.portOf(0), "/events"));
        CHK(streams.back()->connected() && streams.back()->status() == 200);
    }

    SseReader overflow(cluster.portOf(0), "/events");
    CHK(overflow.connected());
    CHK(overflow.status() == 503);

    // Free one slot; the server notices when its next send fails.
    streams[0]->close();
    streams.erase(streams.begin());

    bool admitted = false;
    for (int i = 0; i < 40 && !admitted; ++i) {
        SseReader retry(cluster.portOf(0), "/events");
        admitted = retry.connected() && retry.status() == 200;
        if (!admitted)
            std::this_thread::sleep_for(100ms);
    }
    CHK(admitted);
}

// Verifies CORS headers appear on plain responses, stream preambles, and preflights.
static void cors_headers_present() {
    TestCluster cluster(3, 19050);
    cluster.serverOptions().corsOrigin = "http://localhost:5173";
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    auto status = api::call(HttpMethod::Get, cluster.portOf(0), "/status");
    CHK(status.has_value() && status->statusCode == 200);
    if (status)
        CHK(status->headers["access-control-allow-origin"] == "http://localhost:5173");

    SseReader stream(cluster.portOf(0), "/cluster/events");
    CHK(stream.connected() && stream.status() == 200);
    CHK(stream.header("access-control-allow-origin") == "http://localhost:5173");

    auto preflight = api::call(HttpMethod::Options, cluster.portOf(0), "/kv/anything");
    CHK(preflight.has_value());
    if (preflight) {
        CHK(preflight->statusCode == 204 || preflight->statusCode == 200);
        CHK(preflight->headers["access-control-allow-methods"].find("PUT") != std::string::npos);
        CHK(preflight->headers["access-control-allow-origin"] == "http://localhost:5173");
    }
}

// Verifies there is no CORS header unless an origin was configured.
static void cors_off_by_default() {
    TestCluster cluster(3, 19055);
    CHK(cluster.startAll());

    auto status = api::call(HttpMethod::Get, cluster.portOf(0), "/status");
    CHK(status.has_value());
    if (status)
        CHK(!status->headers.contains("access-control-allow-origin"));
}

// Verifies stopping a node that has open streams returns promptly and ends them.
static void shutdown_closes_streams() {
    TestCluster cluster(3, 19060);
    CHK(cluster.startAll());
    CHK(cluster.waitForLeader() >= 0);

    SseReader nodeStream(cluster.portOf(0), "/events");
    SseReader clusterStream(cluster.portOf(0), "/cluster/events");
    CHK(nodeStream.connected() && clusterStream.connected());
    CHK(nodeStream.next(2000ms).has_value());
    CHK(clusterStream.next(2000ms).has_value());

    const auto begin = std::chrono::steady_clock::now();
    cluster.kill(0);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CHK(elapsed < 3000ms); // streams must not pin the server open

    // Drain whatever was in flight; the stream must then end, not stall.
    for (int i = 0; i < 20 && !nodeStream.closed(); ++i)
        (void)nodeStream.next(500ms);
    for (int i = 0; i < 20 && !clusterStream.closed(); ++i)
        (void)clusterStream.next(500ms);
    CHK(nodeStream.closed());
    CHK(clusterStream.closed());
}

static void run_tests() {
    RUN(node_stream_sends_state_first);
    RUN(node_stream_pushes_changes);
    RUN(idle_stream_sends_ping);
    RUN(cluster_stream_shows_all_up);
    RUN(cluster_endpoint_returns_view);
    RUN(cluster_stream_sees_node_death);
    RUN(cluster_stream_sees_leader_change);
    RUN(stream_cap_enforced);
    RUN(cors_headers_present);
    RUN(cors_off_by_default);
    RUN(shutdown_closes_streams);
}

REGISTER_TEST_SUITE();
