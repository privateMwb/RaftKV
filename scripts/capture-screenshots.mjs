// ============================================================
// scripts/capture-screenshots.mjs
//
// Takes the README screenshots of the live dashboard with Playwright:
//
//   dashboard-overview.png   healthy cluster, leader elected
//   dashboard-kv-console.png after writing a key and a burst of keys
//   dashboard-failover.png   after killing the leader, new leader elected
//
// Run by .github/workflows/screenshots.yml, which installs Playwright
// next to this script. Config comes from the environment:
//
//   DASHBOARD_URL   dashboard to open   (default https://raft-kv.vercel.app)
//   OUT_DIR         where PNGs go       (default ./docs/images)
//
// Note: this really kills the current leader on the live demo. The
// demo's watchdog revives it on its own (RESET_AFTER_SECONDS).
// ============================================================

import { chromium } from "playwright";
import { mkdir } from "node:fs/promises";
import path from "node:path";

const URL_ = process.env.DASHBOARD_URL ?? "https://raft-kv.vercel.app";
const OUT = process.env.OUT_DIR ?? "docs/images";

const LEADER = 'article[aria-label$=", leader"]';
const COLD_START_MS = 180_000; // free Render instance + first election

await mkdir(OUT, { recursive: true });

const browser = await chromium.launch();
const context = await browser.newContext({
  viewport: { width: 1280, height: 900 },
  deviceScaleFactor: 1,
  colorScheme: "dark",
  reducedMotion: "reduce",
});
const page = await context.newPage();

const shot = async (name) => {
  const file = path.join(OUT, name);
  await page.screenshot({ path: file, fullPage: true });
  console.log("saved", file);
};

const leaderId = async () => {
  const label = await page.locator(LEADER).first().getAttribute("aria-label");
  return label.split(",")[0];
};

try {
  console.log("opening", URL_);
  await page.goto(URL_, { waitUntil: "domcontentloaded" });

  // The page keeps retrying until the backend wakes up and elects a leader.
  await page.waitForSelector(LEADER, { timeout: COLD_START_MS });
  await page.waitForTimeout(2_000); // let every card settle
  const firstLeader = await leaderId();
  console.log("leader:", firstLeader);
  await shot("dashboard-overview.png");

  // Key-value console: one PUT, one GET, then a burst of keys.
  await page.getByPlaceholder("key").fill("greeting");
  await page.getByPlaceholder("value").fill("hello world");
  await page.getByRole("button", { name: "PUT", exact: true }).click();
  await page.waitForTimeout(1_500);
  await page.getByRole("button", { name: "GET", exact: true }).click();
  await page.waitForTimeout(1_000);
  await page.getByRole("button", { name: /Write \d+ keys/ }).click();
  await page.waitForTimeout(4_000);
  await shot("dashboard-kv-console.png");

  // Failover: kill the leader, wait for a different node to take over.
  await page
    .locator(LEADER)
    .first()
    .getByRole("button", { name: "Kill node" })
    .click();
  await page.waitForFunction(
    (oldId) => {
      const leader = document.querySelector('article[aria-label$=", leader"]');
      return leader !== null && !leader.getAttribute("aria-label").startsWith(oldId);
    },
    firstLeader,
    { timeout: 60_000 },
  );
  await page.waitForTimeout(2_500);
  console.log("new leader:", await leaderId());
  await shot("dashboard-failover.png");
} catch (error) {
  // Keep a picture of whatever the page showed, to debug from.
  await page.screenshot({ path: path.join(OUT, "_debug-failure.png"), fullPage: true });
  console.error(error);
  process.exitCode = 1;
} finally {
  await browser.close();
}
