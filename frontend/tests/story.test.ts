import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { explain } from "../src/lib/story.ts";
import { buildModel } from "../src/lib/model.ts";
import { modelOf, refs } from "./helpers.ts";

const healthy = () =>
  modelOf(3, { "node-1": {}, "node-2": { role: "leader", term: 1 }, "node-3": {} });

describe("explain", () => {
  it("waits quietly before any data, and says so when nothing is reachable later", () => {
    assert.equal(explain(buildModel(refs(3), {}, {}, 0)).title, "Waiting for the cluster");

    const first = healthy();
    const gone = modelOf(
      3,
      {},
      200_000,
      Object.fromEntries(first.nodes.map((n) => [n.id, { state: n.state!, at: 1 }])),
    );
    const story = explain(gone);
    assert.equal(story.tone, "bad");
    assert.equal(story.title, "No node is reachable");
  });

  it("describes a healthy cluster and counts elections from the term", () => {
    const one = explain(healthy());
    assert.equal(one.tone, "good");
    assert.equal(one.title, "node-2 leads term 1");
    assert.match(one.body, /no election has happened/);
    assert.match(one.body, /2 of 3/);

    const later = explain(
      modelOf(3, {
        "node-1": { term: 4 },
        "node-2": { role: "leader", term: 4 },
        "node-3": { term: 4 },
      }),
    );
    assert.match(later.body, /3 elections have happened/);
    const second = explain(
      modelOf(3, {
        "node-1": { term: 2 },
        "node-2": { role: "leader", term: 2 },
        "node-3": { term: 2 },
      }),
    );
    assert.match(second.body, /1 election has happened/);
  });

  it("explains an election in progress and names the candidate", () => {
    const story = explain(
      modelOf(3, {
        "node-1": { role: "candidate", term: 5 },
        "node-3": { term: 5 },
        "node-2": { live: false },
      }),
    );
    assert.equal(story.title, "Election in progress");
    assert.match(story.body, /node-1 is campaigning in term 5/);
  });

  it("explains a node being down while a majority remains", () => {
    const story = explain(
      modelOf(3, {
        "node-1": { sees: ["node-2"] },
        "node-2": { role: "leader", sees: ["node-1"] },
      }),
    );
    assert.equal(story.title, "node-3 is down");
    assert.match(story.body, /2 of 3 nodes are a majority/);
    assert.match(story.hint ?? "", /Revive node-3/);
  });

  it("explains a lone survivor as no majority, without blaming the network", () => {
    const story = explain(modelOf(3, { "node-1": { sees: [] } }));
    assert.equal(story.tone, "bad");
    assert.match(story.body, /Only 1 of 3 nodes are running/);
    assert.doesNotMatch(story.body, /cut off|cannot reach enough/);
  });

  it("explains a stale leader after a partition", () => {
    const story = explain(
      modelOf(3, {
        "node-1": { role: "leader", term: 1, sees: [] },
        "node-2": { role: "leader", term: 2, sees: ["node-3"] },
        "node-3": { term: 2, sees: ["node-2"] },
      }),
    );
    assert.equal(story.title, "node-1 is a stale leader");
    assert.match(story.body, /believes it leads term 1/);
    assert.match(story.body, /node-2 in term 2/);
  });

  it("explains a cut-off follower without a stale leader", () => {
    const story = explain(
      modelOf(3, {
        "node-1": { role: "leader", term: 2, sees: ["node-2"] },
        "node-2": { term: 2, sees: ["node-1"] },
        "node-3": { term: 2, sees: [] },
      }),
    );
    assert.equal(story.title, "node-3 is cut off from its peers");
    assert.match(story.body, /node-1 leads/);
  });

  it("explains a three-way split", () => {
    const story = explain(
      modelOf(3, { "node-1": { sees: [] }, "node-2": { sees: [] }, "node-3": { sees: [] } }),
    );
    assert.equal(story.title, "No group of nodes has a majority");
  });

  it("explains a node catching up", () => {
    const story = explain(
      modelOf(3, {
        "node-1": { lastLogIndex: 7, commitIndex: 7 },
        "node-2": { role: "leader", lastLogIndex: 12, commitIndex: 12 },
        "node-3": { lastLogIndex: 12, commitIndex: 12 },
      }),
    );
    assert.equal(story.title, "node-1 is catching up");
    assert.match(story.body, /5 entries behind/);
  });

  it("adds a network-delay note that matches the measured behaviour", () => {
    assert.equal(explain(healthy(), 0).note, undefined);
    assert.equal(explain(healthy(), 10).note, undefined);
    assert.match(explain(healthy(), 40).note ?? "", /roughly 80–160 ms/);
    const bad = explain(healthy(), 80).note ?? "";
    assert.match(bad, /about 160 ms/);
    assert.match(bad, /150 ms/);
    assert.match(bad, /constant elections/);
  });
});

describe("delayZone", () => {
  it("matches the measured thresholds and the story's wording", async () => {
    const { delayZone } = await import("../src/lib/story.ts");

    assert.equal(delayZone(0).level, "ok");
    assert.equal(delayZone(10).level, "ok");
    assert.equal(delayZone(20).level, "slow");
    assert.equal(delayZone(40).level, "slow");
    assert.equal(delayZone(60).level, "bad");
    assert.equal(delayZone(200).level, "bad");

    // The same boundaries drive the explanation's note.
    for (const ms of [10, 40, 80]) {
      const hasNote = explain(healthy(), ms).note !== undefined;
      assert.equal(hasNote, delayZone(ms).level !== "ok");
    }
  });
});
