import { useEffect, useRef, useState } from "react";
import { ClusterStream } from "@/lib/clusterStream";
import { deriveEvents } from "@/lib/events";
import type { ClusterView, FeedEvent, NodeRef, NodeState, StreamStatus } from "@/lib/types";

const MAX_EVENTS = 60;

export interface LastSeen {
  state: NodeState;
  at: number;
}

export interface ClusterFeed {
  /** The newest cluster view; null until the first one arrives. */
  view: ClusterView | null;
  status: StreamStatus;
  /** Each node's last report while it was up, kept so a down node still shows its numbers. */
  lastSeen: Record<string, LastSeen>;
  /** Newest first. */
  events: FeedEvent[];
}

/**
 * Subscribes to the cluster stream for as long as the component is mounted. The view is
 * pushed in by the server; this hook never polls.
 */
export function useClusterStream(nodes: NodeRef[]): ClusterFeed {
  const [feed, setFeed] = useState<ClusterFeed>({
    view: null,
    status: { state: "connecting", via: null },
    lastSeen: {},
    events: [],
  });
  const previous = useRef<ClusterView | null>(null);

  useEffect(() => {
    previous.current = null;

    const stream = new ClusterStream({
      nodes,
      onStatus: (status) => setFeed((current) => ({ ...current, status })),
      onView: (view) => {
        const now = Date.now();
        const fresh = deriveEvents(previous.current, view, now);
        previous.current = view;

        setFeed((current) => {
          const lastSeen = { ...current.lastSeen };
          for (const member of view.members)
            if (member.up && member.state) lastSeen[member.id] = { state: member.state, at: now };

          return {
            ...current,
            view,
            lastSeen,
            events: [...fresh.reverse(), ...current.events].slice(0, MAX_EVENTS),
          };
        });
      },
    });

    stream.start();
    return () => stream.stop();
  }, [nodes]);

  return feed;
}
