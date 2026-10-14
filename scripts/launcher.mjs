#!/usr/bin/env node
// RaftKV demo launcher: starts a local cluster of real backend processes and lets the
// dashboard kill and revive them. A browser cannot kill a process, so this tiny
// zero-dependency HTTP service does it on its behalf:
//
//   GET  /nodes                 -> { nodes: [{ id, port, running, pid }], ... }
//   POST /nodes/:id/start       start that node (no-op if it is running)
//   POST /nodes/:id/kill        SIGKILL it: a real crash, nothing gets flushed or closed
//   POST /nodes/:id/stop        SIGTERM it: a graceful shutdown
//   POST /cluster/start|stop    all nodes at once
//   GET  /network               -> { enabled, delayMs, isolated: [ids], blocked: [[from, to]] }
//   POST /network/delay?ms=N    one-way delay on every node-to-node link (0-2000)
//   POST /network/isolate/:id   cut every link to and from a node (it keeps running)
//   POST /network/reconnect/:id restore that node's links
//   POST /network/partition?groups=node-1,node-2;node-3   split into groups
//   POST /network/heal          remove every partition (delay stays)
//   POST /network/reset         remove partitions and delay
//   GET  /health
//
// Node-to-node traffic goes through proxies owned by this process (see network.mjs),
// which is how delay and partitions are injected into real traffic. The dashboard's
// own connections to the nodes bypass them, so it always sees each node's own truth.
//
// Only the nodes it was configured with can be controlled; there is no way to run
// arbitrary commands through it. It listens on 127.0.0.1 unless told otherwise.

import { spawn } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { NetworkEmulator } from "./network.mjs";

const repoRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "..",
);

const USAGE = `usage: node scripts/launcher.mjs [options]
  --nodes N           cluster size (default 3)
  --base-port P       node i listens on P+i-1 (default 8101)
  --binary PATH       backend binary (default backend/build/raftkv_backend)
  --data DIR          data root, one subfolder per node (default backend/data/demo)
  --port P            launcher port (default 8099)
  --host H            launcher bind address (default 127.0.0.1; 0.0.0.0 exposes it to the LAN)
  --proxy-base P      first port of the node-to-node proxies (default base-port + 100)
  --no-network        no proxies: nodes talk directly (disables delay and partitions)
  --public-host H     host clients use to reach the nodes, for redirect links (default 127.0.0.1)
  --cors-origin O     value passed to nodes as --cors-origin (default *)
  --fresh             wipe the data root first (start from an empty cluster)
  --no-autostart      don't start the nodes at launch`;

function parseArgs(argv) {
  const options = {
    nodes: 3,
    basePort: 8101,
    binary: path.join(repoRoot, "backend/build/raftkv_backend"),
    data: path.join(repoRoot, "backend/data/demo"),
    port: 8099,
    host: "127.0.0.1",
    proxyBase: null,
    network: true,
    publicHost: "127.0.0.1",
    corsOrigin: "*",
    fresh: false,
    autostart: true,
  };

  const number = (name, value, min, max) => {
    const parsed = Number.parseInt(value, 10);
    if (!Number.isInteger(parsed) || parsed < min || parsed > max)
      throw new Error(`${name} must be an integer from ${min} to ${max}`);
    return parsed;
  };

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    const value = () => {
      if (i + 1 >= argv.length) throw new Error(`${arg} needs a value`);
      i += 1;
      return argv[i];
    };

    if (arg === "--nodes") options.nodes = number(arg, value(), 1, 9);
    else if (arg === "--base-port")
      options.basePort = number(arg, value(), 1024, 65000);
    else if (arg === "--binary") options.binary = path.resolve(value());
    else if (arg === "--data") options.data = path.resolve(value());
    else if (arg === "--port") options.port = number(arg, value(), 1, 65535);
    else if (arg === "--host") options.host = value();
    else if (arg === "--proxy-base")
      options.proxyBase = number(arg, value(), 1024, 65000);
    else if (arg === "--no-network") options.network = false;
    else if (arg === "--public-host") options.publicHost = value();
    else if (arg === "--cors-origin") options.corsOrigin = value();
    else if (arg === "--fresh") options.fresh = true;
    else if (arg === "--no-autostart") options.autostart = false;
    else if (arg === "--help" || arg === "-h") {
      console.log(USAGE);
      process.exit(0);
    } else throw new Error(`unknown option ${arg}`);
  }

  return options;
}

