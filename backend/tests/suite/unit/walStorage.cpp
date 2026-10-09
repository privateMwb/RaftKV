// Unit WalStorage Test Suite
// Verifies the adapter gives RaftCore the Storage contract it expects on
// top of MiniDB's WriteAheadLog.
//
// Covers:
// - Append assigns 1-based indices; entryAt/range read them back
// - The empty-payload election no-op round-trips
// - Binary payloads (NUL, high bytes) round-trip exactly
// - Out-of-range reads are NOT_FOUND
// - truncateFrom() drops a suffix; truncating past the end is a no-op
// - A reopened log recovers entries, lastIndex, and lastTerm
// - An empty log reports kNoIndex / kNoTerm

#include <support/framework.h>

#include "raft/WalStorage.h"

#include <fstream>

using namespace RaftKV::Raft;
using namespace RaftCore;

static Vector<std::uint8_t> bytes(const std::string& text) {
    Vector<std::uint8_t> out;
    for (char c : text)
        out.push_back(static_cast<std::uint8_t>(c));
    return out;
}

static std::string text(const Vector<std::uint8_t>& payload) {
    std::string out;
    for (std::size_t i = 0; i < payload.size(); ++i)
        out.push_back(static_cast<char>(payload[i]));
    return out;
}

// Verifies a fresh log is empty with the "none" sentinels.
static void empty_log_sentinels() {
    TempDir dir;
    WalStorage storage(dir.file("log.wal"));

    CHK(storage.open() == Status::OK);
    CHK(storage.lastIndex() == kNoIndex);
    CHK(storage.lastTerm() == kNoTerm);

    LogEntry entry;
    CHK(storage.entryAt(1, entry) == Status::NOT_FOUND);
}

// Verifies append assigns consecutive indices readable via entryAt/range.
static void append_and_read_back() {
    TempDir dir;
    WalStorage storage(dir.file("log.wal"));
    CHK(storage.open() == Status::OK);

    LogIndex first = 0, second = 0, third = 0;
    CHK(storage.append(1, bytes("one"), first) == Status::OK);
    CHK(storage.append(1, bytes("two"), second) == Status::OK);
    CHK(storage.append(2, bytes("three"), third) == Status::OK);

    CHK(first == 1);
    CHK(second == 2);
    CHK(third == 3);
    CHK(storage.lastIndex() == 3);
    CHK(storage.lastTerm() == 2);

    LogEntry entry;
    CHK(storage.entryAt(2, entry) == Status::OK);
    CHK(entry.term == 1);
    CHK(text(entry.payload) == "two");

    Vector<LogEntry> entries;
    CHK(storage.range(1, 3, entries) == Status::OK);
    CHK(entries.size() == 3);
    if (entries.size() == 3) {
        CHK(text(entries[0].payload) == "one");
        CHK(text(entries[2].payload) == "three");
        CHK(entries[2].term == 2);
    }
}

// Verifies the election no-op (empty payload) is stored and read back empty.
static void empty_payload_round_trips() {
    TempDir dir;
    WalStorage storage(dir.file("log.wal"));
    CHK(storage.open() == Status::OK);

    LogIndex index = 0;
    CHK(storage.append(1, Vector<std::uint8_t>{}, index) == Status::OK);

    LogEntry entry;
    CHK(storage.entryAt(index, entry) == Status::OK);
    CHK(entry.term == 1);
    CHK(entry.payload.size() == 0);
}

// Verifies NULs and high bytes survive the string <-> bytes conversion.
static void binary_payload_exact() {
    TempDir dir;
    WalStorage storage(dir.file("log.wal"));
    CHK(storage.open() == Status::OK);

    const std::string raw("a\0\xff\n\r", 5);
    LogIndex index = 0;
    CHK(storage.append(1, bytes(raw), index) == Status::OK);

    LogEntry entry;
    CHK(storage.entryAt(index, entry) == Status::OK);
    CHK(text(entry.payload) == raw);
}

// Verifies reads outside the log report NOT_FOUND.
static void out_of_range_not_found() {
    TempDir dir;
    WalStorage storage(dir.file("log.wal"));
    CHK(storage.open() == Status::OK);

    LogIndex index = 0;
    CHK(storage.append(1, bytes("x"), index) == Status::OK);

    LogEntry entry;
    Vector<LogEntry> entries;
    CHK(storage.entryAt(0, entry) == Status::NOT_FOUND);
    CHK(storage.entryAt(2, entry) == Status::NOT_FOUND);
    CHK(storage.range(1, 5, entries) == Status::NOT_FOUND);
}

// Verifies truncateFrom drops the suffix and appends resume at the cut.
static void truncate_then_append() {
    TempDir dir;
    WalStorage storage(dir.file("log.wal"));
    CHK(storage.open() == Status::OK);

    LogIndex index = 0;
    CHK(storage.append(1, bytes("a"), index) == Status::OK);
    CHK(storage.append(1, bytes("b"), index) == Status::OK);
    CHK(storage.append(1, bytes("c"), index) == Status::OK);

    CHK(storage.truncateFrom(2) == Status::OK);
    CHK(storage.lastIndex() == 1);

    CHK(storage.append(3, bytes("z"), index) == Status::OK);
    CHK(index == 2);

    LogEntry entry;
    CHK(storage.entryAt(2, entry) == Status::OK);
    CHK(entry.term == 3);
    CHK(text(entry.payload) == "z");

    // Past the end: documented no-op.
    CHK(storage.truncateFrom(50) == Status::OK);
    CHK(storage.lastIndex() == 2);
}

// Verifies a reopened log recovers everything and keeps indexing.
static void reopen_recovers_entries() {
    TempDir dir;
    {
        WalStorage storage(dir.file("log.wal"));
        CHK(storage.open() == Status::OK);
        LogIndex index = 0;
        CHK(storage.append(4, bytes("kept"), index) == Status::OK);
        CHK(storage.append(5, bytes("also"), index) == Status::OK);
    }

    WalStorage reopened(dir.file("log.wal"));
    CHK(reopened.open() == Status::OK);
    CHK(reopened.lastIndex() == 2);
    CHK(reopened.lastTerm() == 5);

    LogEntry entry;
    CHK(reopened.entryAt(1, entry) == Status::OK);
    CHK(text(entry.payload) == "kept");

    LogIndex next = 0;
    CHK(reopened.append(5, bytes("next"), next) == Status::OK);
    CHK(next == 3);
}

static void run_tests() {
    RUN(empty_log_sentinels);
    RUN(append_and_read_back);
    RUN(empty_payload_round_trips);
    RUN(binary_payload_exact);
    RUN(out_of_range_not_found);
    RUN(truncate_then_append);
    RUN(reopen_recovers_entries);
}

REGISTER_TEST_SUITE();
