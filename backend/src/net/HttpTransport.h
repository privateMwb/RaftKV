/**
 * @file            HttpTransport.h
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
#include "PeerAddress.h"                    // RaftKV::Net::PeerAddress

#include <RaftCore/Interfaces/Transport.h>  // RaftCore::Transport (the interface implemented here)
#include <FalconHTTP/Client/HttpClient.h>   // FalconHTTP::Client::HttpClient - outbound RPC calls

#include <chrono>             // std::chrono::milliseconds, steady_clock - timeouts and send timestamps
#include <condition_variable> // std::condition_variable - waking a peer's sender thread
#include <cstddef>            // std::size_t - queue bound
#include <deque>              // std::deque - per-peer pending sends
#include <functional>         // std::function - the locked-reply runner
#include <memory>             // std::unique_ptr - one sender per peer
#include <mutex>              // std::mutex
#include <string>             // std::string - peer id
#include <thread>             // std::thread - one sender thread per peer
#include <unordered_map>      // std::unordered_map - peer id -> sender
#include <vector>             // std::vector - peer list
// clang-format on

namespace RaftKV::Net {

/// @brief Tuning for HttpTransport. Defaults suit a LAN/loopback cluster
/// with a 150-300 ms election timeout.
struct TransportOptions {
    /// Give up establishing a connection to a peer after this long.
    std::chrono::milliseconds connectTimeout{100};
    /// Give up waiting on any single send/receive to a peer after this long,
    /// for a small message. Larger bodies get extra time (see perKiBTimeout):
    /// the peer must fsync what it receives before it can reply.
    std::chrono::milliseconds ioTimeout{300};
    /// Extra I/O timeout per KiB of request body (about 1 MB/s of headroom),
    /// so an AppendEntries carrying a big entry isn't abandoned mid-fsync.
    std::chrono::milliseconds perKiBTimeout{1};
    /// Most RPCs queued for one peer. When full, the oldest is dropped:
    /// Raft treats a lost RPC like any other loss and resends on its own.
    /// Kept small on purpose: a deep queue of stale heartbeats ahead of a new
    /// entry is latency (each one costs a full round trip) for no benefit.
    std::size_t maxQueuedPerPeer = 4;
};

/**
 * @brief RaftCore::Transport over HTTP: each RPC is a POST to the peer's
 * `/raft/request-vote` or `/raft/append-entries`.
 *
 * @details RaftNode calls the transport from inside its (mutex-held) tick(),
 * and is not allowed to block there, so a send only enqueues work. Each peer
 * has its own sender thread and queue: RPCs to one peer stay in order (except
 * that a newer AppendEntries replaces an older one still waiting its turn), a slow
 * or dead peer can't delay the others, and at most one call per peer is ever
 * in flight. The sender does the blocking HTTP call, decodes the reply, and
 * hands the RaftCore reply callback to the `runner` supplied at construction,
 * which must run it under whatever lock guards the RaftNode (RaftCore's reply
 * handlers are not thread-safe).
 *
 * Any failure -- peer down, timeout, non-2xx, undecodable body -- silently
 * drops that RPC. That is the contract Raft is built on: the next heartbeat
 * or election retries.
 */
class HttpTransport final : public RaftCore::Transport {
  public:
    /// Runs @p deliver while holding the lock that serializes RaftNode access.
    using LockedRunner = std::function<void(FunctionPro::MoveOnlyFunction<void()>& deliver)>;

    /// Called (under that same lock, just *before* RaftCore sees the reply)
    /// for every decoded AppendEntries reply. @p sentAt is when the RPC was
    /// handed to the transport, a lower bound on when the peer processed it.
    using AckObserver =
        std::function<void(const RaftCore::NodeId& peer, const RaftCore::AppendEntriesReply& reply,
                           std::chrono::steady_clock::time_point sentAt)>;

  private:
    struct PeerSender {
        PeerAddress address;
        std::mutex mutex;
        std::condition_variable wake;
        /// One queued RPC. `supersedable` ones (AppendEntries) are replaced by a
        /// newer one for the same peer: the newer message carries everything the
        /// older one did, so sending both only delays the one that matters.
        struct Pending {
            FunctionPro::MoveOnlyFunction<void()> task;
            bool supersedable = false;
        };
        std::deque<Pending> queue;
        bool stopping = false;
        std::thread thread;
    };

    TransportOptions options_;
    FalconHTTP::Client::ClientOptions clientOptions_;
    LockedRunner runner_;
    AckObserver ackObserver_;
    std::unordered_map<std::string, std::unique_ptr<PeerSender>> senders_;

    [[nodiscard]] FalconHTTP::Client::HttpClient clientFor(std::size_t bodyBytes) const;
    void enqueue(const RaftCore::NodeId& target, FunctionPro::MoveOnlyFunction<void()> task,
                 bool supersedable = false);
    void senderLoop(PeerSender& sender);

  public:
    /**
     * @param peers Every other node in the cluster (not this one).
     * @param runner Executes a reply delivery under the RaftNode lock.
     * @param options Timeouts and queue bound.
     * @param ackObserver Optional hook for AppendEntries replies (see AckObserver).
     * @details Sender threads start immediately.
     */
    HttpTransport(const std::vector<PeerAddress>& peers, LockedRunner runner,
                  TransportOptions options = TransportOptions(), AckObserver ackObserver = nullptr);
    ~HttpTransport() override;

    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    /// @brief Stops and joins all sender threads; queued RPCs are discarded.
    /// Idempotent. Must not be called while holding the runner's lock.
    void stop();

    /// @return `NOT_FOUND` if @p target isn't a configured peer; otherwise `OK`
    /// (delivery is asynchronous and best-effort).
    [[nodiscard]] RaftCore::Status sendRequestVote(
        const RaftCore::NodeId& target, RaftCore::RequestVoteArgs args,
        FunctionPro::MoveOnlyFunction<void(RaftCore::RequestVoteReply)> onReply) override;

    /// @copydoc sendRequestVote
    [[nodiscard]] RaftCore::Status sendAppendEntries(
        const RaftCore::NodeId& target, RaftCore::AppendEntriesArgs args,
        FunctionPro::MoveOnlyFunction<void(RaftCore::AppendEntriesReply)> onReply) override;
};

} // namespace RaftKV::Net
