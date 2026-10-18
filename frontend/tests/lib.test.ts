import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { buildConfig } from "../src/lib/config.ts";
import { deriveEvents } from "../src/lib/events.ts";
import { portOf } from "../src/lib/format.ts";
import { buildModel } from "../src/lib/model.ts";
import { buildLane, entriesBehind, laneWindow } from "../src/lib/lanes.ts";
import { ringPositions } from "../src/lib/layout.ts";
import { parseClusterView } from "../src/lib/parse.ts";
import { SseParser } from "../src/lib/sse.ts";
import { describeStatus, kvRequest } from "../src/lib/kv.ts";
import { fetchNetwork, NO_NETWORK, networkAction, networkPath } from "../src/lib/launcher.ts";
import { down, ids, member, modelOf, refs, view, viewFrom } from "./helpers.ts";

describe("SseParser", () => {
  it("parses an event split across chunks", () => {
    const parser = new SseParser();
    assert.deepEqual(parser.feed("event: cluster\nda"), []);
    assert.deepEqual(parser.feed('ta: {"a":1}\n\n'), [{ event: "cluster", data: '{"a":1}' }]);
  });

  it("handles several events in one chunk, CRLF, comments and multi-line data", () => {
    const messages = new SseParser().feed(
      ": hello\r\nevent: ping\r\ndata:\r\n\r\ndata: one\ndata: two\n\nevent:x\ndata:y\n\n",
    );
    assert.deepEqual(messages, [
      { event: "ping", data: "" },
      { event: "message", data: "one\ntwo" },
      { event: "x", data: "y" },
    ]);
  });

  it("ignores blocks with no event or data field", () => {
    assert.deepEqual(new SseParser().feed("id: 5\nretry: 100\n\n"), []);
  });
});

describe("parseClusterView", () => {
  const good = view([member("node-1", "follower"), member("node-2", "leader"), down("node-3")]);

  it("accepts a well-formed view", () => {
    assert.deepEqual(parseClusterView(JSON.stringify(good)), good);
  });

  it("rejects bad JSON, wrong shapes, bad roles, and 'up' without a report", () => {
    assert.equal(parseClusterView("nope"), null);
    assert.equal(parseClusterView("[]"), null);
    assert.equal(parseClusterView(JSON.stringify({ ...good, quorum: "yes" })), null);
    assert.equal(parseClusterView(JSON.stringify({ ...good, term: -1 })), null);

    const badRole = structuredClone(good);
    (badRole.members[0].state as { role: string }).role = "wizard";
    assert.equal(parseClusterView(JSON.stringify(badRole)), null);

    const upNoState = structuredClone(good);
    upNoState.members[0].state = null;
    assert.equal(parseClusterView(JSON.stringify(upNoState)), null);
  });
});

