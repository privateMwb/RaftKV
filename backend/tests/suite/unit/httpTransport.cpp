// Unit HttpTransport Test Suite
// Verifies the HTTP Transport against a stub peer (a real FalconHTTP server
// returning canned replies): requests arrive intact, replies come back through
// the locked runner, and every failure mode is a silent drop.
//
// Covers:
// - A RequestVote reply is decoded and delivered via the runner
// - The ack observer sees each AppendEntries reply with the peer id and a
//   send time no earlier than the call, before RaftCore's callback runs
// - AppendEntries args (with entries) reach the peer intact
// - RPCs to one peer arrive in the order they were sent
// - A slow reply is abandoned for a small RPC but awaited for a large one
//   (the I/O timeout grows with the body size)
// - An unknown target is NOT_FOUND
// - A dead peer, a 500, and an undecodable 200 body never invoke the callback
// - stop() is idempotent and discards queued work

#include <support/framework.h>

#include <FalconHTTP/FalconHTTP.h>

#include "net/HttpTransport.h"
#include "net/RpcCodec.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace RaftKV::Net;
using namespace RaftCore;
using namespace std::chrono_literals;
using FalconHTTP::HTTP::HttpRequest;
using FalconHTTP::HTTP::HttpResponse;
using FalconHTTP::HTTP::HttpStatus;

namespace {

// A stub peer: a Router + Server on its own thread, stopped on scope exit.
class StubPeer {
  public:
    explicit StubPeer(uint16_t port) : server_(router_, 2), port_(port) {}

    ~StubPeer() {
        stop();
    }

    FalconHTTP::Routing::Router& router() {
        return router_;
    }

    bool start() {
        if (!server_.start(port_))
            return false;
        running_ = true;
        thread_ = std::thread([this]() { server_.run(); });
        std::this_thread::sleep_for(50ms);
        return true;
    }

    void stop() {
        if (!running_)
            return;
        running_ = false;
        server_.stop();
        if (thread_.joinable())
            thread_.join();
    }

  private:
    FalconHTTP::Routing::Router router_;
    FalconHTTP::Core::Server server_;
    std::thread thread_;
    uint16_t port_;
    bool running_ = false;
};

// The "lock" the transport delivers replies under in these tests.
struct CountingRunner {
    std::mutex mutex;
    int runs = 0;

    HttpTransport::LockedRunner runner() {
        return [this](FunctionPro::MoveOnlyFunction<void()>& deliver) {
            std::lock_guard lock(mutex);
            ++runs;
            deliver();
        };
    }
};

bool waitFor(const std::atomic<int>& counter, int target, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (counter.load() >= target)
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return counter.load() >= target;
}

} // namespace

// Verifies a granted vote comes back decoded, through the runner.
static void vote_reply_delivered() {
    StubPeer peer(18851);
    peer.router().post(kRequestVotePath, [](const HttpRequest& request, HttpResponse& response) {
        const auto args = decodeRequestVoteArgs(request.body());
        RequestVoteReply reply;
        reply.term = args ? args->term : 0;
        reply.voteGranted = true;
        response.setStatus(HttpStatus::Ok);
        response.setBody(encode(reply));
    });
    CHK(peer.start());

    CountingRunner runner;
    HttpTransport transport({{"peer", "127.0.0.1", 18851}}, runner.runner());

    std::atomic<int> delivered{0};
    std::atomic<Term> seenTerm{0};
    std::atomic<bool> seenGrant{false};

    RequestVoteArgs args;
    args.term = 7;
    args.candidateId = "me";
    CHK(transport.sendRequestVote("peer", args, [&](RequestVoteReply reply) {
        seenTerm = reply.term;
        seenGrant = reply.voteGranted;
        ++delivered;
    }) == Status::OK);

    CHK(waitFor(delivered, 1, 3s));
    CHK(seenTerm == 7);
    CHK(seenGrant);
    CHK(runner.runs == 1);
}

