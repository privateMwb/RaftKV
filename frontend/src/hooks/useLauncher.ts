import { useCallback, useEffect, useState } from "react";
import {
  fetchLauncherNodes,
  fetchNetwork,
  launcherAction,
  networkAction,
  NO_NETWORK,
  type LauncherAction,
  type LauncherNode,
  type NetworkAction,
  type NetworkState,
} from "@/lib/launcher";

const POLL_MS = 3000;

export interface Launcher {
  /** Whether the launcher process answered the last time we asked. */
  online: boolean;
  nodes: Record<string, LauncherNode>;
  /** Node ids with a start/kill request in flight. */
  busy: Record<string, boolean>;
  /** Injected network faults (delay, partitions). `enabled` is false without proxies. */
  network: NetworkState;
  networkBusy: boolean;
  error: string | null;
  act: (id: string, action: LauncherAction) => Promise<void>;
  changeNetwork: (action: NetworkAction) => Promise<void>;
}

/**
 * Talks to the demo launcher (scripts/launcher.mjs), which owns the node processes and
 * the network proxies between them. The launcher is a local dev tool rather than part
 * of the cluster, so checking in every few seconds is the right amount of machinery;
 * the cluster state itself arrives over the push streams instead.
 */
export function useLauncher(url: string): Launcher {
  const [online, setOnline] = useState(false);
  const [nodes, setNodes] = useState<Record<string, LauncherNode>>({});
  const [network, setNetwork] = useState<NetworkState>(NO_NETWORK);
  const [busy, setBusy] = useState<Record<string, boolean>>({});
  const [networkBusy, setNetworkBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      const [list, net] = await Promise.all([
        fetchLauncherNodes(url),
        fetchNetwork(url).catch(() => NO_NETWORK),
      ]);
      setNodes(Object.fromEntries(list.map((node) => [node.id, node])));
      setNetwork(net);
      setOnline(true);
    } catch {
      setOnline(false);
      setNetwork(NO_NETWORK);
    }
  }, [url]);

  useEffect(() => {
    const first = setTimeout(() => void refresh(), 0);
    const timer = setInterval(() => void refresh(), POLL_MS);
    return () => {
      clearTimeout(first);
      clearInterval(timer);
    };
  }, [refresh]);

  const act = useCallback(
    async (id: string, action: LauncherAction) => {
      setBusy((current) => ({ ...current, [id]: true }));
      setError(null);

      try {
        await launcherAction(url, id, action);
      } catch (cause) {
        setError(cause instanceof Error ? cause.message : "launcher request failed");
      } finally {
        await refresh();
        setBusy((current) => ({ ...current, [id]: false }));
      }
    },
    [url, refresh],
  );

  const changeNetwork = useCallback(
    async (action: NetworkAction) => {
      setNetworkBusy(true);
      setError(null);

      try {
        setNetwork(await networkAction(url, action));
      } catch (cause) {
        setError(cause instanceof Error ? cause.message : "network request failed");
        await refresh();
      } finally {
        setNetworkBusy(false);
      }
    },
    [url, refresh],
  );

  return { online, nodes, busy, network, networkBusy, error, act, changeNetwork };
}
