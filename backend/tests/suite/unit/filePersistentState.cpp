// Unit FilePersistentState Test Suite
// Verifies currentTerm/votedFor are durable and never silently reset.
//
// Covers:
// - Fresh open starts at term 0 with no vote and writes nothing
// - Term and vote survive a reopen
// - Clearing the vote survives a reopen
// - NodeIds with spaces and newlines round-trip (length-prefixed)
// - A corrupt file is PARSE_ERROR, not a silent reset
// - No temp file is left behind after a successful write

#include <support/framework.h>

#include "raft/FilePersistentState.h"

#include <filesystem>
#include <fstream>

using namespace RaftKV::Raft;
using RaftCore::Status;

// Verifies a missing file means fresh state and creates nothing.
static void fresh_open_is_empty() {
    TempDir dir;
    FilePersistentState state(dir.file("s"));

    CHK(state.open() == Status::OK);
    CHK(state.currentTerm() == 0);
    CHK(!state.votedFor().has_value());
    CHK(!std::filesystem::exists(dir.file("s")));
}

// Verifies term and vote are read back after a reopen.
static void term_and_vote_survive() {
    TempDir dir;
    {
        FilePersistentState state(dir.file("s"));
        CHK(state.open() == Status::OK);
        CHK(state.setCurrentTerm(7) == Status::OK);
        CHK(state.setVotedFor(std::optional<RaftCore::NodeId>("node-2")) == Status::OK);
    }

    FilePersistentState reopened(dir.file("s"));
    CHK(reopened.open() == Status::OK);
    CHK(reopened.currentTerm() == 7);
    CHK(reopened.votedFor() == std::optional<RaftCore::NodeId>("node-2"));
}

// Verifies clearing the vote is durable too.
static void cleared_vote_survives() {
    TempDir dir;
    {
        FilePersistentState state(dir.file("s"));
        CHK(state.open() == Status::OK);
        CHK(state.setVotedFor(std::optional<RaftCore::NodeId>("a")) == Status::OK);
        CHK(state.setVotedFor(std::nullopt) == Status::OK);
        CHK(state.setCurrentTerm(3) == Status::OK);
    }

    FilePersistentState reopened(dir.file("s"));
    CHK(reopened.open() == Status::OK);
    CHK(reopened.currentTerm() == 3);
    CHK(!reopened.votedFor().has_value());
}

// Verifies awkward NodeIds round-trip exactly.
static void odd_node_ids_round_trip() {
    TempDir dir;
    const RaftCore::NodeId odd("node one\nV 3 x", 14);
    {
        FilePersistentState state(dir.file("s"));
        CHK(state.open() == Status::OK);
        CHK(state.setVotedFor(std::optional<RaftCore::NodeId>(odd)) == Status::OK);
    }

    FilePersistentState reopened(dir.file("s"));
    CHK(reopened.open() == Status::OK);
    CHK(reopened.votedFor() == std::optional<RaftCore::NodeId>(odd));
}

// Verifies garbage and truncated files are rejected, not treated as fresh.
static void corrupt_file_rejected() {
    TempDir dir;

    {
        std::ofstream out(dir.file("garbage"), std::ios::binary);
        out << "not a state file";
    }
    FilePersistentState garbage(dir.file("garbage"));
    CHK(garbage.open() == Status::PARSE_ERROR);

    {
        std::ofstream out(dir.file("short"), std::ios::binary);
        out << "RKVP1\n5\nV 9 abc\n"; // claims 9 bytes, has 3.
    }
    FilePersistentState truncated(dir.file("short"));
    CHK(truncated.open() == Status::PARSE_ERROR);
}

// Verifies the write-temp-then-rename leaves only the final file.
static void no_temp_file_left() {
    TempDir dir;
    FilePersistentState state(dir.file("s"));
    CHK(state.open() == Status::OK);
    CHK(state.setCurrentTerm(1) == Status::OK);

    CHK(std::filesystem::exists(dir.file("s")));
    CHK(!std::filesystem::exists(dir.file("s.tmp")));
}

// Verifies a failed write leaves the in-memory value untouched.
static void failed_write_keeps_old() {
    TempDir dir;
    FilePersistentState state("/nonexistent-dir-xyz/state");

    CHK(state.setCurrentTerm(5) == Status::IO_ERROR);
    CHK(state.currentTerm() == 0);
}

static void run_tests() {
    RUN(fresh_open_is_empty);
    RUN(term_and_vote_survive);
    RUN(cleared_vote_survives);
    RUN(odd_node_ids_round_trip);
    RUN(corrupt_file_rejected);
    RUN(no_temp_file_left);
    RUN(failed_write_keeps_old);
}

REGISTER_TEST_SUITE();
