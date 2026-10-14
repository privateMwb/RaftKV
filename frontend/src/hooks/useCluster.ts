import { useEffect, useState } from "react";
import { buildModel, type ClusterModel } from "@/lib/model";
import type { FeedEvent, NodeRef } from "@/lib/types";
import { ClusterWatcher } from "@/lib/watcher";

const MAX_EVENTS = 60;

export interface ClusterState {
  /** Every node, as reported by that node itself, combined. */
  model: ClusterModel;
  /** Newest first. */
  events: FeedEvent[];
}

/**
 * Follows every node's own state stream for as long as the component is mounted.
 * The data is pushed by the nodes; this hook never polls the cluster.
 */
export function useCluster(nodes: NodeRef[]): ClusterState {
  const [state, setState] = useState<ClusterState>(() => ({
    model: buildModel(nodes, {}, {}, 0),
    events: [],
  }));

  useEffect(() => {
    const watcher = new ClusterWatcher({
      nodes,
      onChange: (model, fresh) =>
        setState((current) => ({
          model,
          events:
            fresh.length > 0
              ? [...fresh.reverse(), ...current.events].slice(0, MAX_EVENTS)
              : current.events,
        })),
    });

    watcher.start();
    return () => watcher.stop();
  }, [nodes]);

  return state;
}
