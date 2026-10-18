import assert from "node:assert/strict";
import http from "node:http";
import { after, before, beforeEach, describe, it } from "node:test";
import { classify, clientIp, createGateway, FixedWindowLimiter } from "../gateway.mjs";
import { isHealthy, Watchdog } from "../watchdog.mjs";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// --------------------------------------------------------------------- routing

describe("classify (the allowlist)", () => {
  const allowed = [
    ["GET", "/n/1/status", { kind: "node" }],
    ["GET", "/n/3/cluster", { kind: "node" }],
    ["GET", "/n/2/events", { kind: "stream" }],
    ["GET", "/n/2/cluster/events", { kind: "stream" }],
    ["GET", "/n/1/kv/greeting", { kind: "node" }],
    ["PUT", "/n/1/kv/greeting", { kind: "node" }],
    ["DELETE", "/n/1/kv/a%20b", { kind: "node" }],
    ["GET", "/launcher/nodes", { kind: "launcher", control: false }],
    ["GET", "/launcher/network", { kind: "launcher", control: false }],
    ["POST", "/launcher/nodes/node-2/kill", { kind: "launcher", control: true }],
    ["POST", "/launcher/nodes/node-1/start", { kind: "launcher", control: true }],
    ["POST", "/launcher/nodes/node-3/stop", { kind: "launcher", control: true }],
    ["POST", "/launcher/network/delay", { kind: "launcher", control: true }],
    ["POST", "/launcher/network/partition", { kind: "launcher", control: true }],
    ["POST", "/launcher/network/heal", { kind: "launcher", control: true }],
    ["POST", "/launcher/network/reset", { kind: "launcher", control: true }],
    ["POST", "/launcher/network/isolate/node-1", { kind: "launcher", control: true }],
    ["POST", "/launcher/network/reconnect/node-3", { kind: "launcher", control: true }],
    ["GET", "/healthz", { kind: "health" }],
    ["GET", "/", { kind: "root" }],
  ];

  for (const [method, path, expected] of allowed)
    it(`allows ${method} ${path}`, () => {
      assert.partialDeepStrictEqual(classify(method, path, 3), expected);
    });

  const blocked = [
    // The nodes' peer-to-peer RPC must never be reachable from outside.
    ["POST", "/n/1/raft/append-entries"],
    ["POST", "/n/1/raft/request-vote"],
    ["GET", "/n/1/raft/append-entries"],
    // Nodes that do not exist.
    ["GET", "/n/0/status"],
    ["GET", "/n/4/status"],
    ["GET", "/n/1x/status"],
    ["GET", "/n/1"],
    // Wrong method for a real path.
    ["PUT", "/n/1/status"],
    ["POST", "/n/1/kv/key"],
    ["DELETE", "/n/1/cluster"],
    ["POST", "/n/1/events"],
    // Malformed keys.
    ["GET", "/n/1/kv/"],
    ["GET", "/n/1/kv"],
    ["GET", "/n/1/kv/a/b"],
    // Launcher routes the dashboard never uses, or that are not controls at all.
    ["POST", "/launcher/cluster/stop"],
    ["POST", "/launcher/cluster/start"],
    ["GET", "/launcher/health"],
    ["GET", "/launcher/"],
    ["GET", "/launcher/nodes/node-1/kill"],
    ["POST", "/launcher/nodes/node-1/restart"],
    ["POST", "/launcher/nodes/evil/kill"],
    ["POST", "/launcher/nodes/node-1/kill/extra"],
    ["POST", "/launcher/network/isolate/../heal"],
    ["POST", "/launcher/network/isolate/evil"],
    ["GET", "/launcher/network/delay"],
    // Top-level paths that belong to a node directly.
    ["GET", "/kv/key"],
    ["GET", "/status"],
    ["POST", "/raft/append-entries"],
    ["GET", "/nodes"],
    ["POST", "/healthz"],
  ];

  for (const [method, path] of blocked)
    it(`blocks ${method} ${path}`, () => {
      assert.equal(classify(method, path, 3), null);
    });

  it("respects the configured node count", () => {
    assert.ok(classify("GET", "/n/5/status", 5));
    assert.equal(classify("GET", "/n/6/status", 5), null);
  });
});

// ---------------------------------------------------------------------- limits

