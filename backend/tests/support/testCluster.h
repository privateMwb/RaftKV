#pragma once

// An in-process N-node RaftKV cluster over real loopback HTTP: every node is a
// real KvNode (real WAL, real term/vote file) behind a real NodeServer. Used by
// the cluster integration suites; nodes can be killed and restarted in place.

// clang-format off
#include "../../src/net/NodeServer.h" // RaftKV::Net::NodeServer
#include "../../src/node/KvNode.h"    // RaftKV::Node::KvNode
#include "tempDir.h"                  // TempDir - per-cluster data root

#include <chrono>   // std::chrono
#include <memory>   // std::unique_ptr
#include <optional> // std::optional
#include <string>   // std::string
#include <thread>   // std::this_thread::sleep_for
#include <vector>   // std::vector
// clang-format on

class TestCluster {
  public:
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::milliseconds;

    /// Client-API settings for tests: rate limiting off unless a test asks.
    static RaftKV::Net::ClientApiConfig defaultApi() {
        RaftKV::Net::ClientApiConfig config;
        config.rateLimitRequests = 0;
        return config;
    }

    /// @param size Number of nodes.
    /// @param basePort Node i listens on basePort + i.
    /// @param api Client API limits/timeouts every node's server uses.
    /// @param publicHost If set, every peer is also given a `publicUrl` on this host
    /// (e.g. "localhost"), so tests can tell a redirect built from it apart from one
    /// built from the peer address (127.0.0.1).
    TestCluster(int size, uint16_t basePort, RaftKV::Net::ClientApiConfig api = defaultApi(),
                std::string publicHost = std::string())
        : basePort_(basePort), publicHost_(std::move(publicHost)) {
        serverOptions_.threadCount = 8; // room for SSE streams (cap = half) plus RPC/client traffic
        serverOptions_.api = api;
        serverOptions_.monitor.pollInterval = Ms(100); // snappy cluster view without loading a 1-core box
        // Loopback answers in milliseconds, and a peer that is mid-shutdown may
        // accept a poll and never answer it: short timeouts keep teardown fast.
        serverOptions_.monitor.connectTimeout = Ms(50);
        serverOptions_.monitor.ioTimeout = Ms(100);
        for (int i = 0; i < size; ++i) {
            Slot slot;
            slot.config = configFor(i, size);
            slots_.push_back(std::move(slot));
        }
    }

    ~TestCluster() {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            kill(static_cast<int>(i));
    }

    /// @brief Starts every node. @return false if any failed to open or bind.
    bool startAll() {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (!start(static_cast<int>(i)))
                return false;
        return true;
    }

    bool start(int i) {
        Slot& slot = slots_[i];
        slot.node = std::make_unique<RaftKV::Node::KvNode>(slot.config);
        if (slot.node->open() != RaftCore::Status::OK)
            return false;

        slot.server = std::make_unique<RaftKV::Net::NodeServer>(*slot.node, portOf(i), serverOptions_);
        if (!slot.server->start())
            return false;

        slot.node->start();
        return true;
    }

    /// @brief Simulates the process dying: server first, then the node.
    void kill(int i) {
        Slot& slot = slots_[i];
        if (slot.server)
            slot.server->stop();
        slot.server.reset();
        if (slot.node)
            slot.node->stop();
        slot.node.reset();
    }

    /// @brief Server settings every node uses; change before the node starts.
    [[nodiscard]] RaftKV::Net::NodeServerOptions& serverOptions() {
        return serverOptions_;
    }

    [[nodiscard]] bool running(int i) const {
        return slots_[i].node != nullptr;
    }

    [[nodiscard]] RaftKV::Node::KvNode& node(int i) {
        return *slots_[i].node;
    }

    [[nodiscard]] uint16_t portOf(int i) const {
        return static_cast<uint16_t>(basePort_ + i);
    }

    [[nodiscard]] int size() const {
        return static_cast<int>(slots_.size());
    }