let options;
try {
  options = parseArgs(process.argv.slice(2));
} catch (error) {
  console.error(`launcher: ${error.message}\n${USAGE}`);
  process.exit(2);
}

options.proxyBase ??= options.basePort + 100;

// ---------------------------------------------------------------- nodes

const nodes = Array.from({ length: options.nodes }, (_, index) => ({
  id: `node-${index + 1}`,
  port: options.basePort + index,
  child: null,
}));

const log = (message) => console.log(`[launcher] ${message}`);

const emulator = options.network
  ? new NetworkEmulator({
      nodes: nodes.map(({ id, port }) => ({ id, port })),
      proxyBase: options.proxyBase,
    })
  : null;

function argsFor(node) {
  const args = [
    "--id",
    node.id,
    "--port",
    String(node.port),
    "--data",
    path.join(options.data, node.id),
    "--no-repl",
    "--cors-origin",
    options.corsOrigin,
  ];

  // Through the proxy for this link when emulating the network, else straight to the peer.
  for (const peer of nodes) {
    if (peer.id === node.id) continue;
    const port = emulator ? emulator.proxyPort(node.id, peer.id) : peer.port;
    args.push("--peer", `${peer.id}=127.0.0.1:${port}`);
    // Peers reach each other via the proxies, but a client following a redirect needs
    // the node's real address, so say so explicitly.
    args.push(
      "--public-url",
      `${peer.id}=http://${options.publicHost}:${peer.port}`,
    );
  }

  return args;
}

function describe(node) {
  const running =
    node.child !== null &&
    node.child.exitCode === null &&
    node.child.signalCode === null;
  return {
    id: node.id,
    port: node.port,
    running,
    pid: running ? node.child.pid : null,
  };
}

function startNode(node) {
  if (describe(node).running) return { started: false };

  if (!fs.existsSync(options.binary))
    throw new Error(
      `backend binary not found: ${options.binary} (build it first)`,
    );

  fs.mkdirSync(options.data, { recursive: true });
  const child = spawn(options.binary, argsFor(node), {
    stdio: ["ignore", "pipe", "pipe"],
  });
  node.child = child;

  const relay = (stream) => {
    let pending = "";
    stream.on("data", (chunk) => {
      pending += chunk.toString();
      const lines = pending.split("\n");
      pending = lines.pop();
      for (const line of lines)
        if (line.trim()) console.log(`[${node.id}] ${line}`);
    });
  };
  relay(child.stdout);
  relay(child.stderr);

  child.on("error", (error) =>
    log(`${node.id} failed to start: ${error.message}`),
  );
  child.on("exit", (code, signal) =>
    log(`${node.id} exited (${signal ?? `code ${code}`})`),
  );

  log(`started ${node.id} on :${node.port} (pid ${child.pid})`);
  return { started: true };
}

function signalNode(node, signal) {
  if (!describe(node).running) return { signalled: false };
  node.child.kill(signal);
  log(`sent ${signal} to ${node.id}`);
  return { signalled: true };
}

// Resolves once the node's process has exited, or after `ms` regardless, so a
// kill/stop request can answer with the node's real state instead of a stale one.
function exited(node, ms) {
  if (!describe(node).running) return Promise.resolve();
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, ms);
    node.child.once("exit", () => {
      clearTimeout(timer);
      resolve();
    });
  });
}

// ---------------------------------------------------------------- http

