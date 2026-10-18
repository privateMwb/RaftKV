// RaftKV demo gateway: puts a whole local cluster behind ONE public port.
//
// A hosting platform such as Render exposes a single port per service, but the
// dashboard talks to every node and to the launcher separately. This zero-dependency
// reverse proxy listens on that one port and routes:
//
//   /n/<k>/kv/<key>           GET PUT DELETE   node k's client API
//   /n/<k>/status             GET              node k's state
//   /n/<k>/cluster            GET              node k's view of the cluster
//   /n/<k>/events             GET (stream)     node k's own state, live
//   /n/<k>/cluster/events     GET (stream)     node k's view of the cluster, live
//   /launcher/nodes           GET              which nodes are running
//   /launcher/network         GET              current network faults
//   /launcher/nodes/<id>/(start|kill|stop)     POST   crash or revive one node
//   /launcher/network/...                      POST   delay, isolate, partition, heal
//   /healthz                  GET              liveness for the platform's health check
//
// Everything else is a 404. In particular the nodes' peer-to-peer RPC routes
// (/raft/...) are NOT reachable: only the routes the dashboard actually uses are
// forwarded, so a stranger can use the public demo the way the dashboard does and
// nothing more. This is an allowlist, not a blocklist.
//
// Live streams are fanned out. The backend pins one server thread per open stream and
// allows only half its pool to be streams, so it would refuse the fifth viewer. The
// gateway keeps ONE upstream stream per node and path, replays the latest state to each
// new viewer, and forwards every later event to all of them.
//
// CORS: the dashboard is served from another origin (for example Vercel), so only the
// origins in `allowedOrigins` get CORS headers. The nodes' and launcher's own CORS
// headers are removed and replaced, never duplicated.
//
// Abuse limits (all per client, plus a global cap on the destructive controls): request
// rate, control-action rate, concurrent streams, and request body size.

import http from "node:http";

const HOP_BY_HOP = new Set([
  "connection",
  "keep-alive",
  "proxy-authenticate",
  "proxy-authorization",
  "te",
  "trailer",
  "transfer-encoding",
  "upgrade",
]);

// Only these request headers are forwarded upstream; the rest (cookies, auth, ...) are not.
const FORWARDED_REQUEST_HEADERS = ["content-type", "content-length", "accept"];

const EXPOSED_HEADERS = "X-RaftKV-Node, Retry-After, Location";

// ------------------------------------------------------------------ routing

const NODE_ROUTE = /^\/n\/(\d+)(\/.*)$/;
const KV_PATH = /^\/kv\/[^/]+$/;
const LAUNCHER_NODE_ACTION = /^\/nodes\/node-\d+\/(start|kill|stop)$/;
const LAUNCHER_NETWORK_ID_ACTION = /^\/network\/(isolate|reconnect)\/node-\d+$/;
const LAUNCHER_NETWORK_PLAIN = new Set([
  "/network/delay",
  "/network/partition",
  "/network/heal",
  "/network/reset",
]);

/**
 * Decides what a request is allowed to reach. Returns null for anything not on the
 * allowlist. `pathname` must already be normalised (as `new URL().pathname` is).
 */
export function classify(method, pathname, nodeCount) {
  if (pathname === "/" && method === "GET") return { kind: "root" };
  if (pathname === "/healthz" && method === "GET") return { kind: "health" };

  const node = NODE_ROUTE.exec(pathname);
  if (node) {
    const index = Number(node[1]);
    const path = node[2];
    if (index < 1 || index > nodeCount) return null;

    if (method === "GET" && (path === "/status" || path === "/cluster"))
      return { kind: "node", index, path };
    if (method === "GET" && (path === "/events" || path === "/cluster/events"))
      return { kind: "stream", index, path };
    if (KV_PATH.test(path) && (method === "GET" || method === "PUT" || method === "DELETE"))
      return { kind: "node", index, path };
    return null;
  }

  if (pathname.startsWith("/launcher/")) {
    const path = pathname.slice("/launcher".length);
    if (method === "GET" && (path === "/nodes" || path === "/network"))
      return { kind: "launcher", path, control: false };
    if (
      method === "POST" &&
      (LAUNCHER_NODE_ACTION.test(path) ||
        LAUNCHER_NETWORK_ID_ACTION.test(path) ||
        LAUNCHER_NETWORK_PLAIN.has(path))
    )
      return { kind: "launcher", path, control: true };
  }

  return null;
}

// ------------------------------------------------------------------ limits

/** At most `limit` hits per key in each `windowMs` window. */
export class FixedWindowLimiter {
  #limit;
  #windowMs;
  #windows = new Map();
  #sinceSweep = 0;

  constructor({ limit, windowMs }) {
    this.#limit = limit;
    this.#windowMs = windowMs;
  }

