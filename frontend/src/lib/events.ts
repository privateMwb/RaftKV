import type { ClusterModel } from "./model.ts";
import type { FeedEvent, FeedTone } from "./types.ts";

let counter = 0;

function event(at: number, tone: FeedTone, text: string): FeedEvent {
  counter += 1;
  return { id: `${at}-${counter}`, at, tone, text };
}

/**
 * Turns the difference between two consecutive cluster models into human events
 * ("node-3 went down", "node-2 became leader · term 4", "node-1 lost contact with
 * its peers"). The first model only sets the baseline, so it yields nothing.
 */
export function deriveEvents(
  prev: ClusterModel | null,
  next: ClusterModel,
  at: number,
): FeedEvent[] {
  if (!prev) return [];

  const out: FeedEvent[] = [];

  for (const node of next.nodes) {
    const before = prev.nodes.find((candidate) => candidate.id === node.id);
    if (!before) continue;

    if (before.live && !node.live) {
      out.push(event(at, "bad", `${node.id} went down`));
      continue;
    }
    if (!before.live && node.live) {
      // Only a node we had already seen is "back"; on page load everything is new.
      if (before.lastSeenAt !== null) out.push(event(at, "good", `${node.id} is back`));
      continue;
    }
    if (!node.live) continue;

    if (!before.isolated && node.isolated)
      out.push(event(at, "warn", `${node.id} lost contact with its peers`));
    else if (before.isolated && !node.isolated)
      out.push(event(at, "good", `${node.id} reconnected to the cluster`));

    const was = before.state?.role;
    const now = node.state?.role;
    if (!now || was === now) continue;

    if (now === "leader") {
      out.push(event(at, "leader", `${node.id} became leader · term ${node.state?.term}`));
    } else if (now === "candidate") {
      out.push(event(at, "warn", `${node.id} started an election · term ${node.state?.term}`));
    } else if (was === "leader") {
      out.push(event(at, "info", `${node.id} stepped down`));
    }
  }

  // While some node is still on its first connection attempt we don't know the real
  // quorum yet (page load looks like "one node up, then three"), so stay quiet.
  const known = prev.nodes.every((node) => node.stream !== "connecting");

  if (!known) {
    // nothing to report about quorum yet
  } else if (prev.quorum && !next.quorum) {
    out.push(
      event(at, "bad", `quorum lost · ${next.healthyCount} of ${next.nodes.length} healthy`),
    );
  } else if (!prev.quorum && next.quorum) {
    out.push(event(at, "good", "quorum restored"));
  }

  return out;
}
