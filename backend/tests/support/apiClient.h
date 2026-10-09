#pragma once

// Tiny helpers for driving the /kv HTTP API from tests: one call, or a call
// that follows 307 redirects by hand (FalconHTTP's HttpClient never follows).

// clang-format off
#include <FalconHTTP/Client/HttpClient.h> // FalconHTTP::Client::HttpClient, ClientResponse
#include <FalconHTTP/HTTP/HttpMethod.h>  // FalconHTTP::HTTP::HttpMethod

#include <chrono>   // std::chrono - patience and retry delay
#include <cstdint>  // uint16_t
#include <optional> // std::optional
#include <string>   // std::string
#include <thread>   // std::this_thread::sleep_for
// clang-format on

namespace api {

using FalconHTTP::Client::ClientResponse;
using FalconHTTP::HTTP::HttpMethod;

/// One request to 127.0.0.1:port; no redirects followed.
inline std::optional<ClientResponse> call(HttpMethod method, uint16_t port, const std::string& path,
                                          const std::string& body = std::string()) {
    FalconHTTP::Client::ClientOptions options;
    options.ioTimeout = std::chrono::milliseconds(4000);
    return FalconHTTP::Client::HttpClient(options).send(method, "127.0.0.1", port, path, body);
}

/// Like call(), but follows up to @p hops 307 redirects (same method and body).
inline std::optional<ClientResponse> callFollowing(HttpMethod method, uint16_t port,
                                                   const std::string& path,
                                                   const std::string& body = std::string(),
                                                   int hops = 3) {
    uint16_t currentPort = port;
    std::string currentPath = path;

    for (int i = 0; i <= hops; ++i) {
        auto response = call(method, currentPort, currentPath, body);
        if (!response || response->statusCode != 307 || !response->headers.contains("location"))
            return response;

        // Location: http://127.0.0.1:PORT/path
        const std::string location = response->headers["location"];
        const std::size_t scheme = location.find("://");
        const std::size_t colon = location.find(':', scheme + 3);
        const std::size_t slash = location.find('/', scheme + 3);
        if (scheme == std::string::npos || colon == std::string::npos || slash == std::string::npos)
            return response;

        currentPort = static_cast<uint16_t>(std::stoi(location.substr(colon + 1, slash - colon - 1)));
        currentPath = location.substr(slash);
    }

    return call(method, currentPort, currentPath, body);
}

/// Like callFollowing(), but behaves like a well-mannered client: while the
/// cluster says "try again" (503 with Retry-After, e.g. a follower that has not
/// yet heard from a freshly elected leader) or does not answer, wait and retry,
/// up to @p patience. Only use it where transient unavailability is not the
/// thing under test.
inline std::optional<ClientResponse> callPatiently(HttpMethod method, uint16_t port,
                                                   const std::string& path,
                                                   const std::string& body = std::string(),
                                                   std::chrono::milliseconds patience =
                                                       std::chrono::milliseconds(4000)) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    std::optional<ClientResponse> response;

    do {
        response = callFollowing(method, port, path, body);
        if (response && response->statusCode != 503)
            return response;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);

    return response;
}

} // namespace api
