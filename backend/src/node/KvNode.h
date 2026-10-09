/**
 * @file            KvNode.h
 *
 * @date            2026-10-7
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
#include "../kv/KvCommand.h"                // RaftKV::Kv::KvCommand - what submit() replicates
#include "../kv/KvStateMachine.h"           // RaftKV::Kv::KvStateMachine - the replicated map
#include "NodeSnapshot.h"                   // RaftKV::Node::NodeSnapshot - observable state
#include "../raft/FilePersistentState.h"    // RaftKV::Raft::FilePersistentState - durable term/vote
#include "../raft/WalStorage.h"             // RaftKV::Raft::WalStorage - durable log
#include "../net/HttpTransport.h"             // RaftKV::Net::HttpTransport, PeerAddress - peer RPC

#include <RaftCore/RaftCore.h>              // RaftCore::RaftNode, NodeState, SystemRandomSource, Role

#include <atomic>             // std::atomic - running flag, tick error
#include <chrono>             // std::chrono::milliseconds
#include <condition_variable> // std::condition_variable - waiting for apply / leadership
#include <cstddef>            // std::size_t
#include <cstdint>            // std::uint64_t - state version
#include <memory>             // std::unique_ptr
#include <mutex>              // std::mutex
#include <optional>           // std::optional
#include <string>             // std::string
#include <thread>             // std::thread - the tick loop
#include <unordered_map>      // std::unordered_map - per-peer leadership acks
#include <vector>             // std::vector - peer list
// clang-format on

namespace RaftKV::Node {

/// @brief Static configuration for one node.
struct KvNodeConfig {
    /// This node's identity; also names its files under dataDir.
    std::string nodeId = "node-1";
    /// Directory for `<nodeId>.wal` and `<nodeId>.state`. Created if missing.
    std::string dataDir = "data";
    /// Wall-clock length of one RaftNode tick.
    std::chrono::milliseconds tickInterval{10};
    /// Election timeout range, in ticks (default: 150-300 ms at a 10 ms tick).
    int electionTimeoutMinTicks = 15;
    int electionTimeoutMaxTicks = 30;
    /// Leader heartbeat interval, in ticks. Keep well below the election minimum.
    int heartbeatIntervalTicks = 5;
    /// Every *other* node in the cluster. Empty means a cluster of one.
    std::vector<Net::PeerAddress> peers;
};

/// @brief Outcome of a write submitted through KvNode.
enum class SubmitResult {
    Ok,              ///< Committed and applied.
    NotLeader,       ///< This node isn't Leader; nothing was appended.
    InvalidArgument, ///< Empty key.
    Timeout,         ///< Appended, but not applied within the timeout. The write may still commit.
    LostLeadership,  ///< Leadership changed mid-write and the entry did not survive.
    StorageError,    ///< The log or its replication failed while appending.
};

/// @brief Outcome of a linearizable read.
enum class ReadStatus {
    Ok,        ///< `value` is authoritative (a missing key is `Ok` with no value).
    NotLeader, ///< This node isn't (or just stopped being) Leader.
    Timeout,   ///< Couldn't confirm leadership with a majority in time.
};

/// @brief Result of KvNode::readLinearizable().
struct ReadResult {
    ReadStatus status = ReadStatus::NotLeader;
    std::optional<std::string> value;
};

/// @brief A NodeSnapshot plus the version at which it was taken (see KvNode::waitForChange).
struct StateUpdate {
    std::uint64_t version = 0;
    NodeSnapshot snapshot;
};

/// @brief Human-readable name for a SubmitResult.
[[nodiscard]] const char* toString(SubmitResult result) noexcept;

/**
 * @brief One RaftKV node: RaftCore + WAL-backed log + durable term/vote + KV map.
 *
 * @details All RaftNode access -- the tick loop, client writes, incoming peer
 * RPCs (handleRequestVote/handleAppendEntries), and reply deliveries from the
 * HttpTransport's sender threads -- is serialized by a single mutex, which
 * keeps RaftCore's not-thread-safe contract; reads of the map bypass it and use
 * the state machine's own reader lock. The cluster is static: peers come from
 * the config and never change.
 * A write blocks its caller until the entry is applied, so a client that gets
 * `Ok` knows the write is durable and visible to get().
 *
 * @note get() reads this node's local state, which is only guaranteed
 * up to date on a Leader that has applied its own term's no-op. Linearizable
 * reads are a Phase 3 concern.
 */
class KvNode {
  private:
    KvNodeConfig config_;

    Raft::FilePersistentState persistent_;
    Raft::WalStorage storage_;
    RaftCore::SystemRandomSource random_;
    Kv::KvStateMachine machine_;

    // Declared before transport_: the transport's threads call back into
    // these, so they must be destroyed after it.
    mutable std::mutex mutex_;              ///< Guards state_/raft_ and everything they touch.
    std::condition_variable condition_;     ///< Signalled after every tick, apply, and RPC.
    std::optional<std::string> knownLeader_; ///< Leader seen via AppendEntries (guarded by mutex_).
    RaftCore::Term knownLeaderTerm_ = RaftCore::kNoTerm;

    /// Latest AppendEntries reply per peer that recognized us as leader
    /// (guarded by mutex_). Feeds linearizable reads.
    struct LeaderAck {
        RaftCore::Term term = RaftCore::kNoTerm;
        std::chrono::steady_clock::time_point sentAt;
    };
    std::unordered_map<std::string, LeaderAck> leaderAcks_;

