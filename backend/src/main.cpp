/**
 * @file main.cpp
 * @brief RaftKV backend entry point.
 *
 * Runs one node. Without `--port` it is a cluster of one (Phase 1). With
 * `--port` it serves the peer RPC endpoints and, given `--peer` entries,
 * joins a static cluster:
 *
 *   raftkv_backend --id node-1 --port 8101 --data data/node-1 \
 *       --peer node-2=127.0.0.1:8102 --peer node-3=127.0.0.1:8103
 *
 * Observability on the same port: `GET /status`, `GET /cluster`, and SSE streams
 * `GET /events` (this node) and `GET /cluster/events` (whole cluster, pushed on
 * change). `--cors-origin ORIGIN` lets a dashboard on another origin read them;
 * `--threads N` sizes the server pool (each open stream pins one thread; the
 * stream cap is half of it). `--public-url ID=URL` tells a node the address clients
 * (not peers) should use for peer ID, which followers put in their 307 redirects;
 * needed when --peer points at a proxy or a container-internal address.
 *
 * Clients use HTTP on the same port: `PUT|GET|DELETE /kv/<percent-encoded key>`
 * (followers answer 307 -> leader; `GET ...?stale=1` reads the local map) and
 * `GET /status`. `--rate-limit N` allows N /kv requests per client per 10 s
 * (default 600; 0 disables). `--max-value-bytes N` caps a value (default 64 KiB;
 * raise it only together with the election timeout, see backend/README.md).
 *
 * Commands on stdin: put <key> <value...> | get <key> | del <key> | status | quit.
 * Writes are accepted only on the leader. `--no-repl` skips stdin and runs
 * until SIGINT/SIGTERM (clean shutdown).
 */

// clang-format off
#include "net/NodeServer.h" // RaftKV::Net::NodeServer - peer RPC + /status
#include "node/KvNode.h"    // RaftKV::Node::KvNode, KvNodeConfig, SubmitResult

#include <atomic>   // std::atomic - shutdown flag
#include <charconv> // std::from_chars - port parsing
#include <chrono>   // std::chrono::seconds
#include <csignal>  // sigaction, SIGINT, SIGTERM
#include <cstdio>   // std::printf, std::fprintf, stderr
#include <iostream> // std::cin, std::getline
#include <memory>   // std::unique_ptr
#include <sstream>  // std::istringstream
#include <string>   // std::string
#include <thread>   // std::this_thread::sleep_for
#include <utility>  // std::pair - parsed --public-url entries
#include <vector>   // std::vector - those entries
// clang-format on

namespace {

std::atomic<bool> g_shutdown{false};

extern "C" void onSignal(int) {
    g_shutdown.store(true);
}

bool parsePort(const std::string& text, uint16_t& out) {
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() || value == 0 ||
        value > 65535)
        return false;

    out = static_cast<uint16_t>(value);
    return true;
}

// "node-2=127.0.0.1:8102" -> PeerAddress
bool parsePeer(const std::string& text, RaftKV::Net::PeerAddress& out) {
    const std::size_t equals = text.find('=');
    const std::size_t colon = text.rfind(':');
    if (equals == std::string::npos || colon == std::string::npos || colon < equals || equals == 0)
        return false;

    out.id = text.substr(0, equals);
    out.host = text.substr(equals + 1, colon - equals - 1);
    return !out.host.empty() && parsePort(text.substr(colon + 1), out.port);
}

void printUsage() {
    std::fprintf(stderr,
                 "usage: raftkv_backend [--id ID] [--data DIR] [--port PORT] "
                 "[--peer ID=HOST:PORT]... [--rate-limit N] [--max-value-bytes N] "
                 "[--threads N] [--cors-origin ORIGIN] [--public-url ID=URL]... [--no-repl]\n");
}

} // namespace