function send(res, status, body) {
  res.writeHead(status, {
    "Content-Type": "application/json",
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
    "Access-Control-Allow-Private-Network": "true",
    "Cache-Control": "no-store",
  });
  res.end(JSON.stringify(body));
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url ?? "/", "http://launcher");

  if (req.method === "OPTIONS") return send(res, 204, {});

  try {
    if (req.method === "GET" && url.pathname === "/health")
      return send(res, 200, { ok: true });

    if (req.method === "GET" && url.pathname === "/nodes")
      return send(res, 200, {
        nodes: nodes.map(describe),
        binary: options.binary,
        dataDir: options.data,
      });

    if (req.method === "GET" && url.pathname === "/network")
      return send(res, 200, emulator ? emulator.state() : { enabled: false });

    if (req.method === "POST" && url.pathname.startsWith("/network/")) {
      if (!emulator)
        return send(res, 409, {
          error: "network emulation is disabled (--no-network)",
        });

      const parts = url.pathname.split("/").filter(Boolean); // ["network", action, id?]
      const [, action, id] = parts;

      try {
        if (action === "delay")
          emulator.setDelay(Number(url.searchParams.get("ms")));
        else if (action === "isolate" && id)
          emulator.isolate(decodeURIComponent(id));
        else if (action === "reconnect" && id)
          emulator.reconnect(decodeURIComponent(id));
        else if (action === "heal") emulator.heal();
        else if (action === "reset") emulator.reset();
        else if (action === "partition") {
          const groups = (url.searchParams.get("groups") ?? "")
            .split(";")
            .map((group) =>
              group
                .split(",")
                .map((name) => name.trim())
                .filter(Boolean),
            )
            .filter((group) => group.length > 0);
          if (groups.length < 2)
            return send(res, 400, { error: "need at least two groups" });
          emulator.partition(groups);
        } else return send(res, 404, { error: "not found" });
      } catch (error) {
        return send(res, 400, { error: error.message });
      }

      log(`network: ${JSON.stringify(emulator.state())}`);
      return send(res, 200, emulator.state());
    }

    if (req.method === "POST") {
      const cluster = /^\/cluster\/(start|stop)$/.exec(url.pathname);
      if (cluster) {
        for (const node of nodes) {
          if (cluster[1] === "start") startNode(node);
          else signalNode(node, "SIGTERM");
        }
        if (cluster[1] === "stop")
          await Promise.all(nodes.map((node) => exited(node, 1500)));
        return send(res, 200, { nodes: nodes.map(describe) });
      }

      const match = /^\/nodes\/([^/]+)\/(start|kill|stop)$/.exec(url.pathname);
      if (match) {
        const node = nodes.find(
          (candidate) => candidate.id === decodeURIComponent(match[1]),
        );
        if (!node) return send(res, 404, { error: "unknown node" });

        if (match[2] === "start") startNode(node);
        else {
          signalNode(node, match[2] === "kill" ? "SIGKILL" : "SIGTERM");
          await exited(node, 1500);
        }

        return send(res, 200, describe(node));
      }
    }

    return send(res, 404, { error: "not found" });
  } catch (error) {
    return send(res, 500, { error: error.message });
  }
});

// ---------------------------------------------------------------- lifecycle

let shuttingDown = false;
function shutdown() {
  if (shuttingDown) return;
  shuttingDown = true;
  log("stopping nodes...");

  for (const node of nodes) signalNode(node, "SIGTERM");
  server.close();
  void emulator?.stop();

  // Anything that ignores SIGTERM for a second gets SIGKILL, so no node outlives us.
  setTimeout(() => {
    for (const node of nodes) signalNode(node, "SIGKILL");
    process.exit(0);
  }, 1000).unref();

  const poll = setInterval(() => {
    if (nodes.every((node) => !describe(node).running)) {
      clearInterval(poll);
      process.exit(0);
    }
  }, 50);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

if (options.fresh) {
  fs.rmSync(options.data, { recursive: true, force: true });
  log(`wiped ${options.data}`);
}

server.on("error", (error) => {
  console.error(
    `launcher: cannot listen on ${options.host}:${options.port}: ${error.message}`,
  );
  process.exit(1);
});

try {
  await emulator?.start();
} catch (error) {
  console.error(`launcher: cannot start the network proxies: ${error.message}`);
  process.exit(1);
}

server.listen(options.port, options.host, () => {
  log(
    `controlling ${nodes.length} nodes (:${nodes[0].port}-${nodes.at(-1).port}) via http://${options.host}:${options.port}`,
  );
  log(
    emulator
      ? `node-to-node traffic goes through proxies on :${options.proxyBase}+ (delay and partitions work)`
      : "network emulation off (--no-network)",
  );
  if (options.host !== "127.0.0.1" && options.host !== "localhost")
    log(
      "WARNING: anyone who can reach this address can kill and start your nodes",
    );

  if (options.autostart) {
    try {
      for (const node of nodes) startNode(node);
    } catch (error) {
      console.error(`launcher: ${error.message}`);
      process.exit(1);
    }
  }
});
