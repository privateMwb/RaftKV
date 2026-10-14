import type { NodeRef } from "./types.ts";

export interface AppConfig {
  nodes: NodeRef[];
  launcherUrl: string;
}

/** Subset of Vite's import.meta.env that we read (kept as a parameter so it's testable). */
export interface EnvVars {
  VITE_NODE_HOST?: string;
  VITE_NODE_PORTS?: string;
  VITE_LAUNCHER_URL?: string;
}

/**
 * Where the nodes and the launcher are. By default they are on the same host that
 * served this page, so the dashboard works on `localhost` and, opened from a phone,
 * on the computer's LAN address, without configuration.
 */
export function buildConfig(env: EnvVars, pageHostname: string): AppConfig {
  const host = env.VITE_NODE_HOST?.trim() || pageHostname || "localhost";

  const ports = (env.VITE_NODE_PORTS ?? "8101,8102,8103")
    .split(",")
    .map((text) => Number.parseInt(text.trim(), 10))
    .filter((port) => Number.isInteger(port) && port > 0 && port < 65536);

  return {
    nodes: ports.map((port, index) => ({
      id: `node-${index + 1}`,
      url: `http://${host}:${port}`,
    })),
    launcherUrl: (env.VITE_LAUNCHER_URL?.trim() || `http://${host}:8099`).replace(/\/+$/, ""),
  };
}