int main(int argc, char** argv) {
    RaftKV::Node::KvNodeConfig config;
    RaftKV::Net::NodeServerOptions serverOptions;
    std::vector<std::pair<std::string, std::string>> publicUrls; // peer id -> client URL
    uint16_t port = 0;
    bool repl = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;

        if (arg == "--id" && hasValue) {
            config.nodeId = argv[++i];
        } else if (arg == "--data" && hasValue) {
            config.dataDir = argv[++i];
        } else if (arg == "--port" && hasValue) {
            if (!parsePort(argv[++i], port)) {
                std::fprintf(stderr, "bad --port\n");
                return 2;
            }
        } else if (arg == "--peer" && hasValue) {
            RaftKV::Net::PeerAddress peer;
            if (!parsePeer(argv[++i], peer)) {
                std::fprintf(stderr, "bad --peer (expected ID=HOST:PORT): %s\n", argv[i]);
                return 2;
            }
            config.peers.push_back(std::move(peer));
        } else if (arg == "--rate-limit" && hasValue) {
            std::size_t requests = 0;
            const std::string text = argv[++i];
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), requests);
            if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
                std::fprintf(stderr, "bad --rate-limit\n");
                return 2;
            }
            serverOptions.api.rateLimitRequests = requests;
        } else if (arg == "--max-value-bytes" && hasValue) {
            std::size_t bytes = 0;
            const std::string text = argv[++i];
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), bytes);
            if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || bytes == 0) {
                std::fprintf(stderr, "bad --max-value-bytes\n");
                return 2;
            }
            serverOptions.api.maxValueBytes = bytes;
        } else if (arg == "--threads" && hasValue) {
            std::size_t threads = 0;
            const std::string text = argv[++i];
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), threads);
            if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
                threads < 2) {
                std::fprintf(stderr, "bad --threads (need at least 2)\n");
                return 2;
            }
            serverOptions.threadCount = threads;
        } else if (arg == "--cors-origin" && hasValue) {
            serverOptions.corsOrigin = argv[++i];
        } else if (arg == "--public-url" && hasValue) {
            const std::string text = argv[++i];
            const std::size_t equals = text.find('=');
            if (equals == std::string::npos || equals == 0 || equals + 1 == text.size()) {
                std::fprintf(stderr, "bad --public-url (expected ID=URL): %s\n", text.c_str());
                return 2;
            }
            publicUrls.emplace_back(text.substr(0, equals), text.substr(equals + 1));
        } else if (arg == "--no-repl") {
            repl = false;
        } else {
            printUsage();
            return 2;
        }
    }

    for (const auto& [id, url] : publicUrls) {
        bool known = false;
        for (RaftKV::Net::PeerAddress& peer : config.peers) {
            if (peer.id == id) {
                peer.publicUrl = url;
                known = true;
            }
        }
        if (!known) {
            std::fprintf(stderr, "--public-url names %s, which is not a --peer\n", id.c_str());
            return 2;
        }
    }

    if (!config.peers.empty() && port == 0) {
        std::fprintf(stderr, "--peer needs --port: peers must be able to reach this node.\n");
        return 2;
    }

    RaftKV::Node::KvNode node(config);
    if (RaftCore::Status status = node.open(); status != RaftCore::Status::OK) {
        std::fprintf(stderr, "open failed (status %d) -- check %s and its files.\n",
                     static_cast<int>(status), config.dataDir.c_str());
        return 1;
    }

    std::unique_ptr<RaftKV::Net::NodeServer> server;
    if (port != 0) {
        server = std::make_unique<RaftKV::Net::NodeServer>(node, port, serverOptions);
        if (!server->start()) {
            std::fprintf(stderr, "could not listen on port %u.\n", static_cast<unsigned>(port));
            return 1;
        }
    }

    node.start();

    if (!repl) {
        struct sigaction action {};
        action.sa_handler = onSignal;
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);

        std::printf("%s up (peers: %zu), serving until SIGINT/SIGTERM.\n", config.nodeId.c_str(),
                    config.peers.size());
        std::fflush(stdout);

        while (!g_shutdown.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
        std::printf("%s up (peers: %zu). Commands: put get del status quit\n",
                    config.nodeId.c_str(), config.peers.size());

        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream in(line);
            std::string command, key;
            in >> command >> key;

            if (command == "quit" || command == "exit") {
                break;
            } else if (command == "put" || command == "del") {
                std::string value;
                if (command == "put")
                    std::getline(in >> std::ws, value);

                const auto result = command == "put" ? node.put(key, value) : node.del(key);
                std::printf("%s", RaftKV::Node::toString(result));

                if (result == RaftKV::Node::SubmitResult::NotLeader) {
                    const auto leader = node.leaderId();
                    std::printf(" (leader: %s)", leader ? leader->c_str() : "unknown");
                }
                std::printf("\n");
            } else if (command == "get") {
                const auto value = node.get(key);
                std::printf("%s\n", value ? value->c_str() : "(not found)");
            } else if (command == "status") {
                const auto leader = node.leaderId();
                std::printf("role=%s term=%llu leader=%s keys=%zu commit=%llu applied=%llu "
                            "lastLog=%llu\n",
                            RaftKV::Node::roleName(node.role()),
                            static_cast<unsigned long long>(node.term()),
                            leader ? leader->c_str() : "?", node.size(),
                            static_cast<unsigned long long>(node.commitIndex()),
                            static_cast<unsigned long long>(node.lastApplied()),
                            static_cast<unsigned long long>(node.lastLogIndex()));
            } else if (!command.empty()) {
                std::printf("unknown command: %s\n", command.c_str());
            }
        }
    }

    // Server first: a stopped node must not keep answering peers.
    if (server)
        server->stop();
    node.stop();
    return 0;
}