describe("FixedWindowLimiter", () => {
  it("allows up to the limit, then refuses until the window rolls over", () => {
    const limiter = new FixedWindowLimiter({ limit: 3, windowMs: 1000 });
    assert.ok(limiter.take("a", 0).ok);
    assert.ok(limiter.take("a", 100).ok);
    assert.ok(limiter.take("a", 200).ok);

    const refused = limiter.take("a", 300);
    assert.equal(refused.ok, false);
    assert.equal(refused.retryAfterSec, 1);

    assert.ok(limiter.take("a", 1000).ok, "a new window starts after windowMs");
  });

  it("counts each key separately", () => {
    const limiter = new FixedWindowLimiter({ limit: 1, windowMs: 1000 });
    assert.ok(limiter.take("a", 0).ok);
    assert.equal(limiter.take("a", 1).ok, false);
    assert.ok(limiter.take("b", 1).ok);
  });

  it("reports how long to wait, rounded up and at least one second", () => {
    const limiter = new FixedWindowLimiter({ limit: 1, windowMs: 60_000 });
    limiter.take("a", 0);
    assert.equal(limiter.take("a", 1_000).retryAfterSec, 59);
    assert.equal(limiter.take("a", 59_900).retryAfterSec, 1);
  });
});

describe("clientIp", () => {
  const request = (xff, remote = "10.9.9.9") => ({
    headers: xff === undefined ? {} : { "x-forwarded-for": xff },
    socket: { remoteAddress: remote },
  });

  it("takes the entry the trusted proxy appended, not one the client supplied", () => {
    assert.equal(clientIp(request("6.6.6.6, 203.0.113.5"), 1), "203.0.113.5");
  });

  it("counts back one more entry per extra trusted proxy", () => {
    assert.equal(clientIp(request("203.0.113.5, 198.51.100.1"), 2), "203.0.113.5");
  });

  it("falls back to the connection address when the chain is too short", () => {
    assert.equal(clientIp(request(undefined), 1), "10.9.9.9");
    assert.equal(clientIp(request("203.0.113.5"), 2), "10.9.9.9");
  });

  it("ignores the header entirely when no proxy is trusted", () => {
    assert.equal(clientIp(request("6.6.6.6"), 0), "10.9.9.9");
  });
});

// ------------------------------------------------------------------- the proxy

/** A stand-in for one backend node: records what reaches it. */
function fakeNode(id) {
  const seen = [];
  let streamConnections = 0;
  let streamClosed = 0;
  let streamSockets = [];
  let streamStatus = 200;

  const server = http.createServer((req, res) => {
    const body = [];
    req.on("data", (chunk) => body.push(chunk));
    req.on("end", () => {
      const record = {
        method: req.method,
        url: req.url,
        headers: req.headers,
        body: Buffer.concat(body).toString(),
      };
      seen.push(record);

      // Real nodes send their own CORS headers; the gateway must replace them.
      const common = { "Access-Control-Allow-Origin": "*", "x-raftkv-node": id };

      if (req.url === "/status") {
        res.writeHead(200, { ...common, "Content-Type": "application/json" });
        return res.end(JSON.stringify({ id, role: "leader" }));
      }
      if (req.url.startsWith("/kv/redirect")) {
        res.writeHead(307, { ...common, Location: "https://demo.example/n/3/kv/redirect" });
        return res.end();
      }
      if (req.url.startsWith("/kv/")) {
        res.writeHead(req.method === "PUT" ? 204 : 200, {
          ...common,
          "Retry-After": "1",
        });
        return res.end(req.method === "GET" ? "stored-value" : undefined);
      }
      if (req.url === "/cluster/events" || req.url === "/events") {
        if (streamStatus !== 200) {
          res.writeHead(streamStatus);
          return res.end();
        }
        streamConnections += 1;
        streamSockets.push(res);
        res.on("close", () => (streamClosed += 1));
        res.writeHead(200, { "Content-Type": "text/event-stream", Connection: "close" });
        return res.write('event: cluster\ndata: {"n":1}\n\n');
      }
      res.writeHead(404);
      res.end();
    });
  });

  return {
    id,
    server,
    seen,
    get streamConnections() {
      return streamConnections;
    },
    get streamClosed() {
      return streamClosed;
    },
    setStreamStatus(status) {
      streamStatus = status;
    },
    /** Sends raw SSE text to every connected stream. */
    push(text) {
      for (const res of streamSockets) res.write(text);
    },
    /** Ends every connected stream from the node's side, as a crash would. */
    dropStreams() {
      for (const res of streamSockets) res.destroy();
      streamSockets = [];
    },
    resetCounters() {
      seen.length = 0;
      streamConnections = 0;
      streamClosed = 0;
      streamSockets = [];
      streamStatus = 200;
    },
    listen: () =>
      new Promise((resolve) =>
        server.listen(0, "127.0.0.1", () => resolve(server.address().port)),
      ),
    close: () =>
      new Promise((resolve) => {
        server.close(resolve);
        server.closeAllConnections();
      }),
  };
}