describe("deriveEvents", () => {
  const healthy = () =>
    modelOf(3, {
      "node-1": { role: "follower" },
      "node-2": { role: "leader" },
      "node-3": { role: "follower" },
    });

  it("yields nothing for the first model or an unchanged one", () => {
    assert.deepEqual(deriveEvents(null, healthy(), 1), []);
    assert.deepEqual(deriveEvents(healthy(), healthy(), 1), []);
  });

  it("reports a node going down and coming back", () => {
    const lost = modelOf(
      3,
      { "node-1": { sees: ["node-2"] }, "node-2": { role: "leader", sees: ["node-1"] } },
      100_000,
      { "node-3": { state: healthy().nodes[2].state!, at: 90_000 } },
    );
    assert.deepEqual(
      deriveEvents(healthy(), lost, 5).map((e) => [e.tone, e.text]),
      [["bad", "node-3 went down"]],
    );
    assert.deepEqual(
      deriveEvents(lost, healthy(), 6).map((e) => [e.tone, e.text]),
      [["good", "node-3 is back"]],
    );
  });

  it("does not call a node 'back' the first time the page sees it", () => {
    // Page load: node-1 has answered; nodes 2 and 3 are still on their first attempt.
    const loading = buildModel(
      refs(3),
      { "node-1": { stream: "live", view: viewFrom("node-1", ids(3), {}), liveSince: 0 } },
      {},
      100_000,
    );
    const loaded = healthy();
    assert.deepEqual(
      deriveEvents(loading, loaded, 1).map((e) => e.text),
      [],
    );
  });

  it("reports an election, a new leader, and a stepped-down leader", () => {
    const election = modelOf(3, {
      "node-1": { role: "candidate", term: 5, sees: ["node-3"] },
      "node-3": { role: "follower", term: 5, sees: ["node-1"] },
    });
    assert.deepEqual(
      deriveEvents(healthy(), election, 7).map((e) => e.text),
      ["node-1 started an election · term 5", "node-2 went down"],
    );

    const won = modelOf(3, {
      "node-1": { role: "leader", term: 5, sees: ["node-3"] },
      "node-3": { role: "follower", term: 5, sees: ["node-1"] },
    });
    assert.deepEqual(
      deriveEvents(election, won, 8).map((e) => [e.tone, e.text]),
      [["leader", "node-1 became leader · term 5"]],
    );

    const twoLeaders = modelOf(3, {
      "node-1": { role: "leader", term: 5 },
      "node-2": { role: "leader", term: 4, sees: [] },
      "node-3": { role: "follower", term: 5 },
    });
    const demoted = modelOf(3, {
      "node-1": { role: "leader", term: 5 },
      "node-2": { role: "follower", term: 5 },
      "node-3": { role: "follower", term: 5 },
    });
    assert.deepEqual(
      deriveEvents(twoLeaders, demoted, 9)
        .map((e) => e.text)
        .filter((t) => t.includes("stepped")),
      ["node-2 stepped down"],
    );
  });

  it("reports a partition: node cut off, new leader, then reconnection and step-down", () => {
    // node-1 leads; the network then cuts it off from node-2 and node-3.
    const before = modelOf(3, { "node-1": { role: "leader" }, "node-2": {}, "node-3": {} });
    const cut = modelOf(3, {
      "node-1": { role: "leader", sees: [] },
      "node-2": { role: "leader", term: 2, sees: ["node-3"] },
      "node-3": { term: 2, sees: ["node-2"] },
    });

    const during = deriveEvents(before, cut, 10).map((e) => e.text);
    assert.ok(during.includes("node-1 lost contact with its peers"));
    assert.ok(during.includes("node-2 became leader · term 2"));
    assert.equal(cut.nodes[0].staleLeader, true);
    assert.equal(cut.leader, "node-2");

    const healed = modelOf(3, {
      "node-1": { term: 2 },
      "node-2": { role: "leader", term: 2 },
      "node-3": { term: 2 },
    });
    const after = deriveEvents(cut, healed, 20).map((e) => e.text);
    assert.ok(after.includes("node-1 reconnected to the cluster"));
    assert.ok(after.includes("node-1 stepped down"));
  });

  it("reports quorum loss and recovery", () => {
    const lost = modelOf(3, { "node-1": { sees: [] } });
    const texts = deriveEvents(healthy(), lost, 1).map((e) => e.text);
    assert.ok(texts.includes("quorum lost · 1 of 3 healthy"));
    assert.ok(deriveEvents(lost, healthy(), 2).some((e) => e.text === "quorum restored"));
  });

  it("gives every event a unique id", () => {
    const lost = modelOf(3, {});
    const ids2 = deriveEvents(healthy(), lost, 1).map((e) => e.id);
    assert.equal(new Set(ids2).size, ids2.length);
  });
});

