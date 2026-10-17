# Changelog

All notable changes to RaftKV are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - Unreleased

First release: a three-node replicated key-value store built on RaftCore, with a live web
dashboard that can crash, slow down, and partition the real processes.

### Added

**Backend**

- Replicated key-value store: nodes elect a leader, replicate writes through a durable
  log, and apply committed commands to an in-memory map. Built on
  [RaftCore](https://github.com/privateMwb/RaftCore), with
  [FalconHTTP](https://github.com/privateMwb/FalconHTTP),
  [RateThrottle](https://github.com/privateMwb/RateThrottle), and
  [MiniDatabase](https://github.com/privateMwb/MiniDatabase) as submodules.
- Durable storage: every Raft log append is `fsync`ed to a write-ahead log before it is
  acknowledged, and `currentTerm`/`votedFor` are rewritten atomically (temp file, `fsync`,
  rename). A node killed with `SIGKILL` restarts from disk and catches up.
- Client API on each node's single HTTP port: `PUT`/`GET`/`DELETE /kv/<key>` with
  binary-safe values, linearizable reads, `?stale=1` for a node's local copy, `307`
  redirects from followers to the leader, and `503` + `Retry-After` when no quorum is
  available.
- Rate limiting on `/kv/*` (default 600 requests per client per 10 s) and a 64 KiB value
  cap, both configurable (`--rate-limit`, `--max-value-bytes`).
- Peer RPC (`AppendEntries`, `RequestVote`) over HTTP with a small versioned binary body.
- Live state over Server-Sent Events: `/events` (one node), `/cluster/events` (a node's
  view of the whole cluster), plus one-shot `/status` and `/cluster`. State is pushed only
  when it changes, with a `ping` every 2 s while idle.
- Command-line interface with a stdin REPL (`put`, `get`, `del`, `status`, `quit`) and a
  headless mode (`--no-repl`); `--cors-origin` for dashboards on another origin.
- Test suites (unit and integration) run with `./build/raftkv_tests`, including real
  multi-node election, failover, replication, client API, and event-stream scenarios.

**Launcher**

- `scripts/launcher.mjs` starts a local cluster of real backend processes and exposes
  **Kill** (`SIGKILL`), **Revive**, and stop controls.
- Network fault injection through node-to-node proxies: per-link **delay**, **Isolate** a
  node while it keeps running, partition into groups, and heal.
- Options for fresh starts (`--fresh`), LAN access (`--host`), redirect hosts
  (`--public-host`), and a no-proxy mode (`--no-network`).

**Dashboard**

- Live cluster view built from every node's own stream, so a partitioned old leader is
  shown alive and marked **stale** instead of being mistaken for a dead node.
- Derived event feed, plain-language "what's happening" narration, per-node role, term,
  and log indices, and a key-value console.
- Kill, Revive, Isolate, and delay-slider controls, with warning zones taken from
  measurement.
- Responsive layout that works on a phone as well as a desktop.
- React 19, Vite, and Tailwind CSS v4.

**Documentation and tooling**

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the system works, the observability
  model, fault injection, measurements, trade-offs, and known limits.
- [docs/DEMO.md](docs/DEMO.md): a ten-minute guided tour.
- Dockerfile and `docker-compose.yml` for a three-node cluster with fixed IPs (**untested**).
- GitHub Actions: Build (GCC and Clang, Debug and Release), Clang Format, Clang Tidy,
  Sanitizers, CodeQL, Frontend, Docs, and Release; Dependabot for Actions and npm.
- README, `SECURITY.md`, `CONTRIBUTING.md`, and this changelog.

[Unreleased]: https://github.com/privateMwb/RaftKV/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/privateMwb/RaftKV/releases/tag/v1.0.0