// Verifies AppendEntries args, entries included, reach the peer unchanged.
static void append_args_arrive_intact() {
    StubPeer peer(18852);
    std::mutex seenMutex;
    std::optional<AppendEntriesArgs> seen;
    peer.router().post(kAppendEntriesPath, [&](const HttpRequest& request, HttpResponse& response) {
        {
            std::lock_guard lock(seenMutex);
            seen = decodeAppendEntriesArgs(request.body());
        }
        AppendEntriesReply reply;
        reply.term = 3;
        reply.success = true;
        reply.matchIndex = 5;
        response.setStatus(HttpStatus::Ok);
        response.setBody(encode(reply));
    });
    CHK(peer.start());

    CountingRunner runner;
    HttpTransport transport({{"peer", "127.0.0.1", 18852}}, runner.runner());

    AppendEntriesArgs args;
    args.term = 3;
    args.leaderId = "leader";
    args.prevLogIndex = 4;
    args.prevLogTerm = 2;
    args.leaderCommit = 4;
    LogEntry entry;
    entry.term = 3;
    entry.payload.push_back(0);
    entry.payload.push_back(255);
    args.entries.push_back(entry);

    std::atomic<int> delivered{0};
    std::atomic<LogIndex> match{0};
    CHK(transport.sendAppendEntries("peer", args, [&](AppendEntriesReply reply) {
        match = reply.matchIndex;
        ++delivered;
    }) == Status::OK);

    CHK(waitFor(delivered, 1, 3s));
    CHK(match == 5);

    std::lock_guard lock(seenMutex);
    CHK(seen.has_value());
    if (seen) {
        CHK(seen->leaderId == "leader");
        CHK(seen->prevLogIndex == 4);
        CHK(seen->entries.size() == 1);
        if (seen->entries.size() == 1) {
            CHK(seen->entries[0].payload.size() == 2);
            CHK(seen->entries[0].payload[1] == 255);
        }
    }
}

// Verifies the ack observer fires first, with the right peer and send time.
static void ack_observer_sees_reply() {
    StubPeer peer(18855);
    peer.router().post(kAppendEntriesPath, [](const HttpRequest&, HttpResponse& response) {
        AppendEntriesReply reply;
        reply.term = 6;
        reply.success = false;
        response.setStatus(HttpStatus::Ok);
        response.setBody(encode(reply));
    });
    CHK(peer.start());

    CountingRunner runner;
    std::mutex eventsMutex;
    std::vector<std::string> events;
    std::chrono::steady_clock::time_point observedSentAt;

    HttpTransport transport(
        {{"peer-x", "127.0.0.1", 18855}}, runner.runner(), TransportOptions(),
        [&](const NodeId& id, const AppendEntriesReply& reply,
            std::chrono::steady_clock::time_point sentAt) {
            std::lock_guard lock(eventsMutex);
            events.push_back("observer:" + id + ":" + std::to_string(reply.term));
            observedSentAt = sentAt;
        });

    const auto before = std::chrono::steady_clock::now();
    std::atomic<int> delivered{0};
    CHK(transport.sendAppendEntries("peer-x", AppendEntriesArgs(), [&](AppendEntriesReply) {
        std::lock_guard lock(eventsMutex);
        events.push_back("callback");
        ++delivered;
    }) == Status::OK);

    CHK(waitFor(delivered, 1, 3s));

    std::lock_guard lock(eventsMutex);
    CHK(events.size() == 2);
    if (events.size() == 2) {
        CHK(events[0] == "observer:peer-x:6"); // before RaftCore's callback.
        CHK(events[1] == "callback");
    }
    CHK(observedSentAt >= before);
    CHK(observedSentAt <= std::chrono::steady_clock::now());
}

// Verifies RPCs to one peer are sent in order.
static void per_peer_order_preserved() {
    StubPeer peer(18853);
    std::mutex orderMutex;
    std::vector<Term> order;
    peer.router().post(kRequestVotePath, [&](const HttpRequest& request, HttpResponse& response) {
        if (const auto args = decodeRequestVoteArgs(request.body())) {
            std::lock_guard lock(orderMutex);
            order.push_back(args->term);
        }
        RequestVoteReply reply;
        response.setStatus(HttpStatus::Ok);
        response.setBody(encode(reply));
    });
    CHK(peer.start());

    CountingRunner runner;
    HttpTransport transport({{"peer", "127.0.0.1", 18853}}, runner.runner());

    std::atomic<int> delivered{0};
    for (Term term = 1; term <= 6; ++term) {
        RequestVoteArgs args;
        args.term = term;
        CHK(transport.sendRequestVote("peer", args, [&](RequestVoteReply) { ++delivered; }) ==
            Status::OK);
    }

    CHK(waitFor(delivered, 6, 5s));

    std::lock_guard lock(orderMutex);
    CHK(order.size() == 6);
    for (std::size_t i = 0; i < order.size(); ++i)
        CHK(order[i] == i + 1);
}

