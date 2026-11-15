// SPDX-License-Identifier: GPL-3.0-only
// keep_previous_rfm1.mjs on a fake live site: node --test web/site/tools/keep_previous_rfm1.test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, writeFileSync, existsSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { keepPrevious, rfm1Files } from "./keep_previous_rfm1.mjs";

const sha = (b) => createHash("sha256").update(b).digest("hex");
const OLD = Buffer.from("old evilgenius"), CUR = Buffer.from("amb"), BAD = Buffer.from("tampered");
const files = { "fw/rfm1/evilgenius-0.1.1.rfm1": OLD, "fw/rfm1/amb-1.3.2.rfm1": CUR, "fw/rfm1/x-1.rfm1": BAD };
const catalog = { firmwares: [
  { id: "evilgenius", rfm1: { file: "fw/rfm1/evilgenius-0.1.1.rfm1", sha256: sha(OLD) } },
  { id: "amb", rfm1: { file: "fw/rfm1/amb-1.3.2.rfm1", sha256: sha(CUR) } },
  { id: "x", rfm1: { file: "fw/rfm1/x-1.rfm1", sha256: sha(Buffer.from("x")) } },
  { id: "gone", rfm1: { file: "fw/rfm1/gone-2.rfm1", sha256: "a".repeat(64) } },
  { id: "evil", rfm1: { file: "fw/../../etc/passwd.rfm1", sha256: "b".repeat(64) } },
  { id: "hub-only" },
] };
const fetchFn = async (url) => {
  const p = new URL(url).pathname.slice(1);
  if (p === "catalog.json") return { ok: true, status: 200, json: async () => catalog };
  const b = files[p];
  return b ? { ok: true, status: 200, arrayBuffer: async () => b } : { ok: false, status: 404 };
};

test("only safe fw/ paths with a SHA-256 are taken", () => {
  assert.deepEqual(rfm1Files(catalog).map((f) => f.file).sort(),
    ["fw/rfm1/amb-1.3.2.rfm1", "fw/rfm1/evilgenius-0.1.1.rfm1", "fw/rfm1/gone-2.rfm1", "fw/rfm1/x-1.rfm1"]);
});

test("a file the new site dropped is kept; one it has is left; a bad or missing one is not kept", async () => {
  const site = mkdtempSync(join(tmpdir(), "kp-"));
  mkdirSync(join(site, "fw/rfm1"), { recursive: true });
  writeFileSync(join(site, "fw/rfm1/amb-1.3.2.rfm1"), CUR);
  const r = await keepPrevious({ site, live: "https://remotefm1.test/", fetchFn, log: () => {} });
  assert.deepEqual(r.kept, ["fw/rfm1/evilgenius-0.1.1.rfm1"]);
  assert.equal(r.present, 1);
  assert.deepEqual(r.failed.map((f) => f.file).sort(), ["fw/rfm1/gone-2.rfm1", "fw/rfm1/x-1.rfm1"]);
  assert.deepEqual(readFileSync(join(site, "fw/rfm1/evilgenius-0.1.1.rfm1")), OLD);
  assert.equal(existsSync(join(site, "fw/rfm1/x-1.rfm1")), false);
});

test("no live catalog: nothing kept, no throw", async () => {
  const r = await keepPrevious({ site: mkdtempSync(join(tmpdir(), "kp-")), live: "https://remotefm1.test/",
    fetchFn: async () => ({ ok: false, status: 503 }), log: () => {} });
  assert.deepEqual(r.kept, []);
  assert.equal(r.failed[0].file, "catalog.json");
});
