import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { ClusterWatcher } from "../src/lib/watcher.ts";
import type { ClusterModel } from "../src/lib/model.ts";
import type { FeedEvent, NodeRef } from "../src/lib/types.ts";
import { fakeNode, ids, refs, viewFrom, waitFor, type FakeNode, type NodeSpec } from "./helpers.ts";

/** Three fake nodes, each streaming its own view of the cluster, as the real ones do. */
async function cluster(specs: Record<string, NodeSpec>) {
  const all = ids(3);
  const current: Record<string, NodeSpec> = { ...specs };
  const nodes: Record<string, FakeNode> = {};
  const sendersOf: Record<string, ((event: string, data: string) => void)[]> = {};

  const push = (id: string) => {
    const view = JSON.stringify(viewFrom(id, all, current[id] ?? {}));
    for (const send of sendersOf[id] ?? []) send("cluster", view);
  };

  for (const id of all) {
    sendersOf[id] = [];
    nodes[id] = await fakeNode((send) => {
      sendersOf[id].push(send);
      send("cluster", JSON.stringify(viewFrom(id, all, current[id] ?? {})));
    });
  }

  const nodeRefs: NodeRef[] = refs(3).map((ref) => ({ ...ref, url: nodes[ref.id].url }));

  return {
    nodes,
    refs: nodeRefs,
    /** Changes what a node says about itself and pushes it, like a real state change. */
    set(id: string, spec: NodeSpec) {
      current[id] = spec;
      push(id);
    },
    async close() {
      await Promise.all(Object.values(nodes).map((node) => node.kill().catch(() => {})));
    },
  };
}

function watch(nodeRefs: NodeRef[]) {
  let model: ClusterModel | null = null;
  const events: FeedEvent[] = [];
  const watcher = new ClusterWatcher({
    nodes: nodeRefs,
    onChange: (next, fresh) => {
      model = next;
      events.push(...fresh);
    },
    checkMs: 100,
    retryMinMs: 50,
    retryMaxMs: 100,
  });
  watcher.start();
  return {
    watcher,
    model: () => model!,
    events: () => events.map((e) => e.text),
  };
}

describe("ClusterWatcher", () => {
  it("combines every node's own stream into one model", async () => {
    const c = await cluster({ "node-1": {}, "node-2": { role: "leader", term: 3 }, "node-3": {} });
    const w = watch(c.refs);

    assert.ok(await waitFor(() => w.model()?.liveCount === 3 && w.model().leader === "node-2"));
    assert.equal(w.model().term, 3);
    assert.equal(w.model().quorum, true);
    assert.deepEqual(w.events(), []); // page load is not news

    w.watcher.stop();
    await c.close();
  });

  it("sees a node die, remembers its last state, and notices when it returns", async () => {
    const c = await cluster({
      "node-1": {},
      "node-2": { role: "leader" },
      "node-3": { lastLogIndex: 9 },
    });
    const w = watch(c.refs);
    assert.ok(await waitFor(() => w.model()?.liveCount === 3));

    const port = c.nodes["node-3"].port;
    await c.nodes["node-3"].kill();

    assert.ok(await waitFor(() => w.model().nodes[2].live === false));
    assert.ok(await waitFor(() => w.events().includes("node-3 went down")));
    assert.equal(w.model().nodes[2].state?.lastLogIndex, 9); // last report kept for display
    assert.ok(w.model().nodes[2].lastSeenAt !== null);

    // Same address comes back (a revived process).
    const again = await fakeNode(
      (send) => send("cluster", JSON.stringify(viewFrom("node-3", ids(3), {}))),
      port,
    );
    assert.ok(await waitFor(() => w.model().nodes[2].live === true, 5000));
    assert.ok(await waitFor(() => w.events().includes("node-3 is back")));

    w.watcher.stop();
    await again.kill();
    await c.close();
  });

  it("sees a partition: the cut-off leader stays alive and stale while a new leader emerges", async () => {
    const c = await cluster({ "node-1": { role: "leader" }, "node-2": {}, "node-3": {} });
    const w = watch(c.refs);
    assert.ok(await waitFor(() => w.model()?.leader === "node-1"));

    // The network splits: node-1 can no longer see anyone; node-2/3 elect node-2.
    c.set("node-1", { role: "leader", sees: [] });
    c.set("node-2", { role: "leader", term: 2, sees: ["node-3"] });
    c.set("node-3", { term: 2, sees: ["node-2"] });

    // "Isolated" needs the node to have been silent about peers for a moment (SETTLE_MS).
    assert.ok(await waitFor(() => w.model().nodes[0].isolated, 4000));
    const m = w.model();
    assert.equal(m.nodes[0].live, true); // alive: the dashboard still hears it directly
    assert.equal(m.nodes[0].staleLeader, true);
    assert.equal(m.leader, "node-2");
    assert.equal(m.quorum, true);
    assert.ok(w.events().includes("node-1 lost contact with its peers"));
    assert.ok(w.events().includes("node-2 became leader · term 2"));

    // Healed: node-1 hears the higher term, steps down, and everyone sees everyone again.
    c.set("node-1", { term: 2 });
    c.set("node-2", { role: "leader", term: 2 });
    c.set("node-3", { term: 2 });
    assert.ok(
      await waitFor(
        () => !w.model().nodes[0].isolated && w.model().nodes[0].state?.role === "follower",
      ),
    );
    assert.ok(w.events().includes("node-1 reconnected to the cluster"));
    assert.ok(w.events().includes("node-1 stepped down"));

    w.watcher.stop();
    await c.close();
  });

  it("stops cleanly and reports nothing afterwards", async () => {
    const c = await cluster({ "node-1": {}, "node-2": { role: "leader" }, "node-3": {} });
    const w = watch(c.refs);
    assert.ok(await waitFor(() => w.model()?.liveCount === 3));

    w.watcher.stop();
    const before = w.events().length;
    c.set("node-2", { role: "follower" });
    await waitFor(() => false, 300);
    assert.equal(w.events().length, before);

    await c.close();
  });
});
