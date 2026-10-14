import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { ClusterStream } from "../src/lib/clusterStream.ts";
import type { ClusterView, NodeRef, StreamStatus } from "../src/lib/types.ts";
import { fakeNode, member, view, waitFor } from "./helpers.ts";

const healthy = (observer: string) =>
  view([member(observer, "follower"), member("node-2", "leader")], { observer });

function harness(nodes: NodeRef[], extra = {}) {
  const views: { view: ClusterView; via: string }[] = [];
  const statuses: StreamStatus[] = [];
  const stream = new ClusterStream({
    nodes,
    onView: (v, via) => views.push({ view: v, via: via.id }),
    onStatus: (s) => statuses.push(s),
    retryMinMs: 20,
    retryMaxMs: 60,
    ...extra,
  });
  return { stream, views, statuses };
}

describe("ClusterStream", () => {
  it("delivers each pushed view, reports live, and skips junk events", async () => {
    const a = await fakeNode();
    const { stream, views, statuses } = harness([{ id: "a", url: a.url }]);
    stream.start();

    assert.ok(await waitFor(() => a.requests() > 0));
    await waitFor(() => false, 50);
    a.send("ping", "");
    a.send("cluster", "this is not json");
    a.send("cluster", JSON.stringify(healthy("node-1")));
    a.send("cluster", JSON.stringify({ ...healthy("node-1"), term: 9 }));

    assert.ok(await waitFor(() => views.length === 2));
    assert.deepEqual(
      views.map((v) => v.view.term),
      [4, 9],
    );
    assert.equal(statuses.at(-1)?.state, "live");
    assert.equal(statuses.at(-1)?.via?.id, "a");

    stream.stop();
    await a.kill();
  });

  it("fails over past a node that is down", async () => {
    const dead = await fakeNode();
    const deadUrl = dead.url;
    await dead.kill();
    const live = await fakeNode((send) => send("cluster", JSON.stringify(healthy("node-2"))));

    const { stream, views } = harness([
      { id: "dead", url: deadUrl },
      { id: "live", url: live.url },
    ]);
    stream.start();

    assert.ok(await waitFor(() => views.length >= 1));
    assert.equal(views[0].via, "live");

    stream.stop();
    await live.kill();
  });

  it("moves to the next node when the current one dies mid-stream", async () => {
    const first = await fakeNode((send) => send("cluster", JSON.stringify(healthy("node-1"))));
    const second = await fakeNode((send) => send("cluster", JSON.stringify(healthy("node-2"))));
    const { stream, views } = harness([
      { id: "first", url: first.url },
      { id: "second", url: second.url },
    ]);
    stream.start();

    assert.ok(await waitFor(() => views.some((v) => v.via === "first")));
    await first.kill();
    assert.ok(await waitFor(() => views.some((v) => v.via === "second")));

    stream.stop();
    await second.kill();
  });

  it("gives up on a node that goes silent (no pings) and tries another", async () => {
    const silent = await fakeNode((send) => send("cluster", JSON.stringify(healthy("node-1")))); // then nothing
    const lively = await fakeNode((send) => send("cluster", JSON.stringify(healthy("node-2"))));
    const { stream, views } = harness(
      [
        { id: "silent", url: silent.url },
        { id: "lively", url: lively.url },
      ],
      { staleMs: 250 },
    );
    stream.start();

    assert.ok(await waitFor(() => views.some((v) => v.via === "silent")));
    // Keep "silent" the only one answering for a moment, then it must lose to the watchdog.
    await silent.kill();
    assert.ok(await waitFor(() => views.some((v) => v.via === "lively"), 4000));

    stream.stop();
    await lively.kill();
  });

  it("reports offline when every node is down, then recovers when one returns", async () => {
    const a = await fakeNode();
    const url = a.url;
    await a.kill();

    const { stream, statuses, views } = harness([{ id: "a", url }]);
    stream.start();
    assert.ok(await waitFor(() => statuses.some((s) => s.state === "offline")));

    stream.stop();
    assert.equal(views.length, 0);
  });

  it("stops delivering after stop()", async () => {
    const a = await fakeNode((send) => send("cluster", JSON.stringify(healthy("node-1"))));
    const { stream, views } = harness([{ id: "a", url: a.url }]);
    stream.start();
    assert.ok(await waitFor(() => views.length === 1));

    stream.stop();
    a.send("cluster", JSON.stringify(healthy("node-1")));
    await waitFor(() => false, 150);
    assert.equal(views.length, 1);

    await a.kill();
  });

  it("is offline immediately with no nodes configured", () => {
    const { stream, statuses } = harness([]);
    stream.start();
    assert.deepEqual(statuses.at(-1), { state: "offline", via: null });
    stream.stop();
  });
});

describe("ClusterStream options", () => {
  it("treats explicitly undefined options as 'use the default'", async () => {
    // Regression: spreading `{ fetchFn: undefined }` over the defaults used to erase fetch.
    const a = await fakeNode((send) => send("cluster", JSON.stringify(healthy("node-1"))));
    const { stream, views } = harness([{ id: "a", url: a.url }], {
      fetchFn: undefined,
      staleMs: undefined,
      retryMinMs: undefined,
      retryMaxMs: undefined,
    });
    stream.start();

    assert.ok(await waitFor(() => views.length === 1));

    stream.stop();
    await a.kill();
  });
});
