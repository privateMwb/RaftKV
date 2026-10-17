# Security Policy

## Supported versions

RaftKV is a small, individually maintained project. Security fixes are made on the
`main` branch and shipped in the next release; older releases are not patched.

| Version | Supported |
|---|:-:|
| `main` and the latest release | ✅ |
| Anything older | ❌ |

## Reporting a vulnerability

**Please do not report a suspected vulnerability in a public issue, pull request, or
discussion.** Use GitHub's private reporting instead, so a fix can be prepared before the
details are public:

1. Open this repository's **Security** tab.
2. Choose **Report a vulnerability**.
3. Describe the problem.

A useful report includes:

- what is affected (backend node, launcher, dashboard, Docker setup) and the commit or
  release you tested;
- steps to reproduce, ideally the exact `curl` request, input bytes, or command line;
- what you expected and what happened (a crash, a sanitizer report, a lost write, a
  stale read);
- any suggested fix.

This is a spare-time project, so responses are best-effort. You will get an
acknowledgement and an honest assessment, and credit in the fix if you want it.

## Security model: read this before deploying

RaftKV is built for learning and demonstration. It is designed to run on a trusted
machine or network, and **it has no authentication, authorization, or encryption**. This
is a deliberate scope limit, not an oversight:

- **No authentication on the client API or peer RPC.** Anyone who can reach a node's port
  can read, write, and delete keys, and can send Raft RPCs as if they were a peer.
- **No TLS.** Traffic, including values, is plaintext HTTP.
- **The launcher can crash and partition your cluster.** `scripts/launcher.mjs` exposes
  `kill`, `stop`, `isolate`, `partition` and `delay` over unauthenticated HTTP. It binds to
  `127.0.0.1` by default. `--host 0.0.0.0` (and `npm run dev:host` for the dashboard)
  hands those controls to everyone on your network.
- **Rate limiting is not an access control.** It covers `/kv/*` only, and clients are keyed
  by the `X-Forwarded-For` header (or share one bucket when it is absent), so the header
  can be spoofed.
- **`--cors-origin *`** lets any web page in a visitor's browser talk to your nodes.
- **Docker** publishes ports `8101`–`8103` on the host.

If you run RaftKV anywhere other than your own machine, keep every node and launcher port
behind a firewall or private network, never expose them to the internet, and put an
authenticating, TLS-terminating proxy in front of anything that must be reachable.

## What counts as a vulnerability

**In scope**

- Memory-safety bugs or crashes triggered by remote input, especially in the parsers that
  read untrusted bytes: the peer RPC codec, the KV command codec, percent-decoding of
  keys, and request handling.
- Data-safety violations: an acknowledged write that is later lost, a read that returns
  stale data when it should have returned `503`, or two leaders acknowledging writes in
  the same term.
- Corrupt or crafted WAL or state files that cause undefined behavior on startup.
- Path traversal or arbitrary file access through any endpoint or flag.
- Command injection or unintended process control in the launcher.
- Resource exhaustion that bypasses the documented limits (64 KiB value cap, 10 MiB body
  limit, the SSE stream cap, the rate limiter).

**Out of scope** (documented design limits)

- The missing authentication and encryption described above, and consequences of exposing
  the services to untrusted networks.
- Availability loss under injected network delay or partitions. Raft is expected to stop
  answering rather than answer wrongly; see
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#what-the-measurements-show).
- Flooding beyond the rate limiter from a client that can already reach the port.
- Vulnerabilities in the vendored libraries themselves; those are best reported to the
  library's own repository ([RaftCore](https://github.com/privateMwb/RaftCore),
  [FalconHTTP](https://github.com/privateMwb/FalconHTTP),
  [RateThrottle](https://github.com/privateMwb/RateThrottle),
  [MiniDatabase](https://github.com/privateMwb/MiniDatabase)). If you are unsure, report
  here and it will be routed.

## How the project stays on top of this

- **CodeQL** analyzes the C++ and TypeScript on every push and weekly.
- **Sanitizer builds** (ASan/UBSan and TSan) run in CI.
- **Dependabot** proposes weekly updates for GitHub Actions and the frontend's npm
  dependencies.
- Submodules are pinned to specific commits and updated deliberately.