    /// @return Index of the running node that is Leader in the highest term,
    /// or -1 if none is.
    [[nodiscard]] int leader() {
        int best = -1;
        RaftCore::Term bestTerm = 0;
        for (int i = 0; i < size(); ++i) {
            if (!running(i) || node(i).role() != RaftCore::Role::Leader)
                continue;
            const RaftCore::Term term = node(i).term();
            if (best < 0 || term > bestTerm) {
                best = i;
                bestTerm = term;
            }
        }
        return best;
    }

    /// @brief Polls until some node is Leader. @return its index, or -1 on timeout.
    int waitForLeader(Ms timeout = Ms(5000), int exclude = -1) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            const int found = leader();
            if (found >= 0 && found != exclude)
                return found;
            std::this_thread::sleep_for(Ms(10));
        }
        return -1;
    }

    /// @brief put() via whichever node is Leader, retrying through elections.
    RaftKV::Node::SubmitResult putViaLeader(const std::string& key, const std::string& value,
                                            Ms timeout = Ms(8000)) {
        return retryWrite(timeout, [&](RaftKV::Node::KvNode& n) { return n.put(key, value, Ms(1500)); });
    }

    RaftKV::Node::SubmitResult delViaLeader(const std::string& key, Ms timeout = Ms(8000)) {
        return retryWrite(timeout, [&](RaftKV::Node::KvNode& n) { return n.del(key, Ms(1500)); });
    }

    /// @brief Waits until node i reads `value` for `key` (nullopt = absent).
    bool waitForValue(int i, const std::string& key, const std::optional<std::string>& value,
                      Ms timeout = Ms(5000)) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (node(i).get(key) == value)
                return true;
            std::this_thread::sleep_for(Ms(10));
        }
        return node(i).get(key) == value;
    }

    /// @brief waitForValue() on every running node.
    bool waitForValueEverywhere(const std::string& key, const std::optional<std::string>& value,
                                Ms timeout = Ms(5000)) {
        for (int i = 0; i < size(); ++i)
            if (running(i) && !waitForValue(i, key, value, timeout))
                return false;
        return true;
    }

  private:
    struct Slot {
        RaftKV::Node::KvNodeConfig config;
        std::unique_ptr<RaftKV::Node::KvNode> node;
        std::unique_ptr<RaftKV::Net::NodeServer> server;
    };

    template <typename Write>
    RaftKV::Node::SubmitResult retryWrite(Ms timeout, Write write) {
        const auto deadline = Clock::now() + timeout;
        RaftKV::Node::SubmitResult last = RaftKV::Node::SubmitResult::NotLeader;

        while (Clock::now() < deadline) {
            const int leaderIndex = leader();
            if (leaderIndex >= 0) {
                last = write(node(leaderIndex));
                if (last == RaftKV::Node::SubmitResult::Ok)
                    return last;
            }
            std::this_thread::sleep_for(Ms(20));
        }
        return last;
    }

    RaftKV::Node::KvNodeConfig configFor(int i, int size) const {
        RaftKV::Node::KvNodeConfig config;
        config.nodeId = "node-" + std::to_string(i + 1);
        config.dataDir = root_.file("node-" + std::to_string(i + 1));
        config.tickInterval = Ms(5);
        config.electionTimeoutMinTicks = 20; // 100-200 ms: generous for a loaded 1-core box.
        config.electionTimeoutMaxTicks = 40;
        config.heartbeatIntervalTicks = 4;

        for (int j = 0; j < size; ++j)
            if (j != i)
                config.peers.push_back({"node-" + std::to_string(j + 1), "127.0.0.1", portOf(j),
                                        publicHost_.empty()
                                            ? std::string()
                                            : "http://" + publicHost_ + ":" + std::to_string(portOf(j)) + "/"});

        return config;
    }

    uint16_t basePort_;
    std::string publicHost_;
    RaftKV::Net::NodeServerOptions serverOptions_;
    TempDir root_;
    std::vector<Slot> slots_;
};
