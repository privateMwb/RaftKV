/**
 * @file            NodeServer.h
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
#include "ClientApi.h"       // RaftKV::Net::ClientApi, ClientApiConfig - the /kv routes
#include "ClusterMonitor.h"  // RaftKV::Net::ClusterMonitor, MonitorOptions - the live cluster view

#include <FalconHTTP/FalconHTTP.h> // FalconHTTP::Routing::Router, FalconHTTP::Core::Server

#include <atomic>   // std::atomic - stream shutdown flag
#include <chrono>   // std::chrono::milliseconds
#include <cstddef>  // std::size_t - worker thread count
#include <cstdint>  // uint16_t - listen port
#include <string>   // std::string - CORS origin
#include <thread>   // std::thread - the accept loop
// clang-format on

namespace RaftKV::Node {
class KvNode;
}

namespace RaftKV::Net {

/// @brief Everything configurable about a NodeServer.
struct NodeServerOptions {
    /// Connection-handling threads. Each open SSE stream pins one of them for as
    /// long as it stays connected, so size this for streams *plus* peer RPC and
    /// client requests.
    std::size_t threadCount = 8;
    /// Most concurrent SSE streams (further ones get 503). 0 means half of
    /// threadCount, leaving the other half for RPC and client traffic.
    std::size_t maxStreamingConnections = 0;
    ClientApiConfig api;
    MonitorOptions monitor;
    /// If non-empty, sent as `Access-Control-Allow-Origin` on every response
    /// (streams included) and OPTIONS preflights are answered. Needed when the
    /// dashboard is served from a different origin than the node; `*` allows any.
    std::string corsOrigin;
    /// How often an idle SSE stream sends a `ping` event. Doubles as the time
    /// to notice a vanished client and release its thread.
    std::chrono::milliseconds streamKeepAlive{2000};
};

/**
 * @brief The node's HTTP server: peer-to-peer Raft RPC, the client `/kv/:key`
 * API (see ClientApi), and the live observability endpoints.
 *
 * @details Routes:
 *   - `POST /raft/request-vote`   binary RequestVoteArgs in, RequestVoteReply out
 *   - `POST /raft/append-entries` binary AppendEntriesArgs in, AppendEntriesReply out
 *   - `GET  /status`              this node's state as JSON (one NodeSnapshot)
 *   - `GET  /cluster`             this node's view of the whole cluster as JSON
 *   - `GET  /events`              SSE: this node's own state, `state` event on every change
 *   - `GET  /cluster/events`      SSE: the cluster view, `cluster` event on every change
 *   - `/kv/...`                   see ClientApi
 * A body that doesn't decode is `400`; a RaftCore failure while handling is `500`.
 *
 * The SSE streams are push-based: a new stream receives the current state
 * immediately, then one event per real change (nothing is sent while nothing
 * changes except a `ping` every `streamKeepAlive`). `/cluster/events` reports
 * the observer's own changes instantly and peers' within one monitor poll
 * interval (see ClusterMonitor); open it on whichever node is alive.
 *
 * Peer RPC, client traffic, and streams share one port; later phases add
 * routes via router() before start().
 *
 * @attention Stop the server *before* stopping its KvNode when simulating a
 * node going down: a stopped KvNode still answers RPCs if its server is up.
 */
class NodeServer {
  private:
    Node::KvNode& node_;
    FalconHTTP::Routing::Router router_;
    NodeServerOptions options_;
    FalconHTTP::Core::Server server_;
    ClientApi api_;
    ClusterMonitor monitor_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    uint16_t port_;
    bool running_ = false;

    void registerRoutes();
    void streamNodeState(FalconHTTP::Streaming::SseConnection& sse);
    void streamClusterState(FalconHTTP::Streaming::SseConnection& sse);

  public:
    /// @param node The node whose state and RPCs this server serves; must outlive it.
    /// @param port TCP port to listen on.
    NodeServer(Node::KvNode& node, uint16_t port, NodeServerOptions options = NodeServerOptions());
    ~NodeServer();

    NodeServer(const NodeServer&) = delete;
    NodeServer& operator=(const NodeServer&) = delete;

    /// @brief Routes added here before start() are served alongside the built-in ones.
    [[nodiscard]] FalconHTTP::Routing::Router& router() noexcept {
        return router_;
    }

    /// @brief Binds the port, starts the cluster monitor, and begins serving.
    /// @return false if the bind failed.
    [[nodiscard]] bool start();

    /// @brief Ends open streams, stops accepting, and joins the server thread.
    /// Idempotent.
    void stop();
};

} // namespace RaftKV::Net