    // Observable state, for SSE streams (all guarded by mutex_). The version
    // only moves when the snapshot actually differs from the last one.
    mutable std::condition_variable stateCondition_;
    std::uint64_t stateVersion_ = 0;
    NodeSnapshot lastSnapshot_;

    Net::HttpTransport transport_;
    std::unique_ptr<RaftCore::NodeState> state_;
    std::unique_ptr<RaftCore::RaftNode> raft_;
    std::thread ticker_;
    std::atomic<bool> running_{false};
    std::atomic<RaftCore::Status> lastTickError_{RaftCore::Status::OK};

    [[nodiscard]] NodeSnapshot snapshotLocked() const;
    [[nodiscard]] std::optional<std::string> leaderIdLocked() const;
    void refreshSnapshotLocked();
    [[nodiscard]] SubmitResult submit(const Kv::KvCommand& command, std::chrono::milliseconds timeout);
    void tickLoop();

  public:
    explicit KvNode(KvNodeConfig config);
    ~KvNode();

    KvNode(const KvNode&) = delete;
    KvNode& operator=(const KvNode&) = delete;

    /**
     * @brief Creates dataDir, loads durable state, opens the log, builds the RaftNode.
     * @return `OK`, or the first failure (`PARSE_ERROR` for a corrupt state
     * file, `IO_ERROR` otherwise). Must succeed before start().
     */
    [[nodiscard]] RaftCore::Status open();

    /// @brief Starts the tick loop. No-op if already running or not opened.
    void start();

    /// @brief Stops and joins the tick loop. Safe to call repeatedly.
    void stop();

    /// @brief Blocks until this node is Leader, or the timeout elapses.
    [[nodiscard]] bool waitUntilLeader(std::chrono::milliseconds timeout);

    [[nodiscard]] SubmitResult put(const std::string& key, const std::string& value,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(2));
    [[nodiscard]] SubmitResult del(const std::string& key,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(2));

    /// @brief Reads this node's *local* map: fast, any node, but possibly stale
    /// on a follower or a just-deposed leader.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    /**
     * @brief Linearizable read, served by the Leader only (Raft's ReadIndex).
     *
     * @details Succeeds only after (1) this leader has committed an entry of its
     * own term, so its commit index is current, (2) a majority of the cluster
     * (itself included) has acknowledged an AppendEntries *sent after the read
     * began*, proving no newer leader exists, and (3) its state machine has
     * applied everything up to the commit index captured at the start. The
     * result therefore reflects every write acknowledged before the read
     * started. Latency is about one heartbeat interval plus a round trip.
     *
     * @return `NotLeader` if not Leader at the start or leadership is lost while
     * waiting; `Timeout` if confirmation doesn't arrive in time (e.g. no quorum).
     */
    [[nodiscard]] ReadResult readLinearizable(const std::string& key,
                                              std::chrono::milliseconds timeout = std::chrono::seconds(2));

    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] RaftCore::Role role() const;
    [[nodiscard]] RaftCore::Term term() const;
    [[nodiscard]] RaftCore::LogIndex commitIndex() const;
    /// @return RaftCore's lastApplied: includes election no-ops, which the
    /// state machine itself never sees.
    [[nodiscard]] RaftCore::LogIndex lastApplied() const;
    [[nodiscard]] RaftCore::LogIndex lastLogIndex() const;

    /// @brief Answers a peer's RequestVote RPC (called by NodeServer).
    [[nodiscard]] RaftCore::Status handleRequestVote(const RaftCore::RequestVoteArgs& args,
                                                     RaftCore::RequestVoteReply& reply);

    /// @brief Answers a peer's AppendEntries RPC (called by NodeServer).
    [[nodiscard]] RaftCore::Status handleAppendEntries(RaftCore::AppendEntriesArgs args,
                                                       RaftCore::AppendEntriesReply& reply);

    [[nodiscard]] const std::string& nodeId() const noexcept {
        return config_.nodeId;
    }

    /// @return Who this node believes leads the current term: itself if Leader,
    /// else the sender of the latest accepted AppendEntries for this term;
    /// `std::nullopt` if unknown (e.g. mid-election).
    [[nodiscard]] std::optional<std::string> leaderId() const;

    /// @return This node's current state, freshly computed.
    [[nodiscard]] NodeSnapshot snapshot() const;

    /**
     * @brief Blocks until the node's observable state differs from the version
     * the caller last saw, or @p timeout elapses.
     * @param lastVersion The version from the previous StateUpdate (0 on the
     * first call, which returns immediately).
     * @return The current update. On timeout `version == lastVersion`, so the
     * caller can use the timeout as a keep-alive tick. Push-based: this never
     * polls; it sleeps on a condition variable that every state change signals.
     */
    [[nodiscard]] StateUpdate waitForChange(std::uint64_t lastVersion,
                                            std::chrono::milliseconds timeout) const;

    /// @brief Wakes every thread blocked in waitForChange() so it can re-check
    /// its own stop condition (the version is unchanged, so they see "no news").
    void wakeListeners() const;

    /// @return The other nodes in the cluster, as configured.
    [[nodiscard]] const std::vector<Net::PeerAddress>& peers() const noexcept {
        return config_.peers;
    }

    /// @return The configured address of the current leader (for redirects),
    /// or `std::nullopt` if there is no known leader or it is this node.
    [[nodiscard]] std::optional<Net::PeerAddress> leaderAddress() const;

    /// @return The most recent non-OK result from tick(), or OK.
    [[nodiscard]] RaftCore::Status lastTickError() const noexcept;
};

} // namespace RaftKV::Node
