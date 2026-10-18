import type { NodeRef } from "./types.ts";

export interface AppConfig {
  nodes: NodeRef[];
  launcherUrl: string;
}

/** Subset of Vite's import.meta.env that we read (kept as a parameter so it's testable). */
export interface EnvVars {
  VITE_NODE_URLS?: string;
  VITE_NODE_HOST?: string;
  VITE_NODE_PORTS?: string;
  VITE_LAUNCHER_URL?: string;
}

/** An absolute http(s) URL with no trailing slash, or null if `text` is not one. */
function cleanUrl(text: string): string | null {
  try {
    const { protocol } = new URL(text.trim());
    return protocol === "http:" || protocol === "https:" ? text.trim().replace(/\/+$/, "") : null;
  } catch {
    return null;
  }
}

/**
 * Where the nodes and the launcher are. By default they are on the same host that
 * served this page, so the dashboard works on `localhost` and, opened from a phone,
 * on the computer's LAN address, without configuration.
 *
 * For a hosted demo, `VITE_NODE_URLS` lists each node's full base URL instead (for
 * example the gateway's `https://demo.example/n/1,https://demo.example/n/2,...`); it
 * wins over the host and ports, and `VITE_LAUNCHER_URL` should point at the gateway's
 * `/launcher`.
 */
export function buildConfig(env: EnvVars, pageHostname: string): AppConfig {
  const host = env.VITE_NODE_HOST?.trim() || pageHostname || "localhost";
  const launcherUrl = (env.VITE_LAUNCHER_URL?.trim() || `http://${host}:8099`).replace(/\/+$/, "");

  const urls = (env.VITE_NODE_URLS ?? "")
    .split(",")
    .map(cleanUrl)
    .filter((url): url is string => url !== null);
  if (urls.length > 0)
    return { nodes: urls.map((url, index) => ({ id: `node-${index + 1}`, url })), launcherUrl };

  const ports = (env.VITE_NODE_PORTS ?? "8101,8102,8103")
    .split(",")
    .map((text) => Number.parseInt(text.trim(), 10))
    .filter((port) => Number.isInteger(port) && port > 0 && port < 65536);

  return {
    nodes: ports.map((port, index) => ({
      id: `node-${index + 1}`,
      url: `http://${host}:${port}`,
    })),
    launcherUrl,
  };
}
