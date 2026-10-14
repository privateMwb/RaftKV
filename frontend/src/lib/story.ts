import { entriesBehind } from "./lanes.ts";
import type { ClusterModel, NodeModel } from "./model.ts";

export type StoryTone = "good" | "warn" | "bad" | "info";

/** A plain-language account of what the cluster is doing right now, and why. */
export interface Story {
  tone: StoryTone;
  title: string;
  body: string;
  /** Something specific the viewer can try next. */
  hint?: string;
  /** An extra remark about the injected network delay, if it matters. */
  note?: string;
}

/** The backend's shortest election timeout (15 ticks of 10 ms). */
export const ELECTION_TIMEOUT_MIN_MS = 150;

function names(nodes: NodeModel[]): string {
  const ids = nodes.map((node) => node.id);
  if (ids.length <= 1) return ids.join("");
  return `${ids.slice(0, -1).join(", ")} and ${ids.at(-1)}`;
}

export type DelayLevel = "ok" | "slow" | "bad";

/**
 * How much a given one-way delay hurts, from measurements of the real cluster:
 * up to ~10 ms it is invisible; at 40 ms commits take 80-160 ms (one to two round
 * trips); from ~80 ms a heartbeat
 * round trip outlasts the shortest election timeout and the cluster cannot hold a leader.
 */
export function delayZone(delayMs: number): { level: DelayLevel; label: string } {
  if (delayMs >= 60) return { level: "bad", label: "Elections churn; writes fail" };
  if (delayMs >= 20) return { level: "slow", label: "Commits get slower" };
  return { level: "ok", label: "Healthy" };
}

function delayNote(delayMs: number): string | undefined {
  if (delayMs >= 60)
    return `Network delay is ${delayMs} ms each way, so a heartbeat round trip (about ${2 * delayMs} ms) is longer than the shortest election timeout (${ELECTION_TIMEOUT_MIN_MS} ms). Followers time out waiting for heartbeats, so expect constant elections and 503s. Data stays safe; availability suffers.`;
  if (delayMs >= 20)
    return `Network delay is ${delayMs} ms each way, so every commit now takes at least one round trip: roughly ${2 * delayMs}–${4 * delayMs} ms (two round trips when a heartbeat is already in flight).`;
  return undefined;
}

export function explain(model: ClusterModel, delayMs = 0): Story {
  const total = model.nodes.length;
  const note = delayNote(delayMs);

  const finish = (story: Omit<Story, "note">): Story => (note ? { ...story, note } : story);

  const everSeen = model.nodes.some((node) => node.state !== null);
  if (model.liveCount === 0) {
    return everSeen
      ? finish({
          tone: "bad",
          title: "No node is reachable",
          body: "The dashboard cannot reach any node. They may all be down, or the launcher stopped them.",
          hint: "Start the cluster with the launcher, then this page reconnects by itself.",
        })
      : finish({
          tone: "info",
          title: "Waiting for the cluster",
          body: "Connecting to the nodes. Each node reports its own state directly to this page, so you see what every node believes, not one node's guess about the others.",
        });
  }

  const isolated = model.nodes.filter((node) => node.isolated);
  const down = model.nodes.filter((node) => !node.live);
  const stale = model.nodes.filter((node) => node.staleLeader && node.live);

  if (!model.quorum) {
    if (isolated.length > 0)
      return finish({
        tone: "bad",
        title: "No group of nodes has a majority",
        body: `The nodes are running but cannot reach enough of each other. Nobody can see ${model.majority} of ${total}, so nobody can commit: writes and linearizable reads are refused rather than risk two nodes disagreeing.`,
        hint: "Heal the network to recover; nothing is lost.",
      });

    return finish({
      tone: "bad",
      title: "No majority, so the cluster is read-only in spirit",
      body: `Only ${model.liveCount} of ${total} nodes are running. Committing needs a majority (${model.majority}), so writes and linearizable reads are refused (503) instead of answering from possibly stale data.`,
      hint: "Revive a node and service resumes with nothing lost.",
    });
  }

  if (stale.length > 0) {
    const leader = model.nodes.find((node) => node.id === model.leader);
    const first = stale[0];
    return finish({
      tone: "warn",
      title: `${first.id} is a stale leader`,
      body: `${first.id} was cut off while it led. It still believes it leads term ${first.state?.term}, but it cannot reach a majority, so nothing it accepts can commit.${
        leader
          ? ` The majority elected ${leader.id} in term ${leader.state?.term}. When ${first.id} hears from it, the higher term makes it step down and discard entries the majority never saw.`
          : ""
      }`,
      hint: `Try a write against ${first.id}: it will be refused (503), never silently lost.`,
    });
  }

  if (model.leader === null) {
    const candidates = model.nodes.filter((node) => node.live && node.state?.role === "candidate");
    return finish({
      tone: "warn",
      title: "Election in progress",
      body: `There is no leader right now.${
        candidates.length > 0
          ? ` ${names(candidates)} ${candidates.length > 1 ? "are" : "is"} campaigning in term ${candidates[0].state?.term}.`
          : ""
      } A follower that stops hearing heartbeats waits a random 150–300 ms, becomes a candidate, raises the term and asks for votes; the first to win a majority leads. This usually settles within a second.`,
    });
  }

  const leader = model.nodes.find((node) => node.id === model.leader)!;

  if (isolated.length > 0) {
    const first = isolated[0];
    return finish({
      tone: "warn",
      title: `${first.id} is cut off from its peers`,
      body: `${first.id} is still running and still answers this page, but it cannot see a majority, so it cannot vote or commit. The other ${model.healthyCount} nodes (${leader.id} leads) are a majority and carry on.`,
      hint: `Reconnect ${first.id} and it catches up from the leader's log.`,
    });
  }

  if (down.length > 0) {
    return finish({
      tone: "warn",
      title: `${names(down)} ${down.length > 1 ? "are" : "is"} down`,
      body: `${model.healthyCount} of ${total} nodes are a majority, so writes and reads keep working. Writes are safe because each one needs a majority to acknowledge it. When ${down[0].id} returns it replays its log from disk and ${leader.id} sends it whatever it missed.`,
      hint: `Revive ${down[0].id} and watch its log lane catch up.`,
    });
  }

  const referenceIndex = leader.state?.lastLogIndex ?? 0;
  const lagging = model.nodes.filter(
    (node) => node.live && node.id !== leader.id && entriesBehind(node.state, referenceIndex) > 0,
  );
  if (lagging.length > 0) {
    const first = lagging[0];
    return finish({
      tone: "info",
      title: `${first.id} is catching up`,
      body: `${first.id} is ${entriesBehind(first.state, referenceIndex)} entries behind. ${leader.id} keeps sending it the missing entries; it counts as caught up once its log matches the leader's.`,
    });
  }

  const term = leader.state?.term ?? model.term;
  return finish({
    tone: "good",
    title: `${leader.id} leads term ${term}`,
    body: `The leader sends heartbeats so followers don't start elections. A write is committed once a majority (${model.majority} of ${total}) has it in its log, and then it is applied to every node's key-value store. ${
      term <= 1
        ? "Term 1 means no election has happened since the cluster started."
        : `Term ${term} means ${term - 1} election${term === 2 ? " has" : "s have"} happened so far.`
    }`,
    hint: "Try: write 20 keys, then kill the leader.",
  });
}
