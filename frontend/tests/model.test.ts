import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { buildModel, SETTLE_MS } from "../src/lib/model.ts";
import { modelOf, refs } from "./helpers.ts";

describe("buildModel", () => {
  it("a healthy cluster has one leader, quorum, nobody isolated", () => {
    const model = modelOf(3, { "node-1": {}, "node-2": { role: "leader", term: 3 }, "node-3": {} });

    assert.equal(model.leader, "node-2");
    assert.equal(model.term, 3);
    assert.equal(model.quorum, true);
    assert.equal(model.liveCount, 3);
    assert.equal(model.healthyCount, 3);
    assert.equal(model.majority, 2);
    assert.ok(model.nodes.every((n) => !n.isolated && !n.staleLeader));
  });

  it("a killed node is down but remembered; the rest keep quorum", () => {
    const first = modelOf(3, { "node-1": {}, "node-2": { role: "leader" }, "node-3": {} });
    const killed = modelOf(
      3,
      { "node-1": { sees: ["node-2"] }, "node-2": { role: "leader", sees: ["node-1"] } },
      200_000,
      { "node-3": { state: first.nodes[2].state!, at: 150_000 } },
    );

    const dead = killed.nodes[2];
    assert.equal(dead.live, false);
    assert.equal(dead.isolated, false); // dead, not cut off
    assert.equal(dead.lastSeenAt, 150_000);
    assert.equal(dead.state?.id, "node-3"); // its last report is kept for display
    assert.equal(killed.quorum, true);
    assert.equal(killed.liveCount, 2);
    assert.equal(killed.leader, "node-2");
  });

  it("tells a partitioned node (alive, cut off) from a dead one, and flags the stale leader", () => {
    const model = modelOf(3, {
      "node-1": { role: "leader", term: 1, sees: [], lastLogIndex: 12 }, // cut off, still thinks it leads
      "node-2": { role: "leader", term: 2, sees: ["node-3"] }, // the majority elected a new leader
      "node-3": { term: 2, sees: ["node-2"] },
    });

    const [old, fresh, other] = model.nodes;
    assert.equal(old.live, true);
    assert.equal(old.isolated, true);
    assert.equal(old.staleLeader, true);
    assert.equal(old.state?.role, "leader"); // it still reports its own (wrong) belief
    assert.equal(fresh.isolated, false);
    assert.equal(other.isolated, false);

    assert.equal(model.leader, "node-2");
    assert.equal(model.term, 2);
    assert.equal(model.healthyCount, 2);
    assert.equal(model.quorum, true);
  });

  it("a lone survivor of two dead nodes is not 'isolated': its peers are down, not unreachable", () => {
    const model = modelOf(3, { "node-1": { role: "leader", sees: [] } });

    assert.equal(model.liveCount, 1);
    assert.equal(model.nodes[0].isolated, false);
    assert.equal(model.quorum, false);
    assert.equal(model.leader, null); // a leader without a majority is not the cluster's leader
    assert.equal(model.nodes[0].staleLeader, true);
  });

  it("a three-way split leaves everybody isolated and no quorum anywhere", () => {
    const model = modelOf(3, {
      "node-1": { sees: [] },
      "node-2": { sees: [] },
      "node-3": { sees: [] },
    });

    assert.ok(model.nodes.every((n) => n.isolated));
    assert.equal(model.quorum, false);
    assert.equal(model.leader, null);
  });

  it("does not call a node isolated until it has streamed for a moment", () => {
    const fresh = modelOf(3, {
      "node-1": { sees: [], liveForMs: SETTLE_MS - 100 }, // just started: hasn't polled peers yet
      "node-2": {},
      "node-3": {},
    });
    assert.equal(fresh.nodes[0].isolated, false);

    const settled = modelOf(3, {
      "node-1": { sees: [], liveForMs: SETTLE_MS + 100 },
      "node-2": {},
      "node-3": {},
    });
    assert.equal(settled.nodes[0].isolated, true);
  });

  it("during an election there is a quorum but no leader", () => {
    const model = modelOf(3, {
      "node-1": { role: "candidate", term: 4 },
      "node-2": { term: 4 },
      "node-3": { term: 4 },
    });

    assert.equal(model.leader, null);
    assert.equal(model.quorum, true);
  });

  it("when two nodes claim leadership with a majority, the higher term wins", () => {
    const model = modelOf(3, {
      "node-1": { role: "leader", term: 4 },
      "node-2": { role: "leader", term: 5 },
      "node-3": { term: 5 },
    });

    assert.equal(model.leader, "node-2");
    assert.equal(model.nodes[0].staleLeader, true);
  });

  it("a cluster nobody has reported yet is all unknown", () => {
    const model = buildModel(refs(3), {}, {}, 1000);

    assert.equal(model.liveCount, 0);
    assert.equal(model.term, 0);
    assert.equal(model.leader, null);
    assert.ok(model.nodes.every((n) => n.state === null && n.stream === "connecting" && !n.live));
  });

  it("scales to five nodes: majority is three", () => {
    const model = modelOf(5, {
      "node-1": { role: "leader", sees: ["node-2", "node-3"] },
      "node-2": { sees: ["node-1", "node-3"] },
      "node-3": { sees: ["node-1", "node-2"] },
      // node-4 and node-5 are down
    });

    assert.equal(model.majority, 3);
    assert.equal(model.quorum, true);
    assert.equal(model.leader, "node-1");
  });
});