/** A stand-in for the launcher. */
function fakeLauncher() {
  const seen = [];
  const server = http.createServer((req, res) => {
    seen.push({ method: req.method, url: req.url, headers: req.headers });
    res.writeHead(200, {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
      "Cache-Control": "max-age=999",
    });
    res.end(JSON.stringify({ ok: true, path: req.url }));
  });
  return {
    seen,
    listen: () =>
      new Promise((resolve) =>
        server.listen(0, "127.0.0.1", () => resolve(server.address().port)),
      ),
    close: () =>
      new Promise((resolve) => {
        server.close(resolve);
        server.closeAllConnections();
      }),
  };
}

const DASHBOARD = "https://raftkv.vercel.app";

describe("gateway", () => {
  let nodes;
  let launcher;
  let gateway;
  let base;

  async function start(overrides = {}) {
    gateway = createGateway({
      nodePorts: nodes.ports,
      launcherPort: nodes.launcherPort,
      allowedOrigins: [DASHBOARD],
      trustHops: 0,
      ...overrides,
    });
    const { port } = await gateway.listen(0, "127.0.0.1");
    base = `http://127.0.0.1:${port}`;
  }

  before(async () => {
    const fakes = [fakeNode("node-1"), fakeNode("node-2"), fakeNode("node-3")];
    launcher = fakeLauncher();
    nodes = {
      fakes,
      ports: await Promise.all(fakes.map((fake) => fake.listen())),
      launcherPort: await launcher.listen(),
    };
  });

  after(async () => {
    await gateway?.close();
    await Promise.all([...nodes.fakes.map((fake) => fake.close()), launcher.close()]);
  });

  beforeEach(async () => {
    // Close first and let the nodes notice, so the previous test's closing streams are
    // not counted in this test.
    await gateway?.close();
    gateway = null;
    await sleep(50);
    for (const fake of nodes.fakes) fake.resetCounters();
    launcher.seen.length = 0;
  });

  // -------------------------------------------------------------- proxying

  it("forwards a node's state and strips the /n/<k> prefix", async () => {
    await start();
    const response = await fetch(`${base}/n/2/status`);
    assert.equal(response.status, 200);
    assert.deepEqual(await response.json(), { id: "node-2", role: "leader" });
    assert.equal(nodes.fakes[1].seen[0].url, "/status");
    assert.equal(nodes.fakes[0].seen.length, 0, "only the addressed node is contacted");
  });

  it("forwards a write's body and method, and the node's answer headers", async () => {
    await start();
    const response = await fetch(`${base}/n/1/kv/greeting`, { method: "PUT", body: "hello" });
    assert.equal(response.status, 204);
    assert.equal(response.headers.get("x-raftkv-node"), "node-1");
    assert.equal(response.headers.get("retry-after"), "1");
    assert.equal(nodes.fakes[0].seen[0].method, "PUT");
    assert.equal(nodes.fakes[0].seen[0].body, "hello");
  });

  it("forwards the query string (for ?stale=1)", async () => {
    await start();
    await fetch(`${base}/n/1/kv/greeting?stale=1`);
    assert.equal(nodes.fakes[0].seen[0].url, "/kv/greeting?stale=1");
  });

  it("passes a follower's 307 through untouched so clients can follow it", async () => {
    await start();
    const response = await fetch(`${base}/n/1/kv/redirect`, { redirect: "manual" });
    assert.equal(response.status, 307);
    assert.equal(response.headers.get("location"), "https://demo.example/n/3/kv/redirect");
  });

  it("does not forward cookies, credentials, or other headers a node has no use for", async () => {
    await start();
    await fetch(`${base}/n/1/status`, {
      headers: { Cookie: "session=secret", Authorization: "Bearer secret", "X-Custom": "x" },
    });
    const received = nodes.fakes[0].seen[0].headers;
    assert.equal(received.cookie, undefined);
    assert.equal(received.authorization, undefined);
    assert.equal(received["x-custom"], undefined);
  });

  it("tells the node who the client is, and ignores a forged X-Forwarded-For", async () => {
    await start({ trustHops: 1 });
    await fetch(`${base}/n/1/status`, { headers: { "X-Forwarded-For": "6.6.6.6, 203.0.113.5" } });
    assert.equal(nodes.fakes[0].seen[0].headers["x-forwarded-for"], "203.0.113.5");
  });

  // ------------------------------------------------------------- allowlist

  it("never forwards peer-to-peer RPC, and the node never sees the attempt", async () => {
    await start();
    for (const path of ["/n/1/raft/append-entries", "/n/1/raft/request-vote"]) {
      const response = await fetch(`${base}${path}`, { method: "POST", body: "x" });
      assert.equal(response.status, 404);
    }
    assert.equal(nodes.fakes[0].seen.length, 0);
  });

  it("returns 404 for unknown routes without touching any upstream", async () => {
    await start();
    for (const [method, path] of [
      ["GET", "/nope"],
      ["GET", "/kv/key"],
      ["POST", "/launcher/cluster/stop"],
      ["GET", "/n/9/status"],
    ])
      assert.equal((await fetch(`${base}${path}`, { method })).status, 404, `${method} ${path}`);
    assert.equal(launcher.seen.length, 0);
    assert.ok(nodes.fakes.every((fake) => fake.seen.length === 0));
  });

  it("answers /healthz and / itself", async () => {
    await start();
    assert.deepEqual(await (await fetch(`${base}/healthz`)).json(), { ok: true });
    assert.equal((await (await fetch(`${base}/`)).json()).nodes, 3);
    assert.equal(launcher.seen.length, 0);
  });

  it("forwards launcher controls and marks the answer uncacheable", async () => {
    await start();
    const response = await fetch(`${base}/launcher/nodes/node-2/kill`, { method: "POST" });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("cache-control"), "no-store");
    assert.equal(launcher.seen[0].url, "/nodes/node-2/kill");
    assert.equal(launcher.seen[0].method, "POST");
  });

  // ------------------------------------------------------------------ CORS

  it("sets CORS headers for an allowed origin, replacing the node's own", async () => {
    await start();
    const response = await fetch(`${base}/n/1/status`, { headers: { Origin: DASHBOARD } });
    assert.equal(response.headers.get("access-control-allow-origin"), DASHBOARD);
    assert.equal(response.headers.get("vary"), "Origin");
    assert.match(response.headers.get("access-control-expose-headers"), /X-RaftKV-Node/);
  });

  it("gives other origins no CORS headers, even though the node sent a wildcard", async () => {
    await start();
    const response = await fetch(`${base}/n/1/status`, { headers: { Origin: "https://evil.example" } });
    assert.equal(response.headers.get("access-control-allow-origin"), null);
  });

  it("gives nobody CORS headers when no origin is allowed", async () => {
    await start({ allowedOrigins: [] });
    const response = await fetch(`${base}/n/1/status`, { headers: { Origin: DASHBOARD } });
    assert.equal(response.headers.get("access-control-allow-origin"), null);
  });

  it("supports a wildcard origin", async () => {
    await start({ allowedOrigins: ["*"] });
    const response = await fetch(`${base}/n/1/status`, { headers: { Origin: "https://anything.example" } });
    assert.equal(response.headers.get("access-control-allow-origin"), "*");
  });

  it("tolerates a trailing slash in a configured origin", async () => {
    await start({ allowedOrigins: [`${DASHBOARD}/`] });
    const response = await fetch(`${base}/n/1/status`, { headers: { Origin: DASHBOARD } });
    assert.equal(response.headers.get("access-control-allow-origin"), DASHBOARD);
  });

  it("answers a preflight for an allowed route without contacting the node", async () => {
    await start();
    const response = await fetch(`${base}/n/1/kv/greeting`, {
      method: "OPTIONS",
      headers: {
        Origin: DASHBOARD,
        "Access-Control-Request-Method": "PUT",
        "Access-Control-Request-Headers": "content-type",
      },
    });
    assert.equal(response.status, 204);
    assert.equal(response.headers.get("access-control-allow-origin"), DASHBOARD);
    assert.match(response.headers.get("access-control-allow-methods"), /PUT/);
    assert.equal(response.headers.get("access-control-allow-headers"), "content-type");
    assert.equal(nodes.fakes[0].seen.length, 0);
  });

  it("refuses a preflight for a route that is not on the allowlist", async () => {
    await start();
    const response = await fetch(`${base}/n/1/raft/append-entries`, {
      method: "OPTIONS",
      headers: { Origin: DASHBOARD, "Access-Control-Request-Method": "POST" },
    });
    assert.equal(response.status, 404);
  });

  // ---------------------------------------------------------------- limits

  it("rejects a request whose declared body is too large, before forwarding it", async () => {
    await start({ maxBodyBytes: 100 });
    const response = await fetch(`${base}/n/1/kv/big`, { method: "PUT", body: "x".repeat(500) });
    assert.equal(response.status, 413);
    assert.equal(nodes.fakes[0].seen.length, 0);
  });

  it("rejects a chunked body that grows past the limit", async () => {
    await start({ maxBodyBytes: 100 });
    const status = await new Promise((resolve, reject) => {
      const request = http.request(`${base}/n/1/kv/big`, { method: "PUT" }, (response) => {
        response.resume();
        resolve(response.statusCode);
      });
      request.on("error", reject);
      request.write("x".repeat(60));
      request.write("x".repeat(60));
      request.end();
    });
    assert.equal(status, 413);
  });

  it("accepts a body at the limit", async () => {
    await start({ maxBodyBytes: 100 });
    const response = await fetch(`${base}/n/1/kv/ok`, { method: "PUT", body: "x".repeat(100) });
    assert.equal(response.status, 204);
  });

  it("rate limits destructive controls per client, and never forwards the refused ones", async () => {
    await start({ controlPerMinute: 2 });
    const kill = () => fetch(`${base}/launcher/nodes/node-1/kill`, { method: "POST" });

    assert.equal((await kill()).status, 200);
    assert.equal((await kill()).status, 200);
    const refused = await kill();
    assert.equal(refused.status, 429);
    assert.ok(Number(refused.headers.get("retry-after")) >= 1);
    assert.equal(launcher.seen.length, 2);
  });

  it("does not count reads against the control limit", async () => {
    await start({ controlPerMinute: 1 });
    for (let i = 0; i < 5; i += 1)
      assert.equal((await fetch(`${base}/launcher/nodes`)).status, 200);
  });

  it("caps destructive controls across all clients together", async () => {
    await start({ controlPerMinute: 100, globalControlPerMinute: 2, trustHops: 1 });
    const kill = (ip) =>
      fetch(`${base}/launcher/nodes/node-1/kill`, {
        method: "POST",
        headers: { "X-Forwarded-For": ip },
      });
    assert.equal((await kill("1.1.1.1")).status, 200);
    assert.equal((await kill("2.2.2.2")).status, 200);
    assert.equal((await kill("3.3.3.3")).status, 429);
  });

  it("applies a general request limit per client", async () => {
    await start({ requestsPerMinute: 3 });
    for (let i = 0; i < 3; i += 1) assert.equal((await fetch(`${base}/n/1/status`)).status, 200);
    assert.equal((await fetch(`${base}/n/1/status`)).status, 429);
  });

  it("limits clients separately", async () => {
    await start({ requestsPerMinute: 1, trustHops: 1 });
    const get = (ip) => fetch(`${base}/n/1/status`, { headers: { "X-Forwarded-For": ip } });
    assert.equal((await get("1.1.1.1")).status, 200);
    assert.equal((await get("1.1.1.1")).status, 429);
    assert.equal((await get("2.2.2.2")).status, 200);
  });

  // ---------------------------------------------------------------- failure

  it("answers 502 when the node is down", async () => {
    const dead = createGateway({ nodePorts: [1, 1, 1], launcherPort: 1, trustHops: 0 });
    const { port } = await dead.listen(0, "127.0.0.1");
    const response = await fetch(`http://127.0.0.1:${port}/n/1/status`);
    assert.equal(response.status, 502);
    assert.deepEqual(await response.json(), { error: "node unreachable" });
    await dead.close();
  });

  it("answers 504 when the node never replies", async () => {
    const hang = http.createServer(() => {}); // accepts the request, never answers
    await new Promise((resolve) => hang.listen(0, "127.0.0.1", resolve));
    const slow = createGateway({
      nodePorts: [hang.address().port, 1, 1],
      launcherPort: 1,
      trustHops: 0,
      upstreamTimeoutMs: 200,
    });
    const { port } = await slow.listen(0, "127.0.0.1");
    const response = await fetch(`http://127.0.0.1:${port}/n/1/status`);
    assert.equal(response.status, 504);
    await slow.close();
    hang.closeAllConnections();
    await new Promise((resolve) => hang.close(resolve));
  });

  // --------------------------------------------------------- live streams

  /** Opens a stream through the gateway and collects what arrives. */
  function viewer(path, headers = {}) {
    return new Promise((resolve, reject) => {
      const request = http.get(`${base}${path}`, { headers }, (response) => {
        const view = {
          status: response.statusCode,
          headers: response.headers,
          text: "",
          ended: new Promise((done) => response.on("close", done)),
          close: () => request.destroy(),
        };
        response.on("data", (chunk) => (view.text += chunk.toString()));
        resolve(view);
      });
      request.on("error", reject);
    });
  }

  it("sends a viewer the node's state as an event stream", async () => {
    await start();
    const view = await viewer("/n/1/cluster/events", { Origin: DASHBOARD });
    await sleep(100);
    assert.equal(view.status, 200);
    assert.equal(view.headers["content-type"], "text/event-stream");
    assert.equal(view.headers["access-control-allow-origin"], DASHBOARD);
    assert.equal(view.headers["x-accel-buffering"], "no");
    assert.match(view.text, /event: cluster\ndata: \{"n":1\}\n\n/);
    view.close();
  });

  it("shares ONE upstream stream between any number of viewers", async () => {
    await start();
    const views = await Promise.all(Array.from({ length: 8 }, () => viewer("/n/1/cluster/events")));
    await sleep(150);

    assert.equal(nodes.fakes[0].streamConnections, 1, "eight viewers, one connection to the node");
    assert.equal(gateway.viewers, 8);

    nodes.fakes[0].push('event: cluster\ndata: {"n":2}\n\n');
    await sleep(100);
    for (const view of views) {
      assert.match(view.text, /"n":1/);
      assert.match(view.text, /"n":2/);
    }
    views.forEach((view) => view.close());
  });

  it("replays the latest state to a late viewer, never a ping", async () => {
    await start();
    const early = await viewer("/n/1/cluster/events");
    await sleep(100);
    nodes.fakes[0].push('event: cluster\ndata: {"n":2}\n\n');
    nodes.fakes[0].push("event: ping\ndata: \n\n");
    await sleep(100);

    const late = await viewer("/n/1/cluster/events");
    await sleep(100);
    assert.match(late.text, /"n":2/, "gets the latest state");
    assert.doesNotMatch(late.text, /"n":1/, "not an older one");
    assert.doesNotMatch(late.text, /event: ping/, "pings are not replayed");
    assert.equal(nodes.fakes[0].streamConnections, 1);
    early.close();
    late.close();
  });

  it("keeps separate streams for separate nodes and paths", async () => {
    await start();
    const views = await Promise.all([
      viewer("/n/1/cluster/events"),
      viewer("/n/2/cluster/events"),
      viewer("/n/1/events"),
    ]);
    await sleep(150);
    assert.equal(nodes.fakes[0].streamConnections, 2);
    assert.equal(nodes.fakes[1].streamConnections, 1);
    views.forEach((view) => view.close());
  });

  it("ends every viewer's stream when the node goes away", async () => {
    await start();
    const views = await Promise.all([viewer("/n/1/cluster/events"), viewer("/n/1/cluster/events")]);
    await sleep(100);

    nodes.fakes[0].dropStreams();
    await Promise.race([
      Promise.all(views.map((view) => view.ended)),
      sleep(2000).then(() => assert.fail("viewers were not disconnected")),
    ]);
  });

  it("reconnects upstream for the next viewer after the node comes back", async () => {
    await start();
    const first = await viewer("/n/1/cluster/events");
    await sleep(100);
    nodes.fakes[0].dropStreams();
    await first.ended;

    const second = await viewer("/n/1/cluster/events");
    await sleep(100);
    assert.equal(second.status, 200);
    assert.match(second.text, /"n":1/);
    assert.equal(nodes.fakes[0].streamConnections, 2);
    second.close();
  });

  it("closes the upstream stream when the last viewer leaves", async () => {
    await start();
    const a = await viewer("/n/1/cluster/events");
    const b = await viewer("/n/1/cluster/events");
    await sleep(100);

    a.close();
    await sleep(100);
    assert.equal(nodes.fakes[0].streamClosed, 0, "still one viewer, so the stream stays");

    b.close();
    await sleep(200);
    assert.equal(nodes.fakes[0].streamClosed, 1, "no viewers left: the node's thread is freed");
    assert.equal(gateway.viewers, 0);
  });

  it("answers 502 to a stream request when the node is down", async () => {
    const dead = createGateway({ nodePorts: [1, 1, 1], launcherPort: 1, trustHops: 0 });
    const { port } = await dead.listen(0, "127.0.0.1");
    const response = await fetch(`http://127.0.0.1:${port}/n/1/cluster/events`);
    assert.equal(response.status, 502);
    await dead.close();
  });

  it("reports a node that refuses the stream (its thread cap) as 503", async () => {
    nodes.fakes[0].setStreamStatus(503);
    await start();
    const response = await fetch(`${base}/n/1/cluster/events`);
    assert.equal(response.status, 503);
  });

  it("caps concurrent streams per client", async () => {
    await start({ streamsPerClient: 2 });
    const a = await viewer("/n/1/cluster/events");
    const b = await viewer("/n/2/cluster/events");
    const refused = await viewer("/n/3/cluster/events");
    assert.equal(refused.status, 503);
    assert.equal(refused.headers["retry-after"], "5");

    a.close();
    await sleep(150);
    const allowed = await viewer("/n/3/cluster/events");
    assert.equal(allowed.status, 200);
    b.close();
    allowed.close();
  });

  it("caps concurrent streams in total", async () => {
    await start({ maxStreams: 2, streamsPerClient: 50, trustHops: 1 });
    const open = (ip) => viewer("/n/1/cluster/events", { "X-Forwarded-For": ip });
    const a = await open("1.1.1.1");
    const b = await open("2.2.2.2");
    const refused = await open("3.3.3.3");
    assert.equal(refused.status, 503);
    a.close();
    b.close();
  });

  it("drops an upstream stream that has gone silent", async () => {
    await start({ streamIdleMs: 200 });
    const view = await viewer("/n/1/cluster/events");
    await Promise.race([
      view.ended,
      sleep(3000).then(() => assert.fail("a silent stream was not dropped")),
    ]);
  });

  it("closes every stream on shutdown", async () => {
    await start();
    const view = await viewer("/n/1/cluster/events");
    await sleep(100);
    await gateway.close();
    gateway = null;
    await Promise.race([view.ended, sleep(2000).then(() => assert.fail("stream stayed open"))]);
  });
});