describe("lanes", () => {
  const state = (commitIndex: number, lastLogIndex: number) => ({
    id: "n",
    role: "follower" as const,
    term: 1,
    leader: null,
    commitIndex,
    lastApplied: commitIndex,
    lastLogIndex,
    keys: 0,
  });

  it("marks committed, pending and missing entries over the window", () => {
    const lane = buildLane(state(6, 8), 10, 5); // shows entries 6..10
    assert.deepEqual(lane.cells, ["committed", "pending", "pending", "missing", "missing"]);
  });

  it("measures how far behind a node is against a reference index, never negative", () => {
    assert.equal(entriesBehind(state(6, 8), 10), 2);
    assert.equal(entriesBehind(state(12, 12), 10), 0); // a stale leader can be ahead
    assert.equal(entriesBehind(null, 4), 4);
  });

  it("pads before the start of the log with 'none'", () => {
    assert.deepEqual(buildLane(state(2, 2), 2, 4).cells, [
      "none",
      "none",
      "committed",
      "committed",
    ]);
  });

  it("treats an unknown node as missing everything", () => {
    assert.deepEqual(buildLane(null, 3, 3).cells, ["missing", "missing", "missing"]);
  });

  it("computes the shown index range", () => {
    assert.deepEqual(laneWindow(44, 28), { first: 17, last: 44 });
    assert.deepEqual(laneWindow(5, 28), { first: 1, last: 5 });
    assert.deepEqual(laneWindow(0, 28), { first: 1, last: 0 });
  });
});

describe("ringPositions", () => {
  it("handles 0, 1 and 2 nodes", () => {
    assert.deepEqual(ringPositions(0), []);
    assert.deepEqual(ringPositions(1), [{ x: 50, y: 50 }]);
    assert.equal(ringPositions(2).length, 2);
  });

  it("puts the first of three at the top and keeps everyone inside the box", () => {
    const [top, right, left] = ringPositions(3);
    assert.deepEqual(top, { x: 50, y: 20 });
    assert.ok(right.x > 50 && left.x < 50 && right.y > 50);
    for (const n of [3, 4, 5, 7])
      for (const p of ringPositions(n)) assert.ok(p.x >= 10 && p.x <= 90 && p.y >= 10 && p.y <= 90);
  });
});

describe("buildConfig", () => {
  it("defaults to three nodes on the page's own host", () => {
    const config = buildConfig({}, "192.168.1.20");
    assert.deepEqual(
      config.nodes.map((n) => n.url),
      ["http://192.168.1.20:8101", "http://192.168.1.20:8102", "http://192.168.1.20:8103"],
    );
    assert.equal(config.launcherUrl, "http://192.168.1.20:8099");
  });

  it("honours overrides and ignores junk ports", () => {
    const config = buildConfig(
      {
        VITE_NODE_HOST: "10.0.0.5",
        VITE_NODE_PORTS: "9001, x ,0,70000,9002",
        VITE_LAUNCHER_URL: "http://l:1/",
      },
      "ignored",
    );
    assert.deepEqual(config.nodes, [
      { id: "node-1", url: "http://10.0.0.5:9001" },
      { id: "node-2", url: "http://10.0.0.5:9002" },
    ]);
    assert.equal(config.launcherUrl, "http://l:1");
  });

  it("takes full node URLs for a hosted demo, and they win over host and ports", () => {
    const config = buildConfig(
      {
        VITE_NODE_URLS:
          "https://demo.example/n/1, https://demo.example/n/2/ ,https://demo.example/n/3",
        VITE_NODE_HOST: "ignored",
        VITE_NODE_PORTS: "9001",
        VITE_LAUNCHER_URL: "https://demo.example/launcher/",
      },
      "ignored",
    );
    assert.deepEqual(config.nodes, [
      { id: "node-1", url: "https://demo.example/n/1" },
      { id: "node-2", url: "https://demo.example/n/2" },
      { id: "node-3", url: "https://demo.example/n/3" },
    ]);
    assert.equal(config.launcherUrl, "https://demo.example/launcher");
  });

  it("skips junk node URLs, and falls back to host and ports if none are usable", () => {
    const mixed = buildConfig(
      { VITE_NODE_URLS: "not a url, ftp://x/n/1, https://demo.example/n/1, javascript:alert(1)" },
      "h",
    );
    assert.deepEqual(mixed.nodes, [{ id: "node-1", url: "https://demo.example/n/1" }]);

    const none = buildConfig({ VITE_NODE_URLS: "junk, also junk", VITE_NODE_PORTS: "9001" }, "h");
    assert.deepEqual(none.nodes, [{ id: "node-1", url: "http://h:9001" }]);
  });
});