// Verifies the I/O timeout stretches with the body size: the same 450 ms
// handler is too slow for a tiny RPC but fine for one carrying ~400 KiB.
static void timeout_scales_with_body() {
    StubPeer peer(18856);
    peer.router().post(kAppendEntriesPath, [](const HttpRequest&, HttpResponse& response) {
        std::this_thread::sleep_for(450ms); // e.g. a slow fsync on the follower.
        AppendEntriesReply reply;
        reply.term = 1;
        reply.success = true;
        response.setStatus(HttpStatus::Ok);
        response.setBody(encode(reply));
    });
    CHK(peer.start());

    CountingRunner runner;
    TransportOptions options; // ioTimeout 300 ms + 1 ms per KiB.
    HttpTransport transport({{"peer", "127.0.0.1", 18856}}, runner.runner(), options);

    std::atomic<int> delivered{0};

    // Tiny body: 300 ms allowance < 450 ms handler -> abandoned.
    CHK(transport.sendAppendEntries("peer", AppendEntriesArgs(), [&](AppendEntriesReply) { ++delivered; }) ==
        Status::OK);
    std::this_thread::sleep_for(900ms);
    CHK(delivered == 0);

    // ~400 KiB body: 300 + 400 = 700 ms allowance > 450 ms -> delivered.
    AppendEntriesArgs big;
    big.term = 1;
    big.leaderId = "n";
    LogEntry entry;
    entry.term = 1;
    for (int i = 0; i < 400 * 1024; ++i)
        entry.payload.push_back(static_cast<std::uint8_t>(i));
    big.entries.push_back(std::move(entry));

    CHK(transport.sendAppendEntries("peer", std::move(big), [&](AppendEntriesReply) { ++delivered; }) ==
        Status::OK);
    CHK(waitFor(delivered, 1, 5s));
}

// Verifies sending to a node that isn't a configured peer is rejected.
static void unknown_peer_not_found() {
    CountingRunner runner;
    HttpTransport transport({{"peer", "127.0.0.1", 18859}}, runner.runner());

    RequestVoteArgs args;
    CHK(transport.sendRequestVote("stranger", args, [](RequestVoteReply) {}) == Status::NOT_FOUND);
    CHK(transport.sendAppendEntries("stranger", AppendEntriesArgs(), [](AppendEntriesReply) {}) ==
        Status::NOT_FOUND);
}

// Verifies a peer that is down never produces a callback (and never hangs send).
static void dead_peer_is_silent() {
    CountingRunner runner;
    HttpTransport transport({{"peer", "127.0.0.1", 18858}}, runner.runner()); // nobody listens.

    std::atomic<int> delivered{0};
    RequestVoteArgs args;
    CHK(transport.sendRequestVote("peer", args, [&](RequestVoteReply) { ++delivered; }) ==
        Status::OK);

    std::this_thread::sleep_for(400ms);
    CHK(delivered == 0);
    CHK(runner.runs == 0);
}

// Verifies a 500 and an undecodable 200 body are both dropped.
static void bad_replies_dropped() {
    StubPeer peer(18854);
    peer.router().post(kRequestVotePath, [](const HttpRequest&, HttpResponse& response) {
        response.setStatus(HttpStatus::InternalServerError);
    });
    peer.router().post(kAppendEntriesPath, [](const HttpRequest&, HttpResponse& response) {
        response.setStatus(HttpStatus::Ok);
        response.setBody("this is not a reply");
    });
    CHK(peer.start());

    CountingRunner runner;
    HttpTransport transport({{"peer", "127.0.0.1", 18854}}, runner.runner());

    std::atomic<int> delivered{0};
    CHK(transport.sendRequestVote("peer", RequestVoteArgs{}, [&](RequestVoteReply) { ++delivered; }) ==
        Status::OK);
    CHK(transport.sendAppendEntries("peer", AppendEntriesArgs(),
                                    [&](AppendEntriesReply) { ++delivered; }) == Status::OK);

    std::this_thread::sleep_for(500ms);
    CHK(delivered == 0);
    CHK(runner.runs == 0);
}

// Verifies stop() can be called twice and sends after it are ignored.
static void stop_is_idempotent() {
    CountingRunner runner;
    HttpTransport transport({{"peer", "127.0.0.1", 18857}}, runner.runner());

    transport.stop();
    transport.stop();

    std::atomic<int> delivered{0};
    CHK(transport.sendRequestVote("peer", RequestVoteArgs{}, [&](RequestVoteReply) { ++delivered; }) ==
        Status::OK);
    std::this_thread::sleep_for(100ms);
    CHK(delivered == 0);
}

static void run_tests() {
    RUN(vote_reply_delivered);
    RUN(ack_observer_sees_reply);
    RUN(append_args_arrive_intact);
    RUN(per_peer_order_preserved);
    RUN(timeout_scales_with_body);
    RUN(unknown_peer_not_found);
    RUN(dead_peer_is_silent);
    RUN(bad_replies_dropped);
    RUN(stop_is_idempotent);
}

REGISTER_TEST_SUITE();
