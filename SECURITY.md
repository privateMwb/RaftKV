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
authenticating, TLS-terminating proxy in front of anything that must be reachable. The one
deliberate exception is the [hosted demo](#hosted-demo) below, which is public by design and
is protected differently.

## Hosted demo

The hosted demo (see [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md)) is **intentionally public**:
anyone who opens the dashboard can read and write keys and crash, slow down, and partition
the cluster. It has no login, because crashing the cluster is the point of it. It is safe to
leave open because of what it exposes and what it holds:

- **It holds nothing real.** Node data is wiped on every start, and the demo stores only
  what visitors type into it. Never put anything sensitive in it.
- **Only an allowlist is reachable.** `scripts/gateway.mjs` forwards the client API, the
  read-only state routes, the live streams, and the specific launcher controls the dashboard
  uses. The nodes' peer-to-peer routes (`/raft/...`), stopping the whole cluster, and every
  other route are `404`. Nodes and launcher listen on `127.0.0.1` inside the container.
- **Cross-origin access is explicit.** Browsers get CORS headers only for the origins in
  `ALLOWED_ORIGINS`; the backend's own `*` headers are removed.
- **Abuse is bounded.** Per-client limits on requests, destructive controls, concurrent
  streams, and body size, plus a global cap on destructive controls.
- **It repairs itself.** A node left down, or a link left cut, is restored automatically.
- **Credentials never reach the backend.** Cookies and authorization headers are dropped.

What this does *not* give you is authentication or confidentiality: assume every request,
and every key and value, is visible to anyone.

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
- A way around the hosted demo's gateway: reaching a route that is not on its allowlist
  (especially `/raft/...`), getting CORS headers for an origin that is not allowed, evading
  its rate limits or stream caps, or making it forward credentials or arbitrary headers.

**Out of scope** (documented design limits)

- The missing authentication and encryption described above, and consequences of exposing
  the services to untrusted networks. (The hosted demo's open Kill button and writes are by
  design; see above.)
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
- **Sanitizers**: the full test suite, including real multi-node clusters, and a smoke test
  of a real node run under ASan/UBSan and under TSan in CI.
- **Dependabot** proposes weekly updates for GitHub Actions and the frontend's npm
  dependencies.
- Submodules are pinned to specific commits and updated deliberately.
