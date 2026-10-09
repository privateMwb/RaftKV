/**
 * @file            ClusterMonitor.h
 *
 * @date            2026-10-9
 *
 * @version         1.0.0
 *
 * @copyright       Copyright (c) 2026 privateMwb
 *                  All rights reserved.
 *                  https://github.com/privateMwb/RaftKV
 *
 * @attention       This source is released under the MIT license
 *                  SPDX-License-Identifier: MIT
 *                  <http://opensource.org/licenses/MIT>
 */

#pragma once

// clang-format off
#include "../node/NodeSnapshot.h"   // RaftKV::Node::NodeSnapshot

#include <chrono>              // std::chrono::milliseconds
#include <condition_variable>  // std::condition_variable - waking stream handlers
#include <cstddef>             // std::size_t
#include <cstdint>             // std::uint64_t - change version
#include <mutex>               // std::mutex
#include <optional>            // std::optional
#include <string>              // std::string
#include <thread>              // std::thread - one watcher per member
#include <vector>              // std::vector
// clang-format on

namespace RaftKV::Node {
class KvNode;
}

namespace RaftKV::Net {

/// @brief One node as seen by the observer.
struct ClusterMember {
    std::string id;
    /// False until the first successful poll, and again after repeated failures.
    bool up = false;
    /// The node's last reported state; set exactly when `up`.
    std::optional<Node::NodeSnapshot> state;

    [[nodiscard]] bool operator==(const ClusterMember&) const = default;
};

/// @brief The cluster as one node sees it: itself plus every peer.
struct ClusterSnapshot {
    /// The node that produced this view (always `members[0]`).
    std::string observer;
    /// The observer first, then the peers in configuration order.
    std::vector<ClusterMember> members;

    /// Derived. The id of the up member that is Leader in the highest term;
    /// `std::nullopt` during an election or when no leader is reachable.
    std::optional<std::string> leader;
    /// Derived. The highest term reported by any up member.
    RaftCore::Term term = RaftCore::kNoTerm;
    /// Derived. Whether a majority of members are up (the cluster can commit).
    bool quorum = false;
};

/// @brief Serializes a ClusterSnapshot to JSON:
/// `{"observer":..,"term":..,"leader":..|null,"quorum":..,"members":[{"id":..,"up":..,"state":{..}|null},..]}`.
[[nodiscard]] std::string toJson(const ClusterSnapshot& snapshot);

/// @brief Polling and sensitivity settings for ClusterMonitor.
struct MonitorOptions {
    /// Delay between polls of one peer.
    std::chrono::milliseconds pollInterval{150};
    std::chrono::milliseconds connectTimeout{100};
    std::chrono::milliseconds ioTimeout{250};
    /// A peer is declared down after this many consecutive failed polls, so a
    /// single dropped request doesn't flash a healthy node as dead.
    int failuresBeforeDown = 2;
};

/// @brief A ClusterSnapshot plus the version at which it was taken.
struct ClusterUpdate {
    std::uint64_t version = 0;
    ClusterSnapshot snapshot;
};

/**
 * @brief Maintains this node's live view of the whole cluster.
 *
 * @details The observer's own state is pushed straight from KvNode (a watcher
 * thread sleeps on KvNode::waitForChange, so it reacts the instant anything
 * changes). Each peer is watched by its own thread that polls the peer's
 * `GET /status` every `pollInterval` and parses the reply; a peer is up after a
 * successful poll and down after `failuresBeforeDown` consecutive failures. So
 * the observer's view of *itself* is instantaneous, while a peer's changes show
 * up within about one poll interval (150 ms by default).
 *
 * Consumers (the `/cluster` endpoints) call waitForChange(), which sleeps on a
 * condition variable and wakes only when the cluster view really changed:
 * nothing is polled on the consumer's side, and N browsers cost N sleeping
 * threads, not N sets of peer polls.
 */
class ClusterMonitor {
  private:
    Node::KvNode& node_;
    MonitorOptions options_;

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::vector<ClusterMember> members_; ///< Index 0 is the observer.
    std::uint64_t version_ = 0;
    bool running_ = false;
    std::vector<std::thread> threads_;

    void watchSelf();
    void watchPeer(std::size_t index, std::string host, uint16_t port);
    void setMember(std::size_t index, bool up, std::optional<Node::NodeSnapshot> state);
    [[nodiscard]] ClusterSnapshot buildLocked() const;

  public:
    /// @param node The observer. Must outlive the monitor.
    explicit ClusterMonitor(Node::KvNode& node, MonitorOptions options = MonitorOptions());
    ~ClusterMonitor();

    ClusterMonitor(const ClusterMonitor&) = delete;
    ClusterMonitor& operator=(const ClusterMonitor&) = delete;

    /// @brief Starts the watcher threads. No-op if already running.
    void start();

    /// @brief Stops and joins them, and wakes every waitForChange() caller.
    /// Idempotent.
    void stop();

    /// @return The current view.
    [[nodiscard]] ClusterSnapshot snapshot() const;

    /// @brief Blocks until the view differs from @p lastVersion, the monitor is
    /// stopped, or @p timeout elapses (then `version == lastVersion`).
    [[nodiscard]] ClusterUpdate waitForChange(std::uint64_t lastVersion,
                                              std::chrono::milliseconds timeout) const;
};

} // namespace RaftKV::Net
