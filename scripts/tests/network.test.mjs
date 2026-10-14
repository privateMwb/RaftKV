import assert from "node:assert/strict";
import net from "node:net";
import { after, before, describe, it } from "node:test";
import { NetworkEmulator } from "../network.mjs";

// Three fake "nodes": each echoes what it receives, prefixed with its own id.
const IDS = ["node-1", "node-2", "node-3"];

function echoServer(id) {
  return new Promise((resolve) => {
    const server = net.createServer((socket) => {
      socket.on("error", () => {});
      socket.on("data", (chunk) => socket.write(`${id}:${chunk}`));
    });
    server.listen(0, "127.0.0.1", () =>
      resolve({ id, server, port: server.address().port }),
    );
  });
}

let nodes;
let emulator;
let base = 28400;

before(async () => {
  nodes = await Promise.all(IDS.map(echoServer));
});

after(async () => {
  await Promise.all(
    nodes.map(({ server }) => new Promise((resolve) => server.close(resolve))),
  );
});

async function fresh() {
  base += 100;
  emulator = new NetworkEmulator({
    nodes: nodes.map(({ id, port }) => ({ id, port })),
    proxyBase: base,
  });
  await emulator.start();
  return emulator;
}

/** Sends `text` through the proxy from -> to and resolves with the reply, or null on timeout/close. */
function ask(from, to, text, timeoutMs = 400) {
  return new Promise((resolve) => {
    const socket = net.connect(emulator.proxyPort(from, to), "127.0.0.1");
    const timer = setTimeout(() => finish(null), timeoutMs);
    const finish = (value) => {
      clearTimeout(timer);
      socket.destroy();
      resolve(value);
    };
    socket.on("error", () => finish(null));
    socket.on("close", () => finish(null));
    socket.on("connect", () => socket.write(text));
    socket.on("data", (chunk) => finish(chunk.toString()));
  });
}

// A server that behaves like the real nodes: it answers once and CLOSES (HTTP
// "Connection: close"). The reply is still in flight through the proxy's delay
// queue when the server's close arrives; it must be delivered, not discarded.
function closingServer(id) {
  return new Promise((resolve) => {
    const server = net.createServer((socket) => {
      socket.on("error", () => {});
      socket.on("data", () =>
        socket.end(`reply-from-${id}-${"x".repeat(5000)}`),
      );
    });
    server.listen(0, "127.0.0.1", () =>
      resolve({ id, server, port: server.address().port }),
    );
  });
}

describe("NetworkEmulator with servers that close after replying", () => {
  it("delivers the whole reply even though the server closed first", async () => {
    const target = await closingServer("node-2");
    const other = await echoServer("node-1");
    const local = new NetworkEmulator({
      nodes: [
        { id: "node-1", port: other.port },
        { id: "node-2", port: target.port },
      ],
      proxyBase: 28390,
    });
    await local.start();

    try {
      for (const delay of [0, 60]) {
        local.setDelay(delay);
        const started = Date.now();

        const reply = await new Promise((resolve) => {
          let text = "";
          const socket = net.connect(
            local.proxyPort("node-1", "node-2"),
            "127.0.0.1",
          );
          socket.on("connect", () => socket.write("ping"));
          socket.on("data", (chunk) => (text += chunk));
          socket.on("close", () => resolve(text));
          setTimeout(() => resolve(text), 3000);
        });

        assert.equal(
          reply.length,
          "reply-from-node-2-".length + 5000,
          `delay ${delay}: reply was ${reply.length} bytes`,
        );
        if (delay > 0)
          assert.ok(
            Date.now() - started >= 110,
            "should have paid the delay both ways",
          );
      }
    } finally {
      await local.stop();
      await new Promise((resolve) => target.server.close(resolve));
      await new Promise((resolve) => other.server.close(resolve));
    }
  });
});

