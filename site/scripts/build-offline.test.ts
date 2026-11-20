import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
	cpSync,
	mkdtempSync,
	mkdirSync,
	readdirSync,
	readFileSync,
	rmSync,
	symlinkSync,
	writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import type { TestContext } from "node:test";
import test from "node:test";

const prepareExport = (context: TestContext) => {
	const checkout = mkdtempSync(path.join(tmpdir(), "fplinux-offline-site-"));
	context.after(() => {
		rmSync(checkout, { force: true, recursive: true });
	});
	const site = path.join(checkout, "site");
	for (const relative of [
		"config/site.mjs",
		"scripts",
		"src/data/phones.ts",
		"src/scripts/offline-icons.ts",
	]) {
		const destination = path.join(site, relative);
		mkdirSync(path.dirname(destination), { recursive: true });
		cpSync(new URL(`../${relative}`, import.meta.url), destination, {
			recursive: true,
		});
	}
	writeFileSync(path.join(checkout, "THIRD_PARTY_NOTICES.md"), "Test notice\n");
	writeFileSync(path.join(site, "src/data/phones.json"), "[]\n");
	writeFileSync(
		path.join(checkout, "fplinux"),
		'#!/bin/sh\nprintf "%s\\n" "$FPLINUX_DATA_DIR"\n',
		{ mode: 0o755 }
	);
	const modules = path.join(site, "node_modules");
	mkdirSync(path.join(modules, ".bin"), { recursive: true });
	for (const name of ["astro", "esbuild", "parse5", "smol-toml", "wawoff2"]) {
		symlinkSync(
			path.resolve(import.meta.dirname, "../node_modules", name),
			path.join(modules, name)
		);
	}
	cpSync(
		new URL("fixtures/offline-export-astro.cjs.fixture", import.meta.url),
		path.join(modules, ".bin/astro")
	);

	const data = path.join(checkout, "data");
	const output = path.join(data, "out/site-offline");
	const previous = path.join(output, "index.html");
	const previousBytes = Buffer.from("<h1>Previous export</h1>\n");
	mkdirSync(output, { recursive: true });
	writeFileSync(previous, previousBytes);
	const run = (mode: string) =>
		spawnSync(process.execPath, ["scripts/build-offline.ts"], {
			cwd: site,
			encoding: "utf-8",
			env: { ...process.env, FPLINUX_DATA_DIR: data, TEST_EXPORT_MODE: mode },
			timeout: 20_000,
		});
	return { output, previous, previousBytes, run };
};

for (const [mode, error] of [
	["broken-link", /missing\.png/u],
	["changed-input", /Site sources changed during the offline export/u],
] as const) {
	void test(`offline export keeps the previous site after ${mode}`, (context) => {
		const { output, previous, previousBytes, run } = prepareExport(context);
		const result = run(mode);
		assert.equal(result.status, 1, result.stderr);
		assert.match(result.stderr, error);
		assert.ok(
			readFileSync(previous).equals(previousBytes),
			"previous index.html changed"
		);
		assert.deepEqual(readdirSync(output), ["index.html"]);
		assert.deepEqual(readdirSync(path.dirname(output)), ["site-offline"]);
	});
}

void test("offline export replaces the previous site with local pages and their search index", (context) => {
	const { output, previous, run } = prepareExport(context);
	const result = run("success");
	assert.equal(result.status, 0, result.stderr);
	assert.match(readFileSync(previous, "utf-8"), /href="guide\/index\.html"/u);
	assert.match(
		readFileSync(previous, "utf-8"),
		/src="offline\/search-index\.js"/u
	);
	assert.equal(
		readFileSync(path.join(output, "offline/search-index.js"), "utf-8"),
		'window.fplinuxOfflineIndex=[{"text":"Guide Read this guide.","title":"Guide","url":"guide/index.html"},{"text":"Home Start here.","title":"Home","url":"index.html"}];\n'
	);
	assert.deepEqual(readdirSync(path.dirname(output)), ["site-offline"]);
});
