# Contributing to RaftKV

Thanks for your interest! Issues and pull requests are welcome, from typo fixes to new
failure scenarios for the dashboard. For anything larger than a small fix, please open an
issue first so we can agree on the approach before you spend the time.

## Getting set up

You need a C++23 compiler, CMake 3.20+, Git, and Node.js 22+ (the frontend tests run
TypeScript directly and need Node 22.18 or newer).

```bash
git clone --recurse-submodules https://github.com/privateMwb/RaftKV
cd RaftKV
```

Forgot `--recurse-submodules`? Run `git submodule update --init --recursive`. Each of the
four libraries under `backend/external/` vendors its own nested submodules, so the
`--recursive` matters.

### Backend

```bash
cd backend
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/raftkv_tests             # run every suite
./build/raftkv_tests list        # list suites and categories
./build/raftkv_tests <name>      # run one suite or category
```

Tests use a small custom framework (`backend/tests/support/`) with `unit/` and
`integration/` suites. The integration suites start real multi-node clusters, so they cover
elections, failover, replication, the client API and the event streams end to end.

### Frontend

```bash
cd frontend
npm ci
npm run lint
npm test
npm run build
```

### See it work

Changes to the Raft path are best checked by breaking things on purpose:

```bash
npm --prefix frontend install
node scripts/launcher.mjs --fresh      # 3 nodes + the kill/revive launcher
npm --prefix frontend run dev          # http://localhost:5173
```

[docs/DEMO.md](docs/DEMO.md) walks through crashing the leader, partitioning it, slowing
the network and losing the majority.

## What CI checks (and what it doesn't)

| Workflow | Covers |
|---|---|
| **Build** | Backend with GCC and Clang, Debug and Release |
| **Clang Format** | `backend/include` and `backend/src` against `.clang-format` |
| **Clang Tidy** | `.cpp` files under `backend/include` and `backend/src` against `.clang-tidy` |
| **Sanitizers** | Backend built with ASan/UBSan and with TSan |
| **CodeQL** | C++ and TypeScript, on push and weekly |
| **Frontend** | `npm run lint`, `npm test`, `npm run build` (only when `frontend/` changes) |

> [!IMPORTANT]
> **CI does not run the backend test suite.** The Build workflow compiles it but never
> executes `raftkv_tests`. Please run the suites locally before opening a pull request, and
> say so in the description.

## Before you open a pull request

- [ ] The backend builds and `./build/raftkv_tests` passes.
- [ ] New behavior has a test, and a bug fix has a test that fails without it.
- [ ] C++ is formatted: `clang-format -i <files>` (the CI check fails on any diff).
- [ ] Frontend changes pass `npm run lint` and `npm test`; run `npm run format` for Prettier.
- [ ] If you touched the Raft path (`KvNode`, `WalStorage`, `FilePersistentState`,
      `HttpTransport`), you tried the crash and partition scenarios in
      [docs/DEMO.md](docs/DEMO.md).
- [ ] Docs match the code: update the README tables, `docs/ARCHITECTURE.md`, or
      `frontend/README.md` if behavior, flags, endpoints or limits changed.
- [ ] Add an entry under **Unreleased** in [CHANGELOG.md](CHANGELOG.md) for anything a user
      would notice.

## Code style

- **C++23**, formatted by `.clang-format`. `.clang-tidy` documents the reasoning for each
  disabled check; please don't suppress a warning without a comment saying why.
- **Correctness over cleverness.** The consensus path is deliberately simple: one mutex
  around `RaftNode`, with `fsync` before every acknowledgement. Changes there need a clear
  argument for why safety still holds.
- **Frontend:** TypeScript, React, Tailwind CSS v4, ESLint and Prettier
  (`frontend/.prettierrc.json`). The dashboard has no runtime dependencies beyond React,
  Tailwind and Boxicons; please keep it that way unless there is a strong reason.

## Commits and pull requests

- Keep each pull request focused on one change.
- Existing history uses a short lowercase prefix: `add:`, `fix:`, `update:`. Follow that,
  for example `fix: reject keys over 1 KiB`.
- Explain *why* in the description, and include what you ran to test it.

## Working with the submodules

The four libraries under `backend/external/` live in their own repositories. A fix to
RaftCore, FalconHTTP, RateThrottle or MiniDatabase belongs in that repository; RaftKV then
bumps the pinned commit in a separate change. Don't edit files inside `backend/external/`
in a RaftKV pull request.

## Releasing (maintainers)

1. Update [CHANGELOG.md](CHANGELOG.md): move **Unreleased** under the new version and date.
2. Write `.github/releases/vX.Y.Z.md`; the **Release** workflow publishes that file as the
   GitHub Release body and fails if it is missing.
3. Tag and push: `git tag vX.Y.Z && git push origin vX.Y.Z`.

## Reporting security issues

Please don't open a public issue. See [SECURITY.md](SECURITY.md).

## License

By contributing, you agree that your contributions are licensed under the
[MIT License](LICENSE).
