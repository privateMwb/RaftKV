#!/usr/bin/env node
// RaftKV hosted-demo server: one process to run in a container.
//
//   - starts the launcher, which runs three real backend nodes on localhost,
//   - puts the gateway on the platform's single public port ($PORT),
//   - restarts the launcher if it dies,
//   - heals the cluster if visitors leave it broken (see watchdog.mjs).
//
// The launcher and the nodes listen on 127.0.0.1 only; the gateway is the one way in, and
// it forwards an allowlist of routes (see gateway.mjs).
//
// Configuration, all through environment variables:
//
//   PORT                  public port (default 10000, which is what Render expects)
//   PUBLIC_BASE_URL       the demo's public URL, used in redirect links
//                         (default $RENDER_EXTERNAL_URL, else http://127.0.0.1:$PORT)
//   ALLOWED_ORIGINS       comma-separated origins allowed to call the gateway from a
//                         browser, e.g. https://raftkv.vercel.app ("*" = any;
//                         unset = no cross-origin access at all)
//   BINARY                backend binary (default backend/build/raftkv_backend)
//   DATA_DIR              node data, wiped on every start (default <tmp>/raftkv-demo)
//   NODES                 cluster size (default 3)
//   RESET_AFTER_SECONDS   heal a broken cluster after this long (default 180)
//   TRUST_PROXY_HOPS      proxies in front of the gateway (default 1: Render's)

import { spawn } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createGateway } from "./gateway.mjs";
import { Watchdog } from "./watchdog.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const env = process.env;

function integer(name, fallback, min, max) {
  const raw = env[name];
  if (raw === undefined || raw === "") return fallback;
  const value = Number.parseInt(raw, 10);
  if (!Number.isInteger(value) || value < min || value > max) {
    console.error(`demo-server: ${name} must be an integer from ${min} to ${max}, got "${raw}"`);
    process.exit(2);
  }
  return value;
}

const port = integer("PORT", 10000, 1, 65535);
const nodeCount = integer("NODES", 3, 1, 9);
const basePort = 8101;
const launcherPort = 8099;
const config = {
  port,
  nodeCount,
  basePort,
  launcherPort,
  publicBaseUrl: (
    env.PUBLIC_BASE_URL ||
    env.RENDER_EXTERNAL_URL ||
    `http://127.0.0.1:${port}`
  ).replace(/\/+$/, ""),
  allowedOrigins: (env.ALLOWED_ORIGINS ?? "").split(",").map((s) => s.trim()).filter(Boolean),
  binary: env.BINARY || path.join(here, "..", "backend/build/raftkv_backend"),
  dataDir: env.DATA_DIR || path.join(os.tmpdir(), "raftkv-demo"),
  resetAfterSeconds: integer("RESET_AFTER_SECONDS", 180, 10, 86_400),
  trustHops: integer("TRUST_PROXY_HOPS", 1, 0, 5),
};

const log = (message) => console.log(`[demo] ${message}`);

// ------------------------------------------------------------ launcher supervisor

let launcher = null;
let stopping = false;
let restartDelay = 1000;

function startLauncher() {
  const child = spawn(
    process.execPath,
    [
      path.join(here, "launcher.mjs"),
      "--fresh",
      "--nodes", String(config.nodeCount),
      "--base-port", String(config.basePort),
      "--port", String(config.launcherPort),
      "--host", "127.0.0.1",
      "--binary", config.binary,
      "--data", config.dataDir,
      "--public-base-url", config.publicBaseUrl,
    ],
    { stdio: ["ignore", "inherit", "inherit"] },
  );
  launcher = child;
  child.on("exit", (code, signal) => {
    if (stopping) return;
    log(`launcher exited (${signal ?? `code ${code}`}); restarting in ${restartDelay} ms`);
    setTimeout(startLauncher, restartDelay);
    restartDelay = Math.min(restartDelay * 2, 15_000);
  });
}

async function waitForLauncher() {
  for (let attempt = 0; attempt < 60; attempt += 1) {
    try {
      const response = await fetch(`http://127.0.0.1:${config.launcherPort}/health`, {
        signal: AbortSignal.timeout(1000),
      });
      if (response.ok) return;
    } catch {
      // not up yet
    }
    await new Promise((resolve) => setTimeout(resolve, 500));
  }
  throw new Error("the launcher did not come up within 30 s");
}

// ------------------------------------------------------------ main

startLauncher();
try {
  await waitForLauncher();
} catch (error) {
  console.error(`demo-server: ${error.message}`);
  process.exit(1);
}
restartDelay = 1000;

const gateway = createGateway({
  nodePorts: Array.from({ length: config.nodeCount }, (_, index) => config.basePort + index),
  launcherPort: config.launcherPort,
  allowedOrigins: config.allowedOrigins,
  trustHops: config.trustHops,
  log,
});

try {
  await gateway.listen(config.port);
} catch (error) {
  console.error(`demo-server: cannot listen on :${config.port}: ${error.message}`);
  process.exit(1);
}

const watchdog = new Watchdog({
  launcherUrl: `http://127.0.0.1:${config.launcherPort}`,
  resetAfterMs: config.resetAfterSeconds * 1000,
  log,
});
watchdog.start(10_000);

log(`gateway on :${config.port}, public URL ${config.publicBaseUrl}`);
log(
  config.allowedOrigins.length > 0
    ? `browser access allowed from: ${config.allowedOrigins.join(", ")}`
    : "ALLOWED_ORIGINS is not set: browsers on other origins cannot reach this demo",
);
log(`a broken cluster is healed after ${config.resetAfterSeconds} s`);

// ------------------------------------------------------------ shutdown

async function shutdown() {
  if (stopping) return;
  stopping = true;
  log("shutting down");
  watchdog.stop();
  await gateway.close();
  launcher?.kill("SIGTERM");
  setTimeout(() => process.exit(0), 3000).unref();
  launcher?.once("exit", () => process.exit(0));
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
