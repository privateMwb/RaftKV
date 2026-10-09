/**
 * @file KvNode.cpp
 * @brief KvNode implementation.
 */

// ============================================================
// Implementation for RaftKV::Node::KvNode.
// ============================================================
//
//  Sections:
//   1. Construction & Lifecycle
//   2. Tick Loop
//   3. Writes
//   4. Peer RPCs, Reads & Status
//
// ============================================================

// clang-format off
#include "KvNode.h"       // KvNode — the class this file implements

#include <filesystem>          // std::filesystem::create_directories - dataDir
#include <utility>             // std::move
// clang-format on

namespace RaftKV::Node {

using namespace RaftCore;

const char* toString(SubmitResult result) noexcept {
    switch (result) {
    case SubmitResult::Ok:
        return "ok";
    case SubmitResult::NotLeader:
        return "not leader";
    case SubmitResult::InvalidArgument:
        return "invalid argument";
    case SubmitResult::Timeout:
        return "timeout";
    case SubmitResult::LostLeadership:
        return "lost leadership";
    case SubmitResult::StorageError:
        return "storage error";
    }
    return "unknown";
}

// ============================================================
//  Section 1 — Construction & Lifecycle
// ============================================================

KvNode::KvNode(KvNodeConfig config)
    : config_(std::move(config))
    , persistent_(config_.dataDir + "/" + config_.nodeId + ".state")
    , storage_(config_.dataDir + "/" + config_.nodeId + ".wal")
    , transport_(
          config_.peers,
          [this](FunctionPro::MoveOnlyFunction<void()>& deliver) {
              {
                  std::lock_guard lock(mutex_);
                  deliver();
                  refreshSnapshotLocked();
              }
              condition_.notify_all(); // A reply may have committed a write or changed our role.
          },
          Net::TransportOptions(),
          // Runs under mutex_, before RaftNode sees the reply. A peer whose
          // reply carries our own term has accepted us as its leader.
          [this](const NodeId& peer, const AppendEntriesReply& reply,
                 std::chrono::steady_clock::time_point sentAt) {
              if (state_ && state_->role() == Role::Leader && reply.term == state_->currentTerm())
                  leaderAcks_[peer] = LeaderAck{reply.term, sentAt};
          }) {
    // Registered before any thread exists, so the hook needs no synchronization.
    machine_.setOnApplied([this](LogIndex) { condition_.notify_all(); });
}

KvNode::~KvNode() {
    stop();
}

Status KvNode::open() {
    std::error_code error;
    std::filesystem::create_directories(config_.dataDir, error);
    if (error)
        return Status::IO_ERROR;

    if (Status status = persistent_.open(); status != Status::OK)
        return status;

    if (Status status = storage_.open(); status != Status::OK)
        return status;

    state_ = std::make_unique<NodeState>(persistent_, storage_, config_.nodeId);
    Vector<NodeId> peerIds;
    for (const Net::PeerAddress& peer : config_.peers)
        peerIds.push_back(peer.id);

    raft_ = std::make_unique<RaftNode>(*state_, transport_, random_, peerIds,
                                       config_.electionTimeoutMinTicks,
                                       config_.electionTimeoutMaxTicks,
                                       config_.heartbeatIntervalTicks);
    raft_->setStateMachine(&machine_);

    {
        std::lock_guard lock(mutex_);
        refreshSnapshotLocked(); // Version 1: the freshly opened state.
    }
    return Status::OK;
}

void KvNode::start() {
    if (!raft_ || running_.exchange(true))
        return;

    ticker_ = std::thread([this]() { tickLoop(); });
}

void KvNode::stop() {
    if (!running_.exchange(false))
        return;

    condition_.notify_all();
    if (ticker_.joinable())
        ticker_.join();

    transport_.stop(); // After the ticker: nothing new gets enqueued.
}

// ============================================================
//  Section 2 — Tick Loop
// ============================================================

void KvNode::tickLoop() {
    auto next = std::chrono::steady_clock::now();

    while (running_.load()) {
        next += config_.tickInterval;

        {
            std::lock_guard lock(mutex_);
            if (Status status = raft_->tick(); status != Status::OK)
                lastTickError_.store(status);
            refreshSnapshotLocked();
        }
        condition_.notify_all(); // Role/term may have changed; wake waiters.

        // If this thread was stalled (typically waiting on mutex_ while an RPC
        // handler or write held it through a slow fsync), do NOT replay the
        // missed ticks in a burst: the node could not have processed a
        // heartbeat during the stall either, so a burst would make its election
        // timer fire spuriously the moment the stall ends. Resynchronize instead.
        const auto now = std::chrono::steady_clock::now();
        if (next < now)
            next = now;

        std::this_thread::sleep_until(next);
    }
}

bool KvNode::waitUntilLeader(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this]() { return state_->role() == Role::Leader; });
}

