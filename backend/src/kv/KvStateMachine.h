/**
 * @file            KvStateMachine.h
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
#include <RaftCore/Common/Types.h>             // RaftCore::LogIndex
#include <RaftCore/Interfaces/StateMachine.h>  // RaftCore::StateMachine (the interface implemented here)

#include <HashMapPro/HashMap.h>                // HashMapPro::HashMap - the key -> value map
#include <VectorPro/Vector.h>                  // VectorPro::Vector - committed payloads

#include <atomic>             // std::atomic - lastApplied / malformed counters
#include <cstddef>            // std::size_t
#include <cstdint>            // std::uint8_t
#include <functional>         // std::function - applied-index hook
#include <optional>           // std::optional - get() miss
#include <shared_mutex>       // std::shared_mutex - readers vs. the apply path
#include <string>             // std::string
// clang-format on

namespace RaftKV::Kv {

/**
 * @brief The replicated key -> value map: committed log entries applied in order.
 *
 * @details apply() runs on the thread driving RaftNode; get()/size() may be
 * called from any thread (a `shared_mutex` guards the map).
 *
 * A committed entry that doesn't decode (corrupt or from a future version) is
 * counted in malformedCount() and skipped, never thrown on: the entry is
 * already committed cluster-wide, so every node must reach the same decision
 * to ignore it.
 *
 * @attention RaftCore replays every committed entry from index 1 after a
 * restart, so a KvStateMachine must be constructed empty on every process start.
 */
class KvStateMachine final : public RaftCore::StateMachine {
  private:
    mutable std::shared_mutex mutex_;
    HashMapPro::HashMap<std::string, std::string> map_;
    std::atomic<RaftCore::LogIndex> lastApplied_{RaftCore::kNoIndex};
    std::atomic<std::size_t> malformed_{0};
    std::function<void(RaftCore::LogIndex)> onApplied_;

  public:
    KvStateMachine() = default;

    void apply(RaftCore::LogIndex index, const VectorPro::Vector<std::uint8_t>& payload) override;

    /// @return The value for `key`, or `std::nullopt` if absent.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    /// @return Number of keys currently stored.
    [[nodiscard]] std::size_t size() const;

    /// @return Highest log index apply() has been called with (0 if none yet).
    [[nodiscard]] RaftCore::LogIndex lastAppliedIndex() const noexcept;

    /// @return How many committed entries were skipped as undecodable.
    [[nodiscard]] std::size_t malformedCount() const noexcept;

    /// @brief Registers a hook called (on the applying thread) after every apply().
    /// Set before the node starts; not synchronized against concurrent apply().
    void setOnApplied(std::function<void(RaftCore::LogIndex)> hook);
};

} // namespace RaftKV::Kv
