/**
 * @file ClientApi.cpp
 * @brief ClientApi implementation.
 */

// ============================================================
// Implementation for RaftKV::Net::ClientApi.
// ============================================================
//
//  Sections:
//   1. Helpers
//   2. Construction, Routes & Rate Limiting
//   3. Handlers
//
// ============================================================

// clang-format off
#include "ClientApi.h"       // ClientApi — the class this file implements

#include "PercentCoding.h"   // percentDecode - the :key path segment
#include "../node/KvNode.h"  // RaftKV::Node::KvNode, SubmitResult, ReadStatus - the node being served

#include <string>            // std::string, std::to_string
// clang-format on

namespace RaftKV::Net {

using FalconHTTP::HTTP::HttpRequest;
using FalconHTTP::HTTP::HttpResponse;
using FalconHTTP::HTTP::HttpStatus;
using Node::ReadStatus;
using Node::SubmitResult;

// ============================================================
//  Section 1 — Helpers
// ============================================================

namespace {

constexpr const char* kKvPrefix = "/kv/";

void stamp(HttpResponse& response, const Node::KvNode& node) {
    response.setHeader("X-RaftKV-Node", node.nodeId());
}

void plain(HttpResponse& response, HttpStatus status, const std::string& message) {
    response.setStatus(status);
    response.setHeader("Content-Type", "text/plain");
    response.setBody(message);
}

void unavailable(HttpResponse& response, const std::string& message) {
    plain(response, HttpStatus::ServiceUnavailable, message);
    response.setHeader("Retry-After", "1");
}

// The rate-limit key: first X-Forwarded-For entry, else one shared bucket.
std::string clientKey(const HttpRequest& request) {
    if (!request.hasHeader("X-Forwarded-For"))
        return "unknown";

    const std::string forwarded = request.header("X-Forwarded-For");
    const std::size_t comma = forwarded.find(',');
    return comma == std::string::npos ? forwarded : forwarded.substr(0, comma);
}

} // namespace

// ============================================================
//  Section 2 — Construction, Routes & Rate Limiting
// ============================================================

ClientApi::ClientApi(Node::KvNode& node, ClientApiConfig config) : node_(node), config_(config) {
    if (config_.rateLimitRequests > 0)
        limiter_ = std::make_unique<ThrottlePro::RateLimiter>(
            config_.rateLimitRequests, config_.rateLimitWindow, config_.rateLimitTrackedClients);
}

void ClientApi::registerRoutes(FalconHTTP::Routing::Router& router) {
    router.get("/kv/:key", [this](const HttpRequest& request, HttpResponse& response) {
        handleGet(request, response);
    });
    router.put("/kv/:key", [this](const HttpRequest& request, HttpResponse& response) {
        handlePut(request, response);
    });
    router.del("/kv/:key", [this](const HttpRequest& request, HttpResponse& response) {
        handleDelete(request, response);
    });
}

void ClientApi::rateLimit(HttpRequest& request, HttpResponse& response,
                          const FalconHTTP::Middleware::NextHandler& next) {
    if (limiter_ && request.path().rfind(kKvPrefix, 0) == 0 &&
        !limiter_->allow(clientKey(request))) {
        plain(response, HttpStatus::TooManyRequests, "rate limit exceeded; slow down and retry");
        response.setHeader("Retry-After", "1");
        stamp(response, node_);
        return;
    }

    next(request, response);
}

// ============================================================
//  Section 3 — Handlers
// ============================================================

void ClientApi::redirectToLeader(const HttpRequest& request, HttpResponse& response) {
    const auto leader = node_.leaderAddress();
    if (!leader) {
        unavailable(response, "no leader known; retry shortly");
        return;
    }

    // The address peers use may not be one a client can reach (proxy, container IP),
    // so prefer the node's configured public URL.
    std::string base = leader->publicUrl.empty()
                           ? "http://" + leader->host + ":" + std::to_string(leader->port)
                           : leader->publicUrl;
    while (!base.empty() && base.back() == '/')
        base.pop_back();

    // 307 (unlike 302) obliges the client to repeat the same method and body.
    response.setStatus(HttpStatus::TemporaryRedirect);
    response.setHeader("Location", base + request.path());
    response.setHeader("Content-Type", "text/plain");
    response.setBody("not the leader; redirecting to " + leader->id);
}

void ClientApi::handlePut(const HttpRequest& request, HttpResponse& response) {
    stamp(response, node_);

    const auto key = percentDecode(request.pathParam("key"));
    if (!key || key->empty() || key->size() > config_.maxKeyBytes) {
        plain(response, HttpStatus::BadRequest, "invalid key");
        return;
    }
    if (request.body().size() > config_.maxValueBytes) {
        plain(response, HttpStatus::PayloadTooLarge, "value too large");
        return;
    }

    switch (node_.put(*key, request.body(), config_.writeTimeout)) {
    case SubmitResult::Ok:
        response.setStatus(HttpStatus::NoContent);
        return;
    case SubmitResult::NotLeader:
        redirectToLeader(request, response);
        return;
    case SubmitResult::InvalidArgument:
        plain(response, HttpStatus::BadRequest, "invalid key");
        return;
    case SubmitResult::Timeout:
        unavailable(response, "timed out waiting for commit; the write may still apply");
        return;
    case SubmitResult::LostLeadership:
        unavailable(response, "leadership changed during the write; retry");
        return;
    case SubmitResult::StorageError:
        plain(response, HttpStatus::InternalServerError, "storage error");
        return;
    }
}

void ClientApi::handleDelete(const HttpRequest& request, HttpResponse& response) {
    stamp(response, node_);

    const auto key = percentDecode(request.pathParam("key"));
    if (!key || key->empty() || key->size() > config_.maxKeyBytes) {
        plain(response, HttpStatus::BadRequest, "invalid key");
        return;
    }

    switch (node_.del(*key, config_.writeTimeout)) {
    case SubmitResult::Ok:
        response.setStatus(HttpStatus::NoContent);
        return;
    case SubmitResult::NotLeader:
        redirectToLeader(request, response);
        return;
    case SubmitResult::InvalidArgument:
        plain(response, HttpStatus::BadRequest, "invalid key");
        return;
    case SubmitResult::Timeout:
        unavailable(response, "timed out waiting for commit; the delete may still apply");
        return;
    case SubmitResult::LostLeadership:
        unavailable(response, "leadership changed during the delete; retry");
        return;
    case SubmitResult::StorageError:
        plain(response, HttpStatus::InternalServerError, "storage error");
        return;
    }
}

void ClientApi::handleGet(const HttpRequest& request, HttpResponse& response) {
    stamp(response, node_);

    const auto key = percentDecode(request.pathParam("key"));
    if (!key || key->empty() || key->size() > config_.maxKeyBytes) {
        plain(response, HttpStatus::BadRequest, "invalid key");
        return;
    }

    std::optional<std::string> value;

    if (request.queryParam("stale") == "1") {
        // Explicit opt-out: this node's local map, no leadership check.
        value = node_.get(*key);
        response.setHeader("X-RaftKV-Stale", "true");
    } else {
        const Node::ReadResult result = node_.readLinearizable(*key, config_.readTimeout);

        if (result.status == ReadStatus::NotLeader) {
            redirectToLeader(request, response);
            return;
        }
        if (result.status == ReadStatus::Timeout) {
            unavailable(response, "could not confirm leadership with a majority; retry");
            return;
        }

        value = result.value;
    }

    if (!value) {
        plain(response, HttpStatus::NotFound, "not found");
        return;
    }

    response.setStatus(HttpStatus::Ok);
    response.setHeader("Content-Type", "application/octet-stream");
    response.setBody(*value);
}

} // namespace RaftKV::Net