// ============================================================
//  Section 3 — Writes
// ============================================================

SubmitResult KvNode::put(const std::string& key, const std::string& value,
                         std::chrono::milliseconds timeout) {
    return submit(Kv::KvCommand{Kv::KvOp::Put, key, value}, timeout);
}

SubmitResult KvNode::del(const std::string& key, std::chrono::milliseconds timeout) {
    return submit(Kv::KvCommand{Kv::KvOp::Delete, key, std::string()}, timeout);
}

SubmitResult KvNode::submit(const Kv::KvCommand& command, std::chrono::milliseconds timeout) {
    if (command.key.empty())
        return SubmitResult::InvalidArgument;

    std::unique_lock lock(mutex_);

    if (!state_ || state_->role() != Role::Leader)
        return SubmitResult::NotLeader;

    const Term proposedTerm = state_->currentTerm();
    LogIndex index = kNoIndex;

    if (Status status = raft_->propose(Kv::encodeCommand(command), index); status != Status::OK) {
        // propose() sets `index` only once the entry is in the log.
        if (index == kNoIndex)
            return status == Status::NOT_LEADER ? SubmitResult::NotLeader
                                                : SubmitResult::StorageError;
        // Appended but replication/apply hit an error: heartbeats retry it, so
        // fall through and wait like a normal write.
    }

    refreshSnapshotLocked(); // The new entry is in the log (and, on one node, applied).

    // Done when applied, or when leadership/term moved (the entry may be gone).
    const bool settled = condition_.wait_for(lock, timeout, [&]() {
        return state_->lastApplied() >= index || state_->role() != Role::Leader ||
               state_->currentTerm() != proposedTerm;
    });

    if (!settled)
        return SubmitResult::Timeout;

    if (state_->lastApplied() < index)
        return SubmitResult::LostLeadership;

    // Applied -- but only *our* entry counts. If another leader overwrote this
    // index, the entry now there has a different term.
    LogEntry stored;
    if (storage_.entryAt(index, stored) != Status::OK || stored.term != proposedTerm)
        return SubmitResult::LostLeadership;

    return SubmitResult::Ok;
}

// ============================================================
//  Section 4 — Peer RPCs, Reads & Status
// ============================================================

Status KvNode::handleRequestVote(const RequestVoteArgs& args, RequestVoteReply& reply) {
    Status status;
    {
        std::lock_guard lock(mutex_);
        if (!raft_)
            return Status::IO_ERROR;
        status = raft_->handleRequestVote(args, reply);
        refreshSnapshotLocked();
    }
    condition_.notify_all(); // A term bump may have demoted a waiting leader.
    return status;
}

Status KvNode::handleAppendEntries(AppendEntriesArgs args, AppendEntriesReply& reply) {
    const NodeId sender = args.leaderId;
    const Term senderTerm = args.term;

    Status status;
    {
        std::lock_guard lock(mutex_);
        if (!raft_)
            return Status::IO_ERROR;

        status = raft_->handleAppendEntries(std::move(args), reply);

        // reply.term == senderTerm means we recognized this term's leader
        // (even if its log didn't match yet): remember who it is.
        if (status == Status::OK && reply.term == senderTerm) {
            knownLeader_ = sender;
            knownLeaderTerm_ = senderTerm;
        }
        refreshSnapshotLocked();
    }
    condition_.notify_all();
    return status;
}

std::optional<Net::PeerAddress> KvNode::leaderAddress() const {
    const std::optional<std::string> leader = leaderId();
    if (!leader || *leader == config_.nodeId)
        return std::nullopt;

    for (const Net::PeerAddress& peer : config_.peers)
        if (peer.id == *leader)
            return peer;

    return std::nullopt;
}

std::optional<std::string> KvNode::leaderIdLocked() const {
    if (!state_)
        return std::nullopt;
    if (state_->role() == Role::Leader)
        return config_.nodeId;
    if (knownLeader_ && knownLeaderTerm_ == state_->currentTerm())
        return knownLeader_;

    return std::nullopt;
}