describe("format and kv helpers", () => {
  it("extracts ports and describes statuses", () => {
    assert.equal(portOf("http://localhost:8101"), ":8101");
    assert.equal(portOf("not a url"), "");
    assert.equal(describeStatus(204), "204 No Content");
    assert.equal(describeStatus(0), "no answer");
    assert.equal(describeStatus(418), "418");
  });

  it("encodes keys, sends raw PUT bodies, and maps failures to status 0", async () => {
    const calls: { url: string; method?: string; body?: unknown }[] = [];
    const fetchFn = (async (url: string, init: RequestInit) => {
      calls.push({ url, method: init.method, body: init.body });
      return new Response(null, { status: 204 });
    }) as unknown as typeof fetch;

    const put = await kvRequest("http://n", "PUT", "user/42 name", "héllo", { fetchFn });
    assert.deepEqual(calls[0], {
      url: "http://n/kv/user%2F42%20name",
      method: "PUT",
      body: "héllo",
    });
    assert.equal(put.ok, true);
    assert.equal(put.status, 204);
    assert.equal(put.body, "");

    const get = await kvRequest("http://n", "GET", "k", undefined, { fetchFn });
    assert.equal(calls[1].body, undefined);
    assert.equal(get.status, 204);

    const failing = (async () => {
      throw new TypeError("fetch failed");
    }) as unknown as typeof fetch;
    const failed = await kvRequest("http://n", "GET", "k", undefined, { fetchFn: failing });
    assert.equal(failed.ok, false);
    assert.equal(failed.status, 0);
  });
});

describe("launcher network client", () => {
  it("builds the right paths", () => {
    assert.equal(networkPath({ kind: "delay", ms: 79.6 }), "/network/delay?ms=80");
    assert.equal(networkPath({ kind: "isolate", id: "node-2" }), "/network/isolate/node-2");
    assert.equal(networkPath({ kind: "reconnect", id: "a b" }), "/network/reconnect/a%20b");
    assert.equal(networkPath({ kind: "heal" }), "/network/heal");
    assert.equal(networkPath({ kind: "reset" }), "/network/reset");
  });

  it("parses the state, treats 'disabled' as disabled, and rejects junk", async () => {
    const reply = (body: unknown, status = 200) =>
      (async () => new Response(JSON.stringify(body), { status })) as unknown as typeof fetch;

    const on = await fetchNetwork(
      "http://l",
      reply({ enabled: true, delayMs: 40, isolated: ["node-2", 7], blocked: [["a", "b"], "x"] }),
    );
    assert.deepEqual(on, {
      enabled: true,
      delayMs: 40,
      isolated: ["node-2"],
      blocked: [["a", "b"]],
    });

    assert.deepEqual(await fetchNetwork("http://l", reply({ enabled: false })), NO_NETWORK);
    await assert.rejects(fetchNetwork("http://l", reply({}, 500)));
    await assert.rejects(fetchNetwork("http://l", reply("nope")));

    const calls: string[] = [];
    const spy = (async (url: string, init: RequestInit) => {
      calls.push(`${init.method} ${url}`);
      return new Response(
        JSON.stringify({ enabled: true, delayMs: 0, isolated: ["node-3"], blocked: [] }),
      );
    }) as unknown as typeof fetch;
    const after = await networkAction("http://l", { kind: "isolate", id: "node-3" }, spy);
    assert.deepEqual(calls, ["POST http://l/network/isolate/node-3"]);
    assert.deepEqual(after.isolated, ["node-3"]);
  });
});
