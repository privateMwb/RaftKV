/**
 * @file            WalStorage.h
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
#include <RaftCore/Common/Status.h>        // RaftCore::Status
#include <RaftCore/Common/Types.h>         // RaftCore::Term, LogIndex, LogEntry
#include <RaftCore/Interfaces/Storage.h>   // RaftCore::Storage (the interface implemented here)

#include <MiniDB/Storage/WriteAheadLog.h>  // MiniDB::Storage::WriteAheadLog - the actual durable log

#include <VectorPro/Vector.h>              // VectorPro::Vector - payloads and entry lists
#include <cstdint>                         // std::uint8_t
#include <string>                          // std::string - file path
// clang-format on

namespace RaftKV::Raft {

/**
 * @brief RaftCore::Storage backed by MiniDB's WriteAheadLog.
 *
 * @details A thin adapter. It bridges three representation gaps and nothing
 * else: MiniDB's `Status` vs RaftCore's `Status`, `std::string` payloads vs
 * `Vector<std::uint8_t>`, and the WAL's `LogEntry` (which carries its own
 * `index`) vs RaftCore's. The WAL fsyncs every append before returning, which
 * satisfies Storage's durable-before-OK requirement; the empty-payload
 * election no-op round-trips like any other entry.
 *
 * @attention Not thread-safe; used only under KvNode's mutex. Call open()
 * before first use.
 */
class WalStorage final : public RaftCore::Storage {
  private:
    MiniDB::Storage::WriteAheadLog wal_;

  public:
    /// @param path Log file to create or resume.
    explicit WalStorage(std::string path);

    /// @brief Opens the log, recovering from a torn or corrupt trailing record.
    [[nodiscard]] RaftCore::Status open();

    [[nodiscard]] RaftCore::Status append(RaftCore::Term term,
                                          VectorPro::Vector<std::uint8_t> payload,
                                          RaftCore::LogIndex& outIndex) override;
    [[nodiscard]] RaftCore::Status entryAt(RaftCore::LogIndex index,
                                           RaftCore::LogEntry& outEntry) const override;
    [[nodiscard]] RaftCore::Status
    range(RaftCore::LogIndex fromIndex, RaftCore::LogIndex toIndex,
          VectorPro::Vector<RaftCore::LogEntry>& outEntries) const override;
    [[nodiscard]] RaftCore::Status truncateFrom(RaftCore::LogIndex index) override;
    RaftCore::LogIndex lastIndex() const override;
    RaftCore::Term lastTerm() const override;
};

} // namespace RaftKV::Raft
