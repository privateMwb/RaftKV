/**
 * @file            FilePersistentState.h
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
#include <RaftCore/Common/Status.h>               // RaftCore::Status
#include <RaftCore/Common/Types.h>                // RaftCore::Term, RaftCore::NodeId
#include <RaftCore/Interfaces/PersistentState.h>  // RaftCore::PersistentState (the interface implemented here)

#include <optional> // std::optional - votedFor
#include <string>   // std::string - file path
// clang-format on

namespace RaftKV::Raft {

/**
 * @brief File-backed RaftCore::PersistentState: `currentTerm` and `votedFor`
 * survive a crash or restart.
 *
 * @details Every setter rewrites the whole (tiny) file via write-temp,
 * `fsync`, atomic `rename`, then `fsync` of the parent directory, so a crash
 * at any point leaves either the complete old state or the complete new one,
 * never a torn mix. A setter updates its in-memory value only after the file
 * write succeeded, so `currentTerm()`/`votedFor()` never report a value that
 * isn't durable.
 *
 * RaftCore persists a term bump and the vote reset as two separate calls
 * (term first). A crash between them leaves the new term with the *previous*
 * term's vote still recorded; that can only make this node refuse other
 * candidates in the new term, never grant two different votes in one term, so
 * it is safe (at worst one extra election).
 *
 * @attention Not thread-safe; RaftKV only touches it under KvNode's mutex.
 * Call open() before first use.
 */
class FilePersistentState final : public RaftCore::PersistentState {
  private:
    std::string path_;
    RaftCore::Term term_ = RaftCore::kNoTerm;
    std::optional<RaftCore::NodeId> votedFor_;

    [[nodiscard]] RaftCore::Status persist(RaftCore::Term term,
                                           const std::optional<RaftCore::NodeId>& vote) const;

  public:
    /// @param path File to keep the state in. Its parent directory must exist.
    explicit FilePersistentState(std::string path);

    /**
     * @brief Loads existing state, or starts fresh if the file doesn't exist.
     * @return `OK`; `PARSE_ERROR` if the file exists but is malformed (never
     * silently reset: a lost vote could allow a double vote); `IO_ERROR` if
     * it can't be read.
     */
    [[nodiscard]] RaftCore::Status open();

    [[nodiscard]] RaftCore::Status setCurrentTerm(RaftCore::Term term) override;
    RaftCore::Term currentTerm() const override;
    [[nodiscard]] RaftCore::Status setVotedFor(std::optional<RaftCore::NodeId> candidate) override;
    std::optional<RaftCore::NodeId> votedFor() const override;
};

} // namespace RaftKV::Raft
