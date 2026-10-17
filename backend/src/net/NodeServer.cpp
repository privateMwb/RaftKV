/**
 * @file NodeServer.cpp
 * @brief NodeServer implementation.
 */

// ============================================================
// Implementation for RaftKV::Net::NodeServer.
// ============================================================
//
//  Sections:
//   1. Helpers
//   2. Routes
//   3. Event Streams
//   4. Lifecycle
//
// ============================================================

// clang-format off
#include "NodeServer.h"      // NodeServer — the class this file implements

#include "RpcCodec.h"        // decode*(), encode(), RPC paths
#include "../node/KvNode.h"  // RaftKV::Node::KvNode - the node whose state the routes read and drive

#include <FalconHTTP/Config/ServerConfig.h> // ServerConfig - thread and stream limits

#include <chrono>            // std::chrono - stream wait/keep-alive timing
#include <string>            // std::string, std::to_string
// clang-format on

namespace RaftKV::Net {

using FalconHTTP::HTTP::HttpRequest;
using FalconHTTP::HTTP::HttpResponse;
using FalconHTTP::HTTP::HttpStatus;

// ============================================================
//  Section 1 — Helpers
// ============================================================

namespace {

void respondBinary(HttpResponse& response, std::string body) {
    response.setStatus(HttpStatus::Ok);
    response.setHeader("Content-Type", "application/octet-stream");
    response.setBody(std::move(body));
}

} // namespace

// ============================================================
//  Section 2 — Routes
// ============================================================

namespace {
FalconHTTP::Config::ServerConfig makeServerConfig(const NodeServerOptions& options) {
    FalconHTTP::Config::ServerConfig config;
    config.threadCount = options.threadCount;
    config.maxStreamingConnections = options.maxStreamingConnections;
    return config;
}
} // namespace

NodeServer::NodeServer(Node::KvNode& node, uint16_t port, NodeServerOptions options)
    : node_(node), router_(), options_(std::move(options)), api_(node, options_.api),
      monitor_(node, options_.monitor), port_(port), server_(router_, makeServerConfig(options_)) {
    registerRoutes();

    // Order matters: CORS first so its headers (and OPTIONS preflight answers)
    // apply to everything, streams included; then /kv rate limiting.
    if (!options_.corsOrigin.empty())
        server_.use(FalconHTTP::Middleware::Cors(options_.corsOrigin));

    server_.use([this](HttpRequest& request, HttpResponse& response,
                       const FalconHTTP::Middleware::NextHandler& next) {
        api_.rateLimit(request, response, next);
    });
}

void NodeServer::registerRoutes() {
    Node::KvNode* node = &node_;

    router_.post(kRequestVotePath, [node](const HttpRequest& request, HttpResponse& response) {
        const auto args = decodeRequestVoteArgs(request.body());
        if (!args) {
            response.setStatus(HttpStatus::BadRequest);
            return;
        }

        RaftCore::RequestVoteReply reply;
        if (node->handleRequestVote(*args, reply) != RaftCore::Status::OK) {
            response.setStatus(HttpStatus::InternalServerError);
            return;
        }

        respondBinary(response, encode(reply));
    });

    router_.post(kAppendEntriesPath, [node](const HttpRequest& request, HttpResponse& response) {
        auto args = decodeAppendEntriesArgs(request.body());
        if (!args) {
            response.setStatus(HttpStatus::BadRequest);
            return;
        }

        RaftCore::AppendEntriesReply reply;
        if (node->handleAppendEntries(std::move(*args), reply) != RaftCore::Status::OK) {
            response.setStatus(HttpStatus::InternalServerError);
            return;
        }

        respondBinary(response, encode(reply));
    });

    api_.registerRoutes(router_);

    router_.get("/status", [node](const HttpRequest&, HttpResponse& response) {
        response.setStatus(HttpStatus::Ok);
        response.setHeader("Content-Type", "application/json");
        response.setBody(Node::toJson(node->snapshot()));
    });

    router_.get("/cluster", [this](const HttpRequest&, HttpResponse& response) {
        response.setStatus(HttpStatus::Ok);
        response.setHeader("Content-Type", "application/json");
        response.setBody(toJson(monitor_.snapshot()));
    });

    router_.stream("/events",
                   [this](const HttpRequest&, FalconHTTP::Streaming::SseConnection& sse) {
                       streamNodeState(sse);
                   });

    router_.stream("/cluster/events",
                   [this](const HttpRequest&, FalconHTTP::Streaming::SseConnection& sse) {
                       streamClusterState(sse);
                   });
}

// ============================================================
//  Section 3 — Event Streams
// ============================================================

// Both streams follow the same shape: send the current state at once, then one
// event per change, with a `ping` whenever nothing has been sent for
// streamKeepAlive. The 250 ms wait granularity bounds how long a stream takes to
// notice stop(); a failed send is how a vanished client is detected.

void NodeServer::streamNodeState(FalconHTTP::Streaming::SseConnection& sse) {
    std::uint64_t version = 0;
    auto lastSent = std::chrono::steady_clock::now();

    while (!stopping_.load() && sse.isConnected()) {
        const Node::StateUpdate update =
            node_.waitForChange(version, std::chrono::milliseconds(250));
        const auto now = std::chrono::steady_clock::now();

        if (update.version != version) {
            version = update.version;
            if (!sse.send("state", Node::toJson(update.snapshot)))
                return;
            lastSent = now;
        } else if (now - lastSent >= options_.streamKeepAlive) {
            if (!sse.send("ping", ""))
                return;
            lastSent = now;
        }
    }
}

void NodeServer::streamClusterState(FalconHTTP::Streaming::SseConnection& sse) {
    std::uint64_t version = 0;
    auto lastSent = std::chrono::steady_clock::now();

    while (!stopping_.load() && sse.isConnected()) {
        const ClusterUpdate update =
            monitor_.waitForChange(version, std::chrono::milliseconds(250));
        const auto now = std::chrono::steady_clock::now();

        if (update.version != version) {
            version = update.version;
            if (!sse.send("cluster", toJson(update.snapshot)))
                return;
            lastSent = now;
        } else if (now - lastSent >= options_.streamKeepAlive) {
            if (!sse.send("ping", ""))
                return;
            lastSent = now;
        }
    }
}

// ============================================================
//  Section 4 — Lifecycle
// ============================================================

NodeServer::~NodeServer() {
    stop();
}

bool NodeServer::start() {
    if (running_)
        return true;

    stopping_.store(false);
    monitor_.start();

    if (!server_.start(port_)) {
        monitor_.stop();
        return false;
    }

    running_ = true;
    thread_ = std::thread([this]() { server_.run(); });
    return true;
}

void NodeServer::stop() {
    if (!running_)
        return;

    running_ = false;
    stopping_.store(true);

    // Wake every open stream at once instead of letting each notice on its own
    // wait timeout: node streams sleep on the node's condition variable, cluster
    // streams on the monitor's (stopping the monitor signals it).
    node_.wakeListeners();
    monitor_.stop();

    server_.stop();
    if (thread_.joinable())
        thread_.join();
}

} // namespace RaftKV::Net
