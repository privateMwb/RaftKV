// End-to-end check against a REAL running cluster, driven the way the dashboard does:
// the same ClusterWatcher (one stream per node), the same KV client, and the launcher
// for kill / revive / network faults. Skipped unless E2E_LAUNCHER and E2E_NODES are set:
//
//   node ../scripts/launcher.mjs --fresh &        # in another terminal
//   E2E_LAUNCHER=http://127.0.0.1:8099 \
//   E2E_NODES=http://127.0.0.1:8101,http://127.0.0.1:8102,http://127.0.0.1:8103 npm run test:e2e

import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";
import { kvRequest } from "../src/lib/kv.ts";
import {
  fetchLauncherNodes,
  fetchNetwork,
  launcherAction,
  networkAction,
} from "../src/lib/launcher.ts";
import type { ClusterModel } from "../src/lib/model.ts";
import type { FeedEvent, NodeRef } from "../src/lib/types.ts";
import { ClusterWatcher } from "../src/lib/watcher.ts";
import { waitFor } from "./helpers.ts";

const launcher = process.env.E2E_LAUNCHER;
const urls = (process.env.E2E_NODES ?? "").split(",").filter(Boolean);
const enabled = Boolean(launcher) && urls.length > 0;

describe("dashboard logic against a live cluster", { skip: !enabled }, () => {
  const nodes: NodeRef[] = urls.map((url, index) => ({ id: `node-${index + 1}`, url }));
  const urlOf = (id: string) => nodes.find((node) => node.id === id)!.url;
  const base = launcher!;

  let model: ClusterModel;
  const feed: FeedEvent[] = [];
  const said = (text: string) => feed.some((event) => event.text === text);
  let watcher: ClusterWatcher;

  const put = async (id: string, key: string, value: string) => {
    let result = await kvRequest(urlOf(id), "PUT", key, value);
    for (let i = 0; i < 30 && result.status === 503; i += 1) {
      await new Promise((resolve) => setTimeout(resolve, 100));
      result = await kvRequest(urlOf(id), "PUT", key, value);
    }
    return result;
  };

  before(async () => {
    // Start from a clean, fully-up network and cluster.
    await networkAction(base, { kind: "reset" }).catch(() => {});
    for (const node of await fetchLauncherNodes(base))
      if (!node.running) await launcherAction(base, node.id, "start");

    watcher = new ClusterWatcher({
      nodes,
      onChange: (next, fresh) => {
        model = next;
        feed.push(...fresh);
      },
    });
    watcher.start();

    assert.ok(
      await waitFor(
        () =>
          model?.liveCount === nodes.length &&
          model.leader !== null &&
          model.healthyCount === nodes.length,
        15000,
      ),
      "cluster never became fully healthy",
    );
  });

  after(async () => {
    watcher.stop();
    await networkAction(base, { kind: "reset" }).catch(() => {});
    for (const node of await fetchLauncherNodes(base).catch(() => []))
      if (!node.running) await launcherAction(base, node.id, "start").catch(() => {});
  });

  it("sees a crashed leader, a new election, and a revival", async () => {
    const oldLeader = model.leader!;
    const oldTerm = model.term;
    await launcherAction(base, oldLeader, "kill");

    assert.ok(
      await waitFor(
        () =>
          model.nodes.find((n) => n.id === oldLeader)?.live === false &&
          model.leader !== null &&
          model.leader !== oldLeader &&
          model.term > oldTerm,
        12000,
      ),
      "no new leader after the crash",
    );
    assert.ok(said(`${oldLeader} went down`));
    assert.ok(feed.some((e) => e.tone === "leader"));
    assert.equal(model.quorum, true);
    assert.ok(model.nodes.find((n) => n.id === oldLeader)!.lastSeenAt !== null);

    const written = await put(model.leader!, `crash-${Date.now()}`, "survives the crash");
    assert.equal(written.status, 204, written.body);

    await launcherAction(base, oldLeader, "start");
    assert.ok(
      await waitFor(() => {
        const node = model.nodes.find((n) => n.id === oldLeader)!;
        return node.live && node.state?.role === "follower" && !node.isolated;
      }, 12000),
      "revived node never rejoined",
    );
    assert.ok(said(`${oldLeader} is back`));
  });

  it("sees a partition truthfully: a stale leader, a new leader, then healing", async () => {
    assert.ok(
      await waitFor(() => model.leader !== null && model.healthyCount === nodes.length, 10000),
    );
    const stale = model.leader!;
    const termBefore = model.term;

    const cut = await networkAction(base, { kind: "isolate", id: stale });
    assert.deepEqual(cut.isolated, [stale]);

    // The cut-off leader is still ALIVE (we hear it directly) and still believes it leads.
    assert.ok(
      await waitFor(() => {
        const node = model.nodes.find((n) => n.id === stale)!;
        return node.live && node.isolated && node.staleLeader;
      }, 15000),
      "cut-off leader was not shown as an isolated stale leader",
    );
    assert.equal(model.nodes.find((n) => n.id === stale)!.state?.role, "leader");

    // The majority elected someone else in a higher term and keeps working.
    assert.ok(model.leader !== null && model.leader !== stale);
    assert.ok(model.term > termBefore);
    assert.equal(model.quorum, true);
    assert.equal(model.healthyCount, nodes.length - 1);
    assert.ok(said(`${stale} lost contact with its peers`));

    const key = `partition-${Date.now()}`;
    const onMajority = await put(model.leader!, key, "written during the partition");
    assert.equal(onMajority.status, 204, onMajority.body);

    // The stale leader cannot commit anything, and says so instead of lying.
    const onStale = await kvRequest(urlOf(stale), "PUT", `${key}-stale`, "must not commit", {
      timeoutMs: 6000,
    });
    assert.equal(onStale.status, 503);
    const staleRead = await kvRequest(urlOf(stale), "GET", key);
    assert.equal(staleRead.status, 503, "a cut-off leader served a linearizable read");

    // Heal: it hears the higher term, steps down, and catches up.
    await networkAction(base, { kind: "heal" });
    assert.ok(
      await waitFor(() => {
        const node = model.nodes.find((n) => n.id === stale)!;
        return node.live && !node.isolated && !node.staleLeader && node.state?.role === "follower";
      }, 15000),
      "stale leader did not step down after the network healed",
    );
    assert.ok(said(`${stale} reconnected to the cluster`));
    assert.ok(said(`${stale} stepped down`));

    // It catches up from the new leader's log: the majority's write appears on it.
    const readLocal = (id: string, k: string) =>
      fetch(`${urlOf(id)}/kv/${encodeURIComponent(k)}?stale=1`);
    let caughtUp = false;
    for (let i = 0; i < 100 && !caughtUp; i += 1) {
      const reply = await readLocal(stale, key);
      caughtUp = reply.status === 200 && (await reply.text()) === "written during the partition";
      if (!caughtUp) await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert.ok(caughtUp, "healed node never received the majority's write");

    // The write the stale leader refused was never applied anywhere.
    const refused = await readLocal(stale, `${key}-stale`);
    assert.equal(refused.status, 404);
  });

  it("injected delay: commits slow down predictably, then recover", async () => {
    assert.ok(
      await waitFor(() => model.leader !== null && model.healthyCount === nodes.length, 10000),
    );

    const timeOne = async (key: string) => {
      const started = Date.now();
      const result = await put(model.leader!, key, "timing");
      return { status: result.status, ms: Date.now() - started };
    };

    const fast = await timeOne(`fast-${Date.now()}`);
    assert.equal(fast.status, 204);

    await networkAction(base, { kind: "delay", ms: 40 });
    assert.equal((await fetchNetwork(base)).delayMs, 40);
    assert.ok(
      await waitFor(() => model.leader !== null && model.healthyCount === nodes.length, 8000),
    );

    const slow = await timeOne(`slow-${Date.now()}`);
    assert.equal(slow.status, 204);
    // One round trip is 80 ms at 40 ms each way; a commit can never be quicker than that.
    assert.ok(slow.ms >= 70, `expected at least one round trip (~80 ms), got ${slow.ms} ms`);
    assert.ok(slow.ms < 1500, `commit took ${slow.ms} ms at 40 ms delay`);

    await networkAction(base, { kind: "reset" });
    assert.ok(
      await waitFor(() => model.leader !== null && model.healthyCount === nodes.length, 10000),
    );
    const recovered = await timeOne(`recovered-${Date.now()}`);
    assert.equal(recovered.status, 204);
    assert.ok(
      recovered.ms < slow.ms,
      `commit did not speed up after reset (${recovered.ms} vs ${slow.ms} ms)`,
    );
  });
});
