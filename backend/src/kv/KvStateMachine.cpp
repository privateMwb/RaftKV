/**
 * @file KvStateMachine.cpp
 * @brief KvStateMachine implementation.
 */

// ============================================================
// Implementation for RaftKV::Kv::KvStateMachine.
// ============================================================
//
//  Sections:
//   1. Applying Commands
//   2. Reading State
//
// ============================================================

// clang-format off
#include "KvStateMachine.h" // KvStateMachine — the class this file implements

#include "KvCommand.h"      // decodeCommand, KvOp - turning a payload back into an operation

#include <mutex>            // std::unique_lock, std::shared_lock
// clang-format on

namespace RaftKV::Kv {

// ============================================================
//  Section 1 — Applying Commands
// ============================================================

void KvStateMachine::apply(RaftCore::LogIndex index, const VectorPro::Vector<std::uint8_t>& payload) {
    const std::optional<KvCommand> command = decodeCommand(payload);

    if (!command) {
        malformed_.fetch_add(1, std::memory_order_relaxed);
    } else {
        std::unique_lock lock(mutex_);

        if (command->op == KvOp::Put)
            map_[command->key] = command->value;
        else
            (void)map_.erase(command->key); // Deleting an absent key is a no-op, not an error.
    }

    lastApplied_.store(index, std::memory_order_release);

    if (onApplied_)
        onApplied_(index);
}

// ============================================================
//  Section 2 — Reading State
// ============================================================

std::optional<std::string> KvStateMachine::get(const std::string& key) const {
    std::shared_lock lock(mutex_);

    if (!map_.contains(key))
        return std::nullopt;

    return map_.at(key);
}

std::size_t KvStateMachine::size() const {
    std::shared_lock lock(mutex_);
    return map_.size();
}

RaftCore::LogIndex KvStateMachine::lastAppliedIndex() const noexcept {
    return lastApplied_.load(std::memory_order_acquire);
}

std::size_t KvStateMachine::malformedCount() const noexcept {
    return malformed_.load(std::memory_order_relaxed);
}

void KvStateMachine::setOnApplied(std::function<void(RaftCore::LogIndex)> hook) {
    onApplied_ = std::move(hook);
}

} // namespace RaftKV::Kv
