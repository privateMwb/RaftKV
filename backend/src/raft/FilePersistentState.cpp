/**
 * @file FilePersistentState.cpp
 * @brief FilePersistentState implementation.
 */

// ============================================================
// Implementation for RaftKV::Raft::FilePersistentState.
// ============================================================
//
//  Sections:
//   1. File Format Helpers
//   2. Construction & Loading
//   3. Setters & Accessors
//
// File format (text, one record, written atomically):
//   RKVP1\n
//   <currentTerm>\n
//   N\n                       -- no vote this term
//   V <len> <votedFor bytes>\n -- voted; length-prefixed, so any NodeId is safe
//
// ============================================================

// clang-format off
#include "FilePersistentState.h" // FilePersistentState — the class this file implements

#include <charconv>   // std::from_chars - parsing term and vote length
#include <cstdio>     // std::rename, std::remove
#include <fstream>    // std::ifstream - reading the state file
#include <iterator>   // std::istreambuf_iterator - slurping the file
#include <filesystem> // std::filesystem::exists - fresh-start detection
#include <string_view> // std::string_view - zero-copy parsing

#include <fcntl.h>    // ::open, O_WRONLY, O_CREAT, O_TRUNC, O_DIRECTORY
#include <unistd.h>   // ::write, ::fsync, ::close
// clang-format on

namespace RaftKV::Raft {

using namespace RaftCore;

// ============================================================
//  Section 1 — File Format Helpers
// ============================================================

namespace {

constexpr std::string_view kMagic = "RKVP1\n";

std::string encode(Term term, const std::optional<NodeId>& vote) {
    std::string out(kMagic);
    out += std::to_string(term);
    out += '\n';

    if (vote)
        out += "V " + std::to_string(vote->size()) + " " + *vote + "\n";
    else
        out += "N\n";

    return out;
}

bool parseNumber(std::string_view text, std::uint64_t& out) {
    if (text.empty())
        return false;

    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool decode(std::string_view data, Term& term, std::optional<NodeId>& vote) {
    if (!data.starts_with(kMagic))
        return false;
    data.remove_prefix(kMagic.size());

    const std::size_t termEnd = data.find('\n');
    if (termEnd == std::string_view::npos || !parseNumber(data.substr(0, termEnd), term))
        return false;
    data.remove_prefix(termEnd + 1);

    if (data == "N\n") {
        vote.reset();
        return true;
    }

    if (!data.starts_with("V "))
        return false;
    data.remove_prefix(2);

    const std::size_t lengthEnd = data.find(' ');
    std::uint64_t length = 0;
    if (lengthEnd == std::string_view::npos || !parseNumber(data.substr(0, lengthEnd), length))
        return false;
    data.remove_prefix(lengthEnd + 1);

    // Exactly <length> bytes then the closing newline, nothing after.
    if (data.size() != length + 1 || data.back() != '\n')
        return false;

    vote = NodeId(data.substr(0, length));
    return true;
}

bool writeAll(int fd, const std::string& bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0)
            return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

// ============================================================
//  Section 2 — Construction & Loading
// ============================================================

FilePersistentState::FilePersistentState(std::string path) : path_(std::move(path)) {}

Status FilePersistentState::open() {
    std::error_code error;
    if (!std::filesystem::exists(path_, error))
        return error ? Status::IO_ERROR : Status::OK; // Fresh node: term 0, no vote.

    std::ifstream file(path_, std::ios::binary);
    if (!file)
        return Status::IO_ERROR;

    const std::string data((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    Term term = kNoTerm;
    std::optional<NodeId> vote;
    if (!decode(data, term, vote))
        return Status::PARSE_ERROR;

    term_ = term;
    votedFor_ = std::move(vote);
    return Status::OK;
}

// ============================================================
//  Section 3 — Setters & Accessors
// ============================================================

Status FilePersistentState::persist(Term term, const std::optional<NodeId>& vote) const {
    const std::string tempPath = path_ + ".tmp";

    const int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Status::IO_ERROR;

    const bool ok = writeAll(fd, encode(term, vote)) && ::fsync(fd) == 0;
    ::close(fd);

    if (!ok || std::rename(tempPath.c_str(), path_.c_str()) != 0) {
        std::remove(tempPath.c_str());
        return Status::IO_ERROR;
    }

    // The rename itself is only durable once the directory entry is.
    const std::string parent = std::filesystem::path(path_).parent_path().string();
    const int dirFd = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirFd < 0)
        return Status::IO_ERROR;

    const bool dirOk = ::fsync(dirFd) == 0;
    ::close(dirFd);
    return dirOk ? Status::OK : Status::IO_ERROR;
}

Status FilePersistentState::setCurrentTerm(Term term) {
    if (Status status = persist(term, votedFor_); status != Status::OK)
        return status;

    term_ = term;
    return Status::OK;
}

Term FilePersistentState::currentTerm() const {
    return term_;
}

Status FilePersistentState::setVotedFor(std::optional<NodeId> candidate) {
    if (Status status = persist(term_, candidate); status != Status::OK)
        return status;

    votedFor_ = std::move(candidate);
    return Status::OK;
}

std::optional<NodeId> FilePersistentState::votedFor() const {
    return votedFor_;
}

} // namespace RaftKV::Raft