describe("NetworkEmulator", () => {
  it("forwards traffic to the right node and gives every link its own port", async () => {
    await fresh();
    assert.equal(await ask("node-1", "node-2", "hi"), "node-2:hi");
    assert.equal(await ask("node-2", "node-3", "yo"), "node-3:yo");

    const ports = new Set();
    for (const a of IDS)
      for (const b of IDS) if (a !== b) ports.add(emulator.proxyPort(a, b));
    assert.equal(ports.size, 6);
    assert.throws(() => emulator.proxyPort("node-1", "node-1"));
    assert.throws(() => emulator.proxyPort("node-1", "nope"));

    await emulator.stop();
  });

  it("delays each direction, so a round trip costs about twice the delay", async () => {
    await fresh();
    emulator.setDelay(80);

    const started = Date.now();
    assert.equal(await ask("node-1", "node-2", "slow", 2000), "node-2:slow");
    const elapsed = Date.now() - started;

    assert.ok(elapsed >= 150, `round trip took ${elapsed} ms, expected >= 150`);
    assert.ok(elapsed < 600, `round trip took ${elapsed} ms, expected < 600`);

    emulator.setDelay(0);
    const fast = Date.now();
    await ask("node-1", "node-2", "fast");
    assert.ok(Date.now() - fast < 100);

    await emulator.stop();
  });

  it("keeps bytes in order even when the delay changes mid-stream", async () => {
    await fresh();

    const received = await new Promise((resolve) => {
      let text = "";
      const socket = net.connect(
        emulator.proxyPort("node-1", "node-3"),
        "127.0.0.1",
      );
      socket.on("data", (chunk) => {
        text += chunk;
        // The echo may merge writes, so count payload digits, not replies.
        if (text.replaceAll("node-3:", "").replace(/\D/g, "").length >= 6) {
          socket.destroy();
          resolve(text);
        }
      });
      socket.on("connect", async () => {
        emulator.setDelay(120);
        for (let i = 1; i <= 3; i += 1) socket.write(`${i}`);
        emulator.setDelay(0); // later chunks must not overtake the delayed ones
        for (let i = 4; i <= 6; i += 1) socket.write(`${i}`);
      });
      setTimeout(() => resolve(text), 3000);
    });

    // Echo prefixes each chunk the node saw; strip prefixes and compare the payload order.
    const payload = received.replaceAll("node-3:", "");
    assert.equal([...payload].filter((c) => /\d/.test(c)).join(""), "123456");

    await emulator.stop();
  });

  it("black-holes a blocked link: connects, then nothing comes back", async () => {
    await fresh();
    emulator.isolate("node-3");

    const started = Date.now();
    assert.equal(await ask("node-1", "node-3", "x", 300), null); // into the isolated node
    assert.equal(await ask("node-3", "node-1", "x", 300), null); // out of it
    assert.ok(
      Date.now() - started >= 550,
      "should have waited for the timeout, not failed fast",
    );

    assert.equal(await ask("node-1", "node-2", "ok"), "node-2:ok"); // others unaffected

    emulator.reconnect("node-3");
    assert.equal(await ask("node-1", "node-3", "back"), "node-3:back");

    await emulator.stop();
  });

  it("ends conversations already open on a link when it is cut", async () => {
    await fresh();

    const closed = await new Promise((resolve) => {
      const socket = net.connect(
        emulator.proxyPort("node-1", "node-2"),
        "127.0.0.1",
      );
      socket.on("close", () => resolve(true));
      socket.on("error", () => {});
      socket.on("connect", () =>
        setTimeout(() => emulator.isolate("node-2"), 50),
      );
      setTimeout(() => resolve(false), 1500);
    });

    assert.equal(closed, true);
    await emulator.stop();
  });

  it("reports state: isolated nodes, blocked links, delay", async () => {
    await fresh();
    assert.deepEqual(emulator.state(), {
      enabled: true,
      delayMs: 0,
      isolated: [],
      blocked: [],
    });

    emulator.isolate("node-2");
    emulator.setDelay(50);
    const state = emulator.state();
    assert.deepEqual(state.isolated, ["node-2"]);
    assert.equal(state.delayMs, 50);
    assert.equal(state.blocked.length, 4);

    emulator.reconnect("node-2");
    assert.deepEqual(emulator.state().isolated, []);

    emulator.setDelay(99999);
    assert.equal(emulator.delayMs, 2000);
    emulator.setDelay(-5);
    assert.equal(emulator.delayMs, 0);
    assert.throws(() => emulator.setDelay(Number.NaN));
    assert.throws(() => emulator.isolate("ghost"));

    await emulator.stop();
  });

  it("partitions into groups that talk only within themselves, then heals", async () => {
    await fresh();
    emulator.partition([["node-1", "node-2"], ["node-3"]]);

    assert.equal(await ask("node-1", "node-2", "in"), "node-2:in");
    assert.equal(await ask("node-2", "node-1", "in"), "node-1:in");
    assert.equal(await ask("node-1", "node-3", "x", 250), null);
    assert.equal(await ask("node-3", "node-2", "x", 250), null);
    assert.deepEqual(emulator.state().isolated, ["node-3"]);

    emulator.heal();
    assert.equal(await ask("node-1", "node-3", "ok"), "node-3:ok");
    assert.equal(emulator.state().blocked.length, 0);

    await emulator.stop();
  });

  it("stop() closes every listener", async () => {
    await fresh();
    const port = emulator.proxyPort("node-1", "node-2");
    await emulator.stop();

    const refused = await new Promise((resolve) => {
      const socket = net.connect(port, "127.0.0.1");
      socket.on("error", (error) => resolve(error.code));
      socket.on("connect", () => {
        socket.destroy();
        resolve("connected");
      });
    });
    assert.equal(refused, "ECONNREFUSED");
  });
});