std::optional<std::string> KvNode::leaderId() const {
    std::lock_guard lock(mutex_);
    return leaderIdLocked();
}

// ---- Observable state -------------------------------------------------

NodeSnapshot KvNode::snapshotLocked() const {
    NodeSnapshot snapshot;
    snapshot.id = config_.nodeId;
    if (!state_)
        return snapshot;

    snapshot.role = state_->role();
    snapshot.term = state_->currentTerm();
    snapshot.leader = leaderIdLocked();
    snapshot.commitIndex = state_->commitIndex();
    snapshot.lastApplied = state_->lastApplied();
    snapshot.lastLogIndex = storage_.lastIndex();
    snapshot.keys = machine_.size();
    return snapshot;
}

// Called, with mutex_ held, at the end of every region that can change the
// node's state. Bumps the version only on a real difference, so listeners wake
// exactly when there is something new to say.
void KvNode::refreshSnapshotLocked() {
    NodeSnapshot current = snapshotLocked();
    if (current == lastSnapshot_ && stateVersion_ != 0)
        return;

    lastSnapshot_ = std::move(current);
    ++stateVersion_;
    stateCondition_.notify_all();
}

NodeSnapshot KvNode::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshotLocked();
}

void KvNode::wakeListeners() const {
    std::lock_guard lock(mutex_); // Take the lock so a waiter can't miss the signal between check and sleep.
    stateCondition_.notify_all();
}

StateUpdate KvNode::waitForChange(std::uint64_t lastVersion, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    stateCondition_.wait_for(lock, timeout, [&]() { return stateVersion_ != lastVersion; });
    return StateUpdate{stateVersion_, lastSnapshot_};
}

std::optional<std::string> KvNode::get(const std::string& key) const {
    return machine_.get(key); // The state machine has its own reader lock.
}

ReadResult KvNode::readLinearizable(const std::string& key, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);

    if (!state_ || state_->role() != Role::Leader)
        return {ReadStatus::NotLeader, std::nullopt};

    const Term term = state_->currentTerm();
    const std::size_t clusterSize = config_.peers.size() + 1;
    const std::size_t majority = clusterSize / 2 + 1;

    // Captured once the leader's commit index is known to be current.
    bool captured = false;
    LogIndex readIndex = kNoIndex;
    std::chrono::steady_clock::time_point startedAt;

    const bool settled = condition_.wait_for(lock, timeout, [&]() {
        if (state_->role() != Role::Leader || state_->currentTerm() != term)
            return true; // Leadership changed; classified below.

        if (!captured) {
            // A new leader's commit index can lag until an entry of its own
            // term commits (Raft section 8). Until then reads could be stale.
            LogEntry committed;
            if (state_->commitIndex() == kNoIndex ||
                storage_.entryAt(state_->commitIndex(), committed) != Status::OK ||
                committed.term != term)
                return false;

            captured = true;
            readIndex = state_->commitIndex();
            startedAt = std::chrono::steady_clock::now();
        }

        // Count ourselves plus every peer that acked an AppendEntries sent
        // after the read began.
        std::size_t confirmations = 1;
        for (const auto& entry : leaderAcks_)
            if (entry.second.term == term && entry.second.sentAt >= startedAt)
                ++confirmations;

        return confirmations >= majority && state_->lastApplied() >= readIndex;
    });

    if (!settled)
        return {ReadStatus::Timeout, std::nullopt};

    if (state_->role() != Role::Leader || state_->currentTerm() != term)
        return {ReadStatus::NotLeader, std::nullopt};

    return {ReadStatus::Ok, machine_.get(key)};
}

std::size_t KvNode::size() const {
    return machine_.size();
}

Role KvNode::role() const {
    std::lock_guard lock(mutex_);
    return state_ ? state_->role() : Role::Follower;
}

Term KvNode::term() const {
    std::lock_guard lock(mutex_);
    return state_ ? state_->currentTerm() : kNoTerm;
}

LogIndex KvNode::commitIndex() const {
    std::lock_guard lock(mutex_);
    return state_ ? state_->commitIndex() : kNoIndex;
}

LogIndex KvNode::lastApplied() const {
    std::lock_guard lock(mutex_);
    return state_ ? state_->lastApplied() : kNoIndex;
}

LogIndex KvNode::lastLogIndex() const {
    std::lock_guard lock(mutex_);
    return storage_.lastIndex();
}


Status KvNode::lastTickError() const noexcept {
    return lastTickError_.load();
}

} // namespace RaftKV::Node
