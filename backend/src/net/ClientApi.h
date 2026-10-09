/**
 * @file            ClientApi.h
 *
 * @date            2026-10-8
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
#include <FalconHTTP/FalconHTTP.h>   // Router, HttpRequest, HttpResponse, Middleware::NextHandler
#include <ThrottlePro/RateLimiter.h> // ThrottlePro::RateLimiter - per-client request limit

#include <chrono>  // std::chrono::milliseconds
#include <cstddef> // std::size_t
#include <memory>  // std::unique_ptr
// clang-format on

namespace RaftKV::Node {
class KvNode;
}

namespace RaftKV::Net {

/// @brief Limits and timeouts for the client-facing KV API.
struct ClientApiConfig {
    /// Longest accepted key, after percent-decoding (400 beyond it).
    std::size_t maxKeyBytes = 1024;
    /// Largest accepted value (413 beyond it). Every write is fsynced by the
    /// leader and a majority *while holding the node lock*, which also pauses
    /// heartbeats; if one fsync outlasts the election timeout the cluster
    /// elects a new leader mid-write. 64 KiB keeps that fsync in the
    /// milliseconds even on slow flash. Raise it only together with the
    /// election timeout, and never past FalconHTTP's 10 MiB body limit.
    std::size_t maxValueBytes = 64 * 1024;
    /// How long a write waits to be committed and applied before 503.
    std::chrono::milliseconds writeTimeout{2000};
    /// How long a linearizable read waits for leadership confirmation before 503.
    std::chrono::milliseconds readTimeout{2000};
    /// Requests allowed per client per window on `/kv/*`; 0 disables the limiter.
    std::size_t rateLimitRequests = 600;
    std::chrono::milliseconds rateLimitWindow{10000};
    /// Distinct clients tracked before the least-recently-seen is evicted.
    std::size_t rateLimitTrackedClients = 10000;
};

/**
 * @brief The client-facing HTTP API: `GET`/`PUT`/`DELETE /kv/:key`.
 *
 * @details
 * Keys travel as one percent-encoded path segment (`/` must be `%2F`), values
 * as the raw request/response body (binary-safe).
 *
 *  | Request                  | Success            | Other outcomes |
 *  |--------------------------|--------------------|----------------|
 *  | `PUT /kv/:key` (body)    | `204`              | see below      |
 *  | `DELETE /kv/:key`        | `204` (idempotent) | see below      |
 *  | `GET /kv/:key`           | `200` + body       | `404` if absent|
 *  | `GET /kv/:key?stale=1`   | `200`/`404` from this node's local map, any node | |
 *
 * Writes and plain reads must go to the leader. A follower answers
 * `307 Temporary Redirect` with `Location` pointing at the leader (method and
 * body are preserved by 307); with no known leader it answers
 * `503` + `Retry-After: 1`. Plain `GET` is linearizable (Raft ReadIndex, see
 * KvNode::readLinearizable); `?stale=1` is the explicit opt-out. Other
 * statuses: `400` bad key, `413` value too large, `429` rate-limited,
 * `503` timeout or leadership lost mid-write (a timed-out write may still
 * commit: retry only idempotent intents), `500` storage failure.
 * Every `/kv` response carries `X-RaftKV-Node: <node id>`.
 *
 * Rate limiting uses ThrottlePro and applies to `/kv/*` only, never to peer
 * RPC or `/status`. FalconHTTP's HttpRequest has no client-address accessor,
 * so the limiter keys on the first `X-Forwarded-For` entry and, without a
 * proxy setting it, all clients share one `unknown` bucket (same limitation
 * and degradation as DnsLint).
 */
class ClientApi {
  private:
    Node::KvNode& node_;
    ClientApiConfig config_;
    std::unique_ptr<ThrottlePro::RateLimiter> limiter_;

    void handleGet(const FalconHTTP::HTTP::HttpRequest& request, FalconHTTP::HTTP::HttpResponse& response);
    void handlePut(const FalconHTTP::HTTP::HttpRequest& request, FalconHTTP::HTTP::HttpResponse& response);
    void handleDelete(const FalconHTTP::HTTP::HttpRequest& request, FalconHTTP::HTTP::HttpResponse& response);

    void redirectToLeader(const FalconHTTP::HTTP::HttpRequest& request, FalconHTTP::HTTP::HttpResponse& response);

  public:
    /// @param node The node to serve; must outlive this object.
    ClientApi(Node::KvNode& node, ClientApiConfig config = ClientApiConfig());

    ClientApi(const ClientApi&) = delete;
    ClientApi& operator=(const ClientApi&) = delete;

    /// @brief Registers the `/kv/:key` routes.
    void registerRoutes(FalconHTTP::Routing::Router& router);

    /// @brief Middleware that rate-limits `/kv/*` requests (429) and passes
    /// everything else straight through. Install with Server::use().
    void rateLimit(FalconHTTP::HTTP::HttpRequest& request, FalconHTTP::HTTP::HttpResponse& response,
                   const FalconHTTP::Middleware::NextHandler& next);
};

} // namespace RaftKV::Net