  /** Counts one hit. `retryAfterSec` is how long until the key may try again. */
  take(key, now = Date.now()) {
    if (++this.#sinceSweep >= 500) this.#sweep(now);

    let window = this.#windows.get(key);
    if (!window || now - window.start >= this.#windowMs) {
      window = { start: now, count: 0 };
      this.#windows.set(key, window);
    }

    if (window.count >= this.#limit)
      return {
        ok: false,
        retryAfterSec: Math.max(1, Math.ceil((window.start + this.#windowMs - now) / 1000)),
      };
    window.count += 1;
    return { ok: true, retryAfterSec: 0 };
  }

  #sweep(now) {
    this.#sinceSweep = 0;
    for (const [key, window] of this.#windows)
      if (now - window.start >= this.#windowMs) this.#windows.delete(key);
  }
}

/**
 * Who is making this request. Behind `trustHops` trusted proxies the client is that many
 * entries from the END of X-Forwarded-For (earlier entries are supplied by the client and
 * can be forged). With no trusted proxy the connection's own address is used.
 */
export function clientIp(req, trustHops = 1) {
  if (trustHops > 0) {
    const chain = String(req.headers["x-forwarded-for"] ?? "")
      .split(",")
      .map((part) => part.trim())
      .filter(Boolean);
    if (chain.length >= trustHops) return chain[chain.length - trustHops];
  }
  return req.socket.remoteAddress ?? "unknown";
}

// ------------------------------------------------------------------ CORS

function normaliseOrigins(list) {
  return list.map((origin) => origin.trim().replace(/\/+$/, "")).filter(Boolean);
}

function corsHeaders(allowedOrigins, origin) {
  if (allowedOrigins.includes("*"))
    return { "Access-Control-Allow-Origin": "*", "Access-Control-Expose-Headers": EXPOSED_HEADERS };
  if (origin && allowedOrigins.includes(origin))
    return {
      "Access-Control-Allow-Origin": origin,
      Vary: "Origin",
      "Access-Control-Expose-Headers": EXPOSED_HEADERS,
    };
  return {};
}

// ------------------------------------------------------------------ responses

function sendJson(res, status, body, extra = {}) {
  if (res.headersSent) {
    res.end();
    return;
  }
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    "Content-Type": "application/json",
    "Content-Length": Buffer.byteLength(payload),
    "Cache-Control": "no-store",
    ...extra,
  });
  res.end(payload);
}

// ------------------------------------------------------------------ live streams

/**
 * One upstream event stream per (node, path), shared by every viewer. Viewers get the
 * latest state immediately, then every later event. When the node goes away every viewer's
 * stream is ended (so the dashboard sees the node as down, exactly as it would directly);
 * when the last viewer leaves the upstream is closed, which frees the node's thread.
 */
class StreamHub {
  #entries = new Map();
  #idleMs;
  #closed = false;

  constructor({ idleMs }) {
    this.#idleMs = idleMs;
  }

  /** Number of viewers currently attached or waiting, across every stream. */
  get viewers() {
    let total = 0;
    for (const entry of this.#entries.values()) total += entry.subscribers.size + entry.waiting.length;
    return total;
  }

  subscribe({ key, port, path, res, cors }) {
    let entry = this.#entries.get(key);
    if (!entry) entry = this.#open(key, port, path);

    if (entry.live) this.#attach(entry, res, cors);
    else entry.waiting.push({ res, cors });

    res.on("close", () => this.#detach(entry, res));
  }

  /** Closes every upstream stream and every viewer's stream (used on shutdown). */
  closeAll() {
    this.#closed = true;
    for (const entry of [...this.#entries.values()]) this.#closeEntry(entry);
  }

  #open(key, port, path) {
    const entry = {
      key,
      live: false,
      closed: false,
      subscribers: new Set(),
      waiting: [],
      last: null,
      buffer: "",
      lastDataAt: Date.now(),
      upstream: null,
      watchdog: null,
    };
    this.#entries.set(key, entry);

    const upstream = http.request({
      host: "127.0.0.1",
      port,
      path,
      method: "GET",
      agent: false,
      headers: { accept: "text/event-stream" },
    });
    entry.upstream = upstream;

    upstream.on("response", (response) => {
      if (entry.closed) return response.destroy();
      if (response.statusCode !== 200) {
        response.resume();
        return this.#fail(entry, response.statusCode === 503 ? 503 : 502);
      }

      entry.live = true;
      entry.lastDataAt = Date.now();
      for (const { res, cors } of entry.waiting.splice(0)) this.#attach(entry, res, cors);

      response.on("data", (chunk) => this.#onData(entry, chunk));
      response.on("end", () => this.#closeEntry(entry));
      response.on("error", () => this.#closeEntry(entry));
      response.on("close", () => this.#closeEntry(entry));

      // The node pings every 2 s; silence means the connection is dead even if the socket
      // is not, so drop it and let viewers reconnect.
      entry.watchdog = setInterval(() => {
        if (Date.now() - entry.lastDataAt > this.#idleMs) this.#closeEntry(entry);
      }, 1000);
      entry.watchdog.unref();
    });

    upstream.on("error", () => (entry.live ? this.#closeEntry(entry) : this.#fail(entry, 502)));
    upstream.end();
    return entry;
  }

  #attach(entry, res, cors) {
    if (res.destroyed || res.writableEnded) return;
    res.writeHead(200, {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache, no-transform",
      Connection: "keep-alive",
      "X-Accel-Buffering": "no",
      ...cors,
    });
    if (entry.last) res.write(entry.last);
    entry.subscribers.add(res);
  }

  #onData(entry, chunk) {
    entry.lastDataAt = Date.now();
    entry.buffer += chunk.toString("utf8");

    let end;
    while ((end = entry.buffer.indexOf("\n\n")) !== -1) {
      const message = entry.buffer.slice(0, end + 2);
      entry.buffer = entry.buffer.slice(end + 2);

      // Pings keep the connection alive but carry no state to replay to a new viewer.
      if (!/^event: ping$/m.test(message) && !message.startsWith(":")) entry.last = message;

      for (const res of entry.subscribers) {
        if (res.writableLength > 1_048_576) res.destroy();
        else res.write(message);
      }
    }
  }

  #fail(entry, status) {
    const waiting = entry.waiting.splice(0);
    this.#closeEntry(entry);
    for (const { res, cors } of waiting)
      sendJson(res, status, { error: "node unreachable" }, cors);
  }

  #detach(entry, res) {
    entry.subscribers.delete(res);
    entry.waiting = entry.waiting.filter((waiter) => waiter.res !== res);
    if (entry.subscribers.size === 0 && entry.waiting.length === 0) this.#closeEntry(entry);
  }

  #closeEntry(entry) {
    if (entry.closed) return;
    entry.closed = true;
    if (this.#entries.get(entry.key) === entry) this.#entries.delete(entry.key);
    clearInterval(entry.watchdog);
    entry.upstream?.destroy();
    for (const res of entry.subscribers) res.end();
    entry.subscribers.clear();
    for (const { res } of entry.waiting.splice(0)) res.end();
  }
}

// ------------------------------------------------------------------ gateway

/**
 * @param {object} options
 * @param {number[]} options.nodePorts      node k (1-based) listens on nodePorts[k-1]
 * @param {number}   options.launcherPort   the launcher's port
 * @param {string[]} [options.allowedOrigins]  origins that get CORS headers ("*" = any)
 * @param {number}   [options.trustHops]    trusted proxies in front of the gateway (default 1)
 * @param {number}   [options.maxBodyBytes] largest request body forwarded (default 70 KiB)
 * @param {number}   [options.requestsPerMinute]       per client, all proxied requests
 * @param {number}   [options.controlPerMinute]        per client, destructive launcher actions
 * @param {number}   [options.globalControlPerMinute]  all clients together
 * @param {number}   [options.streamsPerClient]        concurrent live streams per client
 * @param {number}   [options.maxStreams]              concurrent live streams in total
 * @param {number}   [options.upstreamTimeoutMs]
 * @param {number}   [options.streamIdleMs]
 * @param {(line: string) => void} [options.log]
 */
export function createGateway(options) {
  const {
    nodePorts,
    launcherPort,
    allowedOrigins: rawOrigins = [],
    trustHops = 1,
    maxBodyBytes = 70 * 1024,
    requestsPerMinute = 600,
    controlPerMinute = 20,
    globalControlPerMinute = 120,
    streamsPerClient = 12,
    maxStreams = 300,
    upstreamTimeoutMs = 10_000,
    streamIdleMs = 8000,
    log = () => {},
  } = options;

  const allowedOrigins = normaliseOrigins(rawOrigins);
  const general = new FixedWindowLimiter({ limit: requestsPerMinute, windowMs: 60_000 });
  const control = new FixedWindowLimiter({ limit: controlPerMinute, windowMs: 60_000 });
  const controlAll = new FixedWindowLimiter({ limit: globalControlPerMinute, windowMs: 60_000 });
  const hub = new StreamHub({ idleMs: streamIdleMs });
  const streamsByClient = new Map();

  function proxy(req, res, { port, path, search, cors, ip, noStore }) {
    const headers = { "x-forwarded-for": ip };
    for (const name of FORWARDED_REQUEST_HEADERS)
      if (req.headers[name] !== undefined) headers[name] = req.headers[name];

    const declared = Number(req.headers["content-length"] ?? 0);
    if (declared > maxBodyBytes) {
      req.resume();
      return sendJson(res, 413, { error: "request body too large" }, cors);
    }

    let timedOut = false;
    let tooLarge = false;
    const upstream = http.request(
      {
        host: "127.0.0.1",
        port,
        path: path + search,
        method: req.method,
        agent: false,
        headers,
      },
      (response) => {
        const out = {};
        for (const [name, value] of Object.entries(response.headers)) {
          if (HOP_BY_HOP.has(name) || name.startsWith("access-control-")) continue;
          out[name] = value;
        }
        if (noStore) out["cache-control"] = "no-store";
        res.writeHead(response.statusCode ?? 502, { ...out, ...cors });
        response.pipe(res);
      },
    );

    upstream.setTimeout(upstreamTimeoutMs, () => {
      timedOut = true;
      upstream.destroy();
    });
    upstream.on("error", () => {
      if (tooLarge) return;
      sendJson(
        res,
        timedOut ? 504 : 502,
        { error: timedOut ? "upstream timed out" : "node unreachable" },
        cors,
      );
    });

    let received = 0;
    req.on("data", (chunk) => {
      received += chunk.length;
      if (received > maxBodyBytes && !tooLarge) {
        tooLarge = true;
        upstream.destroy();
        sendJson(res, 413, { error: "request body too large" }, cors);
      }
    });
    req.on("aborted", () => upstream.destroy());
    req.pipe(upstream);
  }

  function handle(req, res) {
    let url;
    try {
      url = new URL(req.url ?? "/", "http://gateway");
    } catch {
      return sendJson(res, 400, { error: "bad request" });
    }

    const cors = corsHeaders(allowedOrigins, req.headers.origin);

    if (req.method === "OPTIONS") {
      const wanted = String(req.headers["access-control-request-method"] ?? "");
      if (!classify(wanted, url.pathname, nodePorts.length)) return sendJson(res, 404, { error: "not found" });
      res.writeHead(204, {
        ...cors,
        "Access-Control-Allow-Methods": "GET, PUT, DELETE, POST, OPTIONS",
        "Access-Control-Allow-Headers": req.headers["access-control-request-headers"] ?? "Content-Type",
        "Access-Control-Max-Age": "600",
      });
      return res.end();
    }

    const route = classify(req.method ?? "", url.pathname, nodePorts.length);
    if (!route) return sendJson(res, 404, { error: "not found" }, cors);

    if (route.kind === "root")
      return sendJson(res, 200, { service: "raftkv-demo-gateway", nodes: nodePorts.length }, cors);
    if (route.kind === "health") return sendJson(res, 200, { ok: true });

    const ip = clientIp(req, trustHops);

    if (route.kind === "stream") {
      const count = streamsByClient.get(ip) ?? 0;
      if (count >= streamsPerClient || hub.viewers >= maxStreams) {
        log(`stream limit reached for ${ip}`);
        return sendJson(res, 503, { error: "too many live streams" }, { ...cors, "Retry-After": "5" });
      }
      streamsByClient.set(ip, count + 1);
      res.on("close", () => {
        const left = (streamsByClient.get(ip) ?? 1) - 1;
        if (left <= 0) streamsByClient.delete(ip);
        else streamsByClient.set(ip, left);
      });

      return hub.subscribe({
        key: `${route.index}${route.path}`,
        port: nodePorts[route.index - 1],
        path: route.path,
        res,
        cors,
      });
    }

    const limited = [
      general.take(ip),
      ...(route.kind === "launcher" && route.control ? [control.take(ip), controlAll.take("*")] : []),
    ].find((verdict) => !verdict.ok);
    if (limited) {
      log(`rate limited ${ip} on ${req.method} ${url.pathname}`);
      return sendJson(
        res,
        429,
        { error: "too many requests" },
        { ...cors, "Retry-After": String(limited.retryAfterSec) },
      );
    }

    if (route.kind === "launcher")
      return proxy(req, res, {
        port: launcherPort,
        path: route.path,
        search: url.search,
        cors,
        ip,
        noStore: true,
      });

    return proxy(req, res, {
      port: nodePorts[route.index - 1],
      path: route.path,
      search: url.search,
      cors,
      ip,
      noStore: false,
    });
  }

  const server = http.createServer((req, res) => {
    try {
      handle(req, res);
    } catch (error) {
      log(`gateway error: ${error.message}`);
      sendJson(res, 500, { error: "internal error" });
    }
  });
  server.keepAliveTimeout = 65_000;

  return {
    server,
    /** Number of live-stream viewers right now. */
    get viewers() {
      return hub.viewers;
    },
    listen(port, host = "0.0.0.0") {
      return new Promise((resolve, reject) => {
        server.once("error", reject);
        server.listen(port, host, () => {
          server.off("error", reject);
          resolve(server.address());
        });
      });
    },
    close() {
      hub.closeAll();
      return new Promise((resolve) => {
        server.close(() => resolve());
        server.closeAllConnections();
      });
    },
  };
}
