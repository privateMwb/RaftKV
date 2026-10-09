/**
 * @file ClusterMonitor.cpp
 * @brief ClusterMonitor implementation.
 */

// ============================================================
// Implementation for RaftKV::Net::ClusterMonitor.
// ============================================================
//
//  Sections:
//   1. Snapshot Building & JSON
//   2. Construction & Lifecycle
//   3. Watchers
//   4. Consumers
//
// ============================================================

// clang-format off
#include "ClusterMonitor.h"   // ClusterMonitor — the class this file implements

#include "../node/KvNode.h"   // RaftKV::Node::KvNode - the observer and its peer list

#include <FalconHTTP/Client/HttpClient.h> // FalconHTTP::Client::HttpClient - polling peers' /status

#include <algorithm>          // std::max - highest term across members
#include <utility>            // std::move
// clang-format on

namespace RaftKV::Net {

// ============================================================
//  Section 1 — Snapshot Building & JSON
// ============================================================

std::string toJson(const ClusterSnapshot& snapshot) {
    std::string json = "{";
    json += "\"observer\":" + Node::jsonQuote(snapshot.observer);
    json += ",\"term\":" + std::to_string(snapshot.term);
    json += ",\"leader\":" + (snapshot.leader ? Node::jsonQuote(*snapshot.leader) : std::string("null"));
    json += std::string(",\"quorum\":") + (snapshot.quorum ? "true" : "false");
    json += ",\"members\":[";

    for (std::size_t i = 0; i < snapshot.members.size(); ++i) {
        const ClusterMember& member = snapshot.members[i];
        if (i > 0)
            json += ",";

        json += "{\"id\":" + Node::jsonQuote(member.id);
        json += std::string(",\"up\":") + (member.up ? "true" : "false");
        json += ",\"state\":" + (member.state ? Node::toJson(*member.state) : std::string("null"));
        json += "}";
    }

    json += "]}";
    return json;
}

ClusterSnapshot ClusterMonitor::buildLocked() const {
    ClusterSnapshot snapshot;
    snapshot.observer = members_.front().id;
    snapshot.members = members_;

    std::size_t upCount = 0;
    for (const ClusterMember& member : members_) {
        if (!member.up || !member.state)
            continue;

        ++upCount;
        snapshot.term = std::max(snapshot.term, member.state->term);
    }

    // The leader is the up member that leads in the highest term: a deposed
    // leader that hasn't heard the news yet still *claims* Leader, but in an
    // older term.
    RaftCore::Term leaderTerm = RaftCore::kNoTerm;
    for (const ClusterMember& member : members_) {
        if (member.up && member.state && member.state->role == RaftCore::Role::Leader &&
            (!snapshot.leader || member.state->term > leaderTerm)) {
            snapshot.leader = member.id;
            leaderTerm = member.state->term;
        }
    }

    snapshot.quorum = upCount >= members_.size() / 2 + 1;
    return snapshot;
}

// ============================================================
//  Section 2 — Construction & Lifecycle
// ============================================================

ClusterMonitor::ClusterMonitor(Node::KvNode& node, MonitorOptions options)
    : node_(node), options_(options) {
    members_.push_back(ClusterMember{node_.nodeId(), false, std::nullopt});
    for (const PeerAddress& peer : node_.peers())
        members_.push_back(ClusterMember{peer.id, false, std::nullopt});
}

ClusterMonitor::~ClusterMonitor() {
    stop();
}

void ClusterMonitor::start() {
    {
        std::lock_guard lock(mutex_);
        if (running_)
            return;
        running_ = true;
    }

    threads_.emplace_back([this]() { watchSelf(); });

    const std::vector<PeerAddress>& peers = node_.peers();
    for (std::size_t i = 0; i < peers.size(); ++i)
        threads_.emplace_back(
            [this, i, host = peers[i].host, port = peers[i].port]() { watchPeer(i + 1, host, port); });
}

void ClusterMonitor::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_ && threads_.empty())
            return;
        running_ = false;
    }
    condition_.notify_all();
    node_.wakeListeners(); // watchSelf sleeps on the node's condition variable, not ours.

    for (std::thread& thread : threads_)
        if (thread.joinable())
            thread.join();
    threads_.clear();
}

// ============================================================
//  Section 3 — Watchers
// ============================================================

// Records a member's status; bumps the version only if it really changed.
void ClusterMonitor::setMember(std::size_t index, bool up, std::optional<Node::NodeSnapshot> state) {
    {
        std::lock_guard lock(mutex_);
        ClusterMember next{members_[index].id, up, up ? std::move(state) : std::nullopt};
        if (members_[index] == next && version_ != 0)
            return;

        members_[index] = std::move(next);
        ++version_;
    }
    condition_.notify_all();
}

void ClusterMonitor::watchSelf() {
    std::uint64_t seen = 0;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!running_)
                return;
        }

        // Sleeps on KvNode's condition variable: wakes the instant anything
        // about this node changes. The timeout only bounds how long stop() can
        // take when its wake-up lands between the running_ check above and this
        // wait (a missed signal), so keep it short.
        const Node::StateUpdate update = node_.waitForChange(seen, std::chrono::milliseconds(40));
        if (update.version != seen) {
            seen = update.version;
            setMember(0, true, update.snapshot);
        }
    }
}

void ClusterMonitor::watchPeer(std::size_t index, std::string host, uint16_t port) {
    FalconHTTP::Client::ClientOptions clientOptions;
    clientOptions.connectTimeout = options_.connectTimeout;
    clientOptions.ioTimeout = options_.ioTimeout;
    const FalconHTTP::Client::HttpClient client(clientOptions);

    int failures = 0;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!running_)
                return;
        }

        std::optional<Node::NodeSnapshot> reported;
        if (const auto response = client.get(host, port, "/status"); response && response->ok())
            reported = Node::parseSnapshot(response->body);

        if (reported) {
            failures = 0;
            setMember(index, true, std::move(reported));
        } else if (++failures >= options_.failuresBeforeDown) {
            setMember(index, false, std::nullopt);
        }

        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, options_.pollInterval, [this]() { return !running_; });
    }
}

// ============================================================
//  Section 4 — Consumers
// ============================================================

ClusterSnapshot ClusterMonitor::snapshot() const {
    std::lock_guard lock(mutex_);
    return buildLocked();
}

ClusterUpdate ClusterMonitor::waitForChange(std::uint64_t lastVersion,
                                            std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, timeout, [&]() { return version_ != lastVersion || !running_; });
    return ClusterUpdate{version_, buildLocked()};
}

} // namespace RaftKV::Net
