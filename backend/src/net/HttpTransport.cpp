/**
 * @file HttpTransport.cpp
 * @brief HttpTransport implementation.
 */

// ============================================================
// Implementation for RaftKV::Net::HttpTransport.
// ============================================================
//
//  Sections:
//   1. Construction & Shutdown
//   2. Enqueueing
//   3. Sender Thread
//   4. Transport Interface
//
// ============================================================

// clang-format off
#include "HttpTransport.h" // HttpTransport — the class this file implements

#include "RpcCodec.h"      // encode(), decode*(), RPC paths

#include <HashMapPro/HashMap.h> // HashMapPro::HashMap - request headers
#include <chrono>               // std::chrono::steady_clock - stamping AppendEntries sends
#include <utility>              // std::move
// clang-format on

namespace RaftKV::Net {

using namespace RaftCore;

namespace {

FalconHTTP::Client::ClientOptions clientOptions(const TransportOptions& options) {
    FalconHTTP::Client::ClientOptions out;
    out.connectTimeout = options.connectTimeout;
    out.ioTimeout = options.ioTimeout;
    return out;
}

HashMapPro::HashMap<std::string, std::string> binaryHeaders() {
    HashMapPro::HashMap<std::string, std::string> headers;
    headers["Content-Type"] = "application/octet-stream";
    return headers;
}

} // namespace

// ============================================================
//  Section 1 — Construction & Shutdown
// ============================================================

HttpTransport::HttpTransport(const std::vector<PeerAddress>& peers, LockedRunner runner,
                             TransportOptions options, AckObserver ackObserver)
    : options_(options), clientOptions_(clientOptions(options)), runner_(std::move(runner)),
      ackObserver_(std::move(ackObserver)) {
    for (const PeerAddress& peer : peers) {
        auto sender = std::make_unique<PeerSender>();
        sender->address = peer;

        PeerSender* raw = sender.get();
        senders_.emplace(peer.id, std::move(sender));
        raw->thread = std::thread([this, raw]() { senderLoop(*raw); });
    }
}

HttpTransport::~HttpTransport() {
    stop();
}

void HttpTransport::stop() {
    for (auto& entry : senders_) {
        PeerSender& sender = *entry.second;
        {
            std::lock_guard lock(sender.mutex);
            sender.stopping = true;
            sender.queue.clear();
        }
        sender.wake.notify_all();
    }

    for (auto& entry : senders_)
        if (entry.second->thread.joinable())
            entry.second->thread.join();
}

// A fresh HttpClient is just an options struct, so building one per RPC to
// stretch its I/O timeout by the request size costs nothing.
FalconHTTP::Client::HttpClient HttpTransport::clientFor(std::size_t bodyBytes) const {
    FalconHTTP::Client::ClientOptions scaled = clientOptions_;
    scaled.ioTimeout += options_.perKiBTimeout * static_cast<long long>(bodyBytes / 1024);
    return FalconHTTP::Client::HttpClient(scaled);
}

// ============================================================
//  Section 2 — Enqueueing
// ============================================================

void HttpTransport::enqueue(const NodeId& target, FunctionPro::MoveOnlyFunction<void()> task,
                            bool supersedable) {
    PeerSender& sender = *senders_.at(target);
    {
        std::lock_guard lock(sender.mutex);
        if (sender.stopping)
            return;

        // A newer AppendEntries makes any not-yet-sent older one pointless. Without
        // this, when a round trip takes longer than the heartbeat interval, a new
        // entry would wait behind a backlog of stale heartbeats.
        if (supersedable)
            std::erase_if(sender.queue,
                          [](const PeerSender::Pending& p) { return p.supersedable; });

        if (sender.queue.size() >= options_.maxQueuedPerPeer)
            sender.queue.pop_front(); // Oldest is the stalest; Raft will resend.

        sender.queue.push_back(PeerSender::Pending{std::move(task), supersedable});
    }
    sender.wake.notify_one();
}

// ============================================================
//  Section 3 — Sender Thread
// ============================================================

void HttpTransport::senderLoop(PeerSender& sender) {
    while (true) {
        FunctionPro::MoveOnlyFunction<void()> task;
        {
            std::unique_lock lock(sender.mutex);
            sender.wake.wait(lock, [&]() { return sender.stopping || !sender.queue.empty(); });
            if (sender.stopping)
                return;

            task = std::move(sender.queue.front().task);
            sender.queue.pop_front();
        }

        task(); // Blocking HTTP call; the queue lock is not held.
    }
}

// ============================================================
//  Section 4 — Transport Interface
// ============================================================

Status
HttpTransport::sendRequestVote(const NodeId& target, RequestVoteArgs args,
                               FunctionPro::MoveOnlyFunction<void(RequestVoteReply)> onReply) {
    const auto found = senders_.find(target);
    if (found == senders_.end())
        return Status::NOT_FOUND;

    PeerAddress address =
        found->second
            ->address; // Non-const: a const capture would be copied, not moved (not nothrow).
    std::string body = encode(args);

    enqueue(target, [this, address = std::move(address), body = std::move(body),
                     onReply = std::move(onReply)]() mutable {
        const auto response =
            clientFor(body.size())
                .post(address.host, address.port, kRequestVotePath, body, binaryHeaders());
        if (!response || !response->ok())
            return;

        const auto reply = decodeRequestVoteReply(response->body);
        if (!reply)
            return;

        FunctionPro::MoveOnlyFunction<void()> deliver = [&onReply, reply = *reply]() mutable {
            onReply(reply);
        };
        runner_(deliver);
    });

    return Status::OK;
}

Status
HttpTransport::sendAppendEntries(const NodeId& target, AppendEntriesArgs args,
                                 FunctionPro::MoveOnlyFunction<void(AppendEntriesReply)> onReply) {
    const auto found = senders_.find(target);
    if (found == senders_.end())
        return Status::NOT_FOUND;

    PeerAddress address =
        found->second
            ->address; // Non-const: a const capture would be copied, not moved (not nothrow).
    std::string body = encode(args);
    const auto sentAt = std::chrono::steady_clock::now();

    enqueue(
        target,
        [this, peerId = target, address = std::move(address), body = std::move(body), sentAt,
         onReply = std::move(onReply)]() mutable {
            const auto response =
                clientFor(body.size())
                    .post(address.host, address.port, kAppendEntriesPath, body, binaryHeaders());
            if (!response || !response->ok())
                return;

            const auto reply = decodeAppendEntriesReply(response->body);
            if (!reply)
                return;

            FunctionPro::MoveOnlyFunction<void()> deliver = [&]() mutable {
                if (ackObserver_)
                    ackObserver_(peerId, *reply, sentAt);
                onReply(*reply);
            };
            runner_(deliver);
        },
        /*supersedable=*/true);

    return Status::OK;
}

} // namespace RaftKV::Net
