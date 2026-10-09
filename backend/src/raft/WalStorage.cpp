/**
 * @file WalStorage.cpp
 * @brief WalStorage implementation.
 */

// ============================================================
// Implementation for RaftKV::Raft::WalStorage.
// ============================================================
//
//  Sections:
//   1. Conversion Helpers
//   2. Construction & Opening
//   3. Storage Interface
//
// ============================================================

// clang-format off
#include "WalStorage.h" // WalStorage — the class this file implements

#include <utility> // std::move
// clang-format on

namespace RaftKV::Raft {

// ============================================================
//  Section 1 — Conversion Helpers
// ============================================================

namespace {

RaftCore::Status toRaftStatus(MiniDB::Common::Status status) {
    switch (status) {
    case MiniDB::Common::Status::OK:
        return RaftCore::Status::OK;
    case MiniDB::Common::Status::NOT_FOUND:
        return RaftCore::Status::NOT_FOUND;
    case MiniDB::Common::Status::OUT_OF_MEMORY:
        return RaftCore::Status::OUT_OF_MEMORY;
    case MiniDB::Common::Status::PARSE_ERROR:
        return RaftCore::Status::PARSE_ERROR;
    default:
        return RaftCore::Status::IO_ERROR; // Anything else is a log failure to RaftCore.
    }
}

std::string toText(const VectorPro::Vector<std::uint8_t>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i)
        text.push_back(static_cast<char>(bytes[i]));
    return text;
}

VectorPro::Vector<std::uint8_t> toBytes(const std::string& text) {
    VectorPro::Vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (char c : text)
        bytes.push_back(static_cast<std::uint8_t>(c));
    return bytes;
}

RaftCore::LogEntry toRaftEntry(const MiniDB::Storage::LogEntry& entry) {
    RaftCore::LogEntry converted;
    converted.term = entry.term;
    converted.payload = toBytes(entry.payload);
    return converted;
}

} // namespace

// ============================================================
//  Section 2 — Construction & Opening
// ============================================================

WalStorage::WalStorage(std::string path) : wal_(std::move(path)) {}

RaftCore::Status WalStorage::open() {
    return toRaftStatus(wal_.open());
}

// ============================================================
//  Section 3 — Storage Interface
// ============================================================

RaftCore::Status WalStorage::append(RaftCore::Term term, VectorPro::Vector<std::uint8_t> payload,
                                    RaftCore::LogIndex& outIndex) {
    return toRaftStatus(wal_.append(term, toText(payload), outIndex));
}

RaftCore::Status WalStorage::entryAt(RaftCore::LogIndex index, RaftCore::LogEntry& outEntry) const {
    MiniDB::Storage::LogEntry entry;
    if (RaftCore::Status status = toRaftStatus(wal_.entryAt(index, entry));
        status != RaftCore::Status::OK)
        return status;

    outEntry = toRaftEntry(entry);
    return RaftCore::Status::OK;
}

RaftCore::Status WalStorage::range(RaftCore::LogIndex fromIndex, RaftCore::LogIndex toIndex,
                                   VectorPro::Vector<RaftCore::LogEntry>& outEntries) const {
    VectorPro::Vector<MiniDB::Storage::LogEntry> entries;
    if (RaftCore::Status status = toRaftStatus(wal_.range(fromIndex, toIndex, entries));
        status != RaftCore::Status::OK)
        return status;

    VectorPro::Vector<RaftCore::LogEntry> converted;
    converted.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
        converted.push_back(toRaftEntry(entries[i]));

    outEntries = std::move(converted);
    return RaftCore::Status::OK;
}

RaftCore::Status WalStorage::truncateFrom(RaftCore::LogIndex index) {
    return toRaftStatus(wal_.truncateFrom(index));
}

RaftCore::LogIndex WalStorage::lastIndex() const {
    return wal_.lastIndex();
}

RaftCore::Term WalStorage::lastTerm() const {
    return wal_.lastTerm();
}

} // namespace RaftKV::Raft
