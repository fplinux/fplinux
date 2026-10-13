import assert from "node:assert/strict";
import {
	cpSync,
	mkdtempSync,
	mkdirSync,
	readFileSync,
	rmSync,
	writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { z } from "astro/zod";
import { loadSupport } from "../src/data/support/index.ts";

const supportCheckout = (context: test.TestContext) => {
	const root = mkdtempSync(path.join(tmpdir(), "fplinux-support-"));
	context.after(() => {
		rmSync(root, { force: true, recursive: true });
	});
	cpSync(new URL("fixtures/support-checkout/", import.meta.url), root, {
		recursive: true,
	});
	return root;
};

void test("support composition retains editorial order, notes and optional hardware independently of build availability", (context) => {
	const root = supportCheckout(context);
	const expected = {
		features: [
			{
				group: "Access",
				guide: "use/connect#shell",
				id: "second-feature",
				implementation: "Implemented",
				kind: "system",
				label: "Connection",
				phones: {
					first: { note: "Recorded result.", status: "Supported" },
					second: { note: "", status: "Unknown" },
				},
				scope: "both",
				verification: "Device-tested",
			},
			{
				group: "Input",
				guide: "use/terminal",
				id: "first-feature",
				implementation: "Implemented",
				kind: "hardware",
				label: "Input device",
				phones: {
					first: { hardware: "Present", note: "", status: "Unknown" },
					second: { note: "Input is unqualified.", status: "Partial" },
				},
				scope: "detail",
				verification: "Not device-tested",
			},
		],
		groups: ["Input", "Access"],
	};
	assert.deepEqual(loadSupport(root), expected);
	assert.deepEqual(Object.keys(loadSupport(root).features[0].phones), [
		"second",
		"first",
	]);
	const directory = path.join(root, "targets/first");
	mkdirSync(directory, { recursive: true });
	writeFileSync(path.join(directory, "target.toml"), "[microsd]\n");
	assert.deepEqual(loadSupport(root), expected);
});

void test("support composition rejects missing phone results instead of displaying an inferred state", (context) => {
	const root = supportCheckout(context);
	writeFileSync(
		path.join(root, "site/src/data/support/phones/first.json"),
		'{"first-feature":{"note":"","status":"Unknown"}}\n'
	);
	assert.throws(
		() => loadSupport(root),
		/Missing first support for second-feature/u
	);
});

void test("support composition rejects a phone result with no catalog feature", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(root, "site/src/data/support/phones/first.json");
	const phone = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	phone["unlisted-feature"] = { note: "", status: "Supported" };
	writeFileSync(filename, JSON.stringify(phone));
	assert.throws(
		() => loadSupport(root),
		/Unknown first support feature: unlisted-feature/u
	);
});

void test("support composition rejects repeated feature IDs instead of duplicating checklist rows", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(root, "site/src/data/support/catalog.json");
	const catalog = z
		.object({
			features: z.array(z.unknown()),
			groups: z.array(z.string()),
		})
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	catalog.features.push(catalog.features[0]);
	writeFileSync(filename, JSON.stringify(catalog));
	assert.throws(() => loadSupport(root), /distinct feature IDs/u);
});