// -------------------------------------------------------------------- watchdog

describe("isHealthy", () => {
  const up = { nodes: [{ id: "node-1", running: true }, { id: "node-2", running: true }] };
  const clean = { enabled: true, delayMs: 0, isolated: [], blocked: [] };

  it("is healthy when every node runs and the network is clean", () => {
    assert.equal(isHealthy(up, clean), true);
  });

  it("is unhealthy when a node is down", () => {
    assert.equal(isHealthy({ nodes: [...up.nodes, { id: "node-3", running: false }] }, clean), false);
  });

  it("is unhealthy with delay, an isolated node, or a blocked link", () => {
    assert.equal(isHealthy(up, { ...clean, delayMs: 50 }), false);
    assert.equal(isHealthy(up, { ...clean, isolated: ["node-1"] }), false);
    assert.equal(isHealthy(up, { ...clean, blocked: [["node-1", "node-2"]] }), false);
  });

  it("ignores the network when emulation is off", () => {
    assert.equal(isHealthy(up, { enabled: false }), true);
  });
});

describe("Watchdog", () => {
  /** A launcher whose state the test controls; records every POST. */
  function harness(state) {
    const posts = [];
    const fetchFn = async (url, init = {}) => {
      const path = new URL(url).pathname;
      if (init.method === "POST") {
        posts.push(path);
        return { ok: state.postOk !== false, status: 200 };
      }
      if (state.down) throw new Error("connection refused");
      const body = path === "/nodes" ? state.nodes : state.network;
      return { ok: true, status: 200, json: async () => body };
    };
    return { posts, fetchFn };
  }

  const healthyState = () => ({
    nodes: { nodes: [{ id: "node-1", running: true }, { id: "node-2", running: true }] },
    network: { enabled: true, delayMs: 0, isolated: [], blocked: [] },
  });

  it("reports a healthy cluster and does nothing", async () => {
    const state = healthyState();
    const { posts, fetchFn } = harness(state);
    const watchdog = new Watchdog({ launcherUrl: "http://l", resetAfterMs: 1000, fetchFn });
    assert.equal(await watchdog.tick(), "healthy");
    assert.deepEqual(posts, []);
  });

  it("waits out the grace period before repairing", async () => {
    const state = healthyState();
    state.nodes.nodes[1].running = false;
    const { posts, fetchFn } = harness(state);
    let now = 0;
    const watchdog = new Watchdog({ launcherUrl: "http://l", resetAfterMs: 1000, fetchFn, now: () => now });

    assert.equal(await watchdog.tick(), "degraded");
    now = 999;
    assert.equal(await watchdog.tick(), "degraded");
    assert.deepEqual(posts, [], "nothing is touched during the grace period");
  });

  it("restarts stopped nodes and clears network faults once the grace period ends", async () => {
    const state = healthyState();
    state.nodes.nodes[1].running = false;
    state.network = { enabled: true, delayMs: 500, isolated: [], blocked: [] };
    const { posts, fetchFn } = harness(state);
    let now = 0;
    const logs = [];
    const watchdog = new Watchdog({
      launcherUrl: "http://l/",
      resetAfterMs: 1000,
      fetchFn,
      now: () => now,
      log: (line) => logs.push(line),
    });

    await watchdog.tick();
    now = 1000;
    assert.equal(await watchdog.tick(), "repaired");
    assert.deepEqual(posts, ["/network/reset", "/nodes/node-2/start"]);
    assert.equal(logs.length, 1);
  });

  it("starts the grace period over once the cluster recovers by itself", async () => {
    const state = healthyState();
    state.nodes.nodes[0].running = false;
    const { posts, fetchFn } = harness(state);
    let now = 0;
    const watchdog = new Watchdog({ launcherUrl: "http://l", resetAfterMs: 1000, fetchFn, now: () => now });

    await watchdog.tick();
    now = 800;
    state.nodes.nodes[0].running = true;
    assert.equal(await watchdog.tick(), "healthy");

    now = 1500;
    state.nodes.nodes[0].running = false;
    assert.equal(await watchdog.tick(), "degraded", "a fresh 1000 ms grace period, not the old one");
    assert.deepEqual(posts, []);
  });

  it("does not touch the network when emulation is off", async () => {
    const state = healthyState();
    state.network = { enabled: false };
    state.nodes.nodes[0].running = false;
    const { posts, fetchFn } = harness(state);
    let now = 0;
    const watchdog = new Watchdog({ launcherUrl: "http://l", resetAfterMs: 0, fetchFn, now: () => now });

    assert.equal(await watchdog.tick(), "repaired");
    assert.deepEqual(posts, ["/nodes/node-1/start"]);
  });

  it("says so when the launcher is unreachable, and does not try to repair", async () => {
    const state = healthyState();
    state.down = true;
    const { posts, fetchFn } = harness(state);
    const watchdog = new Watchdog({ launcherUrl: "http://l", resetAfterMs: 0, fetchFn });
    assert.equal(await watchdog.tick(), "unreachable");
    assert.deepEqual(posts, []);
  });

  it("keeps trying when a repair call fails", async () => {
    const state = healthyState();
    state.nodes.nodes[0].running = false;
    state.postOk = false;
    const { fetchFn } = harness(state);
    const logs = [];
    const watchdog = new Watchdog({
      launcherUrl: "http://l",
      resetAfterMs: 0,
      fetchFn,
      log: (line) => logs.push(line),
    });

    assert.equal(await watchdog.tick(), "degraded");
    assert.match(logs.at(-1), /repair failed/);

    state.postOk = true;
    assert.equal(await watchdog.tick(), "repaired", "the next tick retries straight away");
  });
});
