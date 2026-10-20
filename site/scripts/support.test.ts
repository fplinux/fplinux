import assert from "node:assert/strict";
import {
	cpSync,
	mkdtempSync,
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

void test("support composition retains editorial order and explicit results for each platform and phone", (context) => {
	const root = supportCheckout(context);
	const expected = {
		features: [
			{
				group: "Access",
				guide: "use/connect#shell",
				id: "second-feature",
				implementations: {
					engine: "implemented",
					plain: "unimplemented",
				},
				kind: "system",
				label: "Connection",
				phones: {
					first: { note: "Recorded result.", status: "verified" },
					second: { note: "", status: "untested" },
				},
				scope: "both",
			},
			{
				group: "Input",
				guide: "use/terminal",
				id: "first-feature",
				implementations: {
					engine: "unimplemented",
					plain: "implemented",
				},
				kind: "hardware",
				label: "Input device",
				phones: {
					first: {
						hardware: "present",
						note: "No supported input path.",
						status: "unsupported",
						stock: "available",
						stockNote: "Available in the original menu.",
					},
					second: {
						hardware: "unknown",
						note: "Input is unqualified.",
						status: "partial",
						stock: "unknown",
					},
				},
				scope: "detail",
				stock: true,
			},
			{
				group: "Display",
				guide: "use/display",
				id: "engine-feature",
				implementations: {
					engine: "implemented",
					plain: "unimplemented",
				},
				kind: "hardware",
				label: "Platform engine",
				phones: {
					first: {
						hardware: "absent",
						note: "This phone has no engine.",
						status: "unsupported",
					},
					second: {
						hardware: "present",
						note: "This implementation is not used on this phone.",
						status: "unsupported",
					},
				},
				scope: "both",
			},
			{
				group: "Access",
				guide: "apps/viewer",
				id: "viewer",
				implementations: {
					engine: "implemented",
					plain: "implemented",
				},
				kind: "application",
				label: "Viewer",
				phones: {
					first: { note: "", status: "verified" },
					second: { note: "", status: "untested" },
				},
				scope: "both",
			},
		],
		groups: ["Input", "Display", "Access"],
		platforms: [
			{ id: "plain", name: "Example Plain" },
			{ id: "engine", name: "Example Engine" },
		],
	};
	assert.deepEqual(loadSupport(root), expected);
	assert.deepEqual(Object.keys(loadSupport(root).features[0].phones), [
		"second",
		"first",
	]);
});

void test("support claims remain unchanged when packages and a microSD profile become available", (context) => {
	const root = supportCheckout(context);
	const original = loadSupport(root);
	const filename = path.join(root, "targets/first/target.toml");
	writeFileSync(
		filename,
		`${readFileSync(filename, "utf-8").replace("packages = []", 'packages = ["example-package"]')}\n[microsd]\n`
	);
	assert.deepEqual(loadSupport(root), original);
});

for (const { feature, implementation, phoneId } of [
	{
		feature: "second-feature",
		implementation: "implemented",
		phoneId: "first",
	},
	{
		feature: "first-feature",
		implementation: "unimplemented",
		phoneId: "first",
	},
]) {
	void test(`support composition requires a phone result when its platform state is ${implementation}`, (context) => {
		const root = supportCheckout(context);
		const filename = path.join(
			root,
			`site/src/data/support/phones/${phoneId}.json`
		);
		const phone = z
			.record(z.string(), z.unknown())
			.parse(JSON.parse(readFileSync(filename, "utf-8")));
		const remaining = Object.fromEntries(
			Object.entries(phone).filter(([id]) => id !== feature)
		);
		writeFileSync(filename, JSON.stringify(remaining));
		assert.throws(
			() => loadSupport(root),
			new RegExp(`Missing ${phoneId} support for ${feature}`, "u")
		);
	});
}

void test("phone hardware, stock and support remain independent when platform implementations change", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(
		root,
		"site/src/data/support/platforms/engine.json"
	);
	const platform = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	platform["first-feature"] = "implemented";
	writeFileSync(filename, JSON.stringify(platform));
	const target = path.join(root, "targets/second/target.toml");
	writeFileSync(
		target,
		readFileSync(target, "utf-8").replace(
			'platform = "plain"',
			'platform = "engine"'
		)
	);
	const result = loadSupport(root);
	assert.deepEqual(result.platforms, [
		{ id: "engine", name: "Example Engine" },
	]);
	assert.deepEqual(result.features[1].implementations, {
		engine: "implemented",
	});
	assert.deepEqual(result.features[1].phones, {
		first: {
			hardware: "present",
			note: "No supported input path.",
			status: "unsupported",
			stock: "available",
			stockNote: "Available in the original menu.",
		},
		second: {
			hardware: "unknown",
			note: "Input is unqualified.",
			status: "partial",
			stock: "unknown",
		},
	});
});

void test("a phone can provide a function without a shared platform implementation", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(
		root,
		"site/src/data/support/platforms/engine.json"
	);
	const platform = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	platform["second-feature"] = "unimplemented";
	writeFileSync(filename, JSON.stringify(platform));
	const [feature] = loadSupport(root).features;
	assert.equal(feature.implementations.engine, "unimplemented");
	assert.deepEqual(feature.phones.first, {
		note: "Recorded result.",
		status: "verified",
	});
});

void test("support composition rejects a phone result with no catalog feature", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(root, "site/src/data/support/phones/first.json");
	const phone = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	phone["unlisted-feature"] = { note: "", status: "verified" };
	writeFileSync(filename, JSON.stringify(phone));
	assert.throws(
		() => loadSupport(root),
		/Unknown first support feature: unlisted-feature/u
	);
});

void test("support composition rejects an implementation missing from a platform", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(
		root,
		"site/src/data/support/platforms/engine.json"
	);
	const platform = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	delete platform["second-feature"];
	writeFileSync(filename, JSON.stringify(platform));
	assert.throws(
		() => loadSupport(root),
		/Missing engine implementation for second-feature/u
	);
});

void test("support composition rejects a platform implementation with no catalog feature", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(
		root,
		"site/src/data/support/platforms/engine.json"
	);
	const platform = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	platform["unlisted-feature"] = "implemented";
	writeFileSync(filename, JSON.stringify(platform));
	assert.throws(
		() => loadSupport(root),
		/Unknown engine implementation feature: unlisted-feature/u
	);
});

void test("support composition rejects implementation values outside the platform states", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(
		root,
		"site/src/data/support/platforms/engine.json"
	);
	const platform = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	platform["second-feature"] = "verified";
	writeFileSync(filename, JSON.stringify(platform));
	assert.throws(() => loadSupport(root), z.ZodError);
});

for (const { field, value } of [
	{ field: "hardware", value: "connected" },
	{ field: "status", value: "device-tested" },
	{ field: "stock", value: "supported" },
]) {
	void test(`support composition rejects an explicit phone ${field} of ${value}`, (context) => {
		const root = supportCheckout(context);
		const filename = path.join(root, "site/src/data/support/phones/first.json");
		const phone = z
			.record(z.string(), z.unknown())
			.parse(JSON.parse(readFileSync(filename, "utf-8")));
		phone["first-feature"] = {
			note: "",
			status: "untested",
			stock: "unknown",
			[field]: value,
		};
		writeFileSync(filename, JSON.stringify(phone));
		assert.throws(() => loadSupport(root), z.ZodError);
	});
}

void test("support composition requires stock facts for a comparable function", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(root, "site/src/data/support/phones/first.json");
	const phone = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	phone["first-feature"] = {
		hardware: "present",
		note: "",
		status: "untested",
	};
	writeFileSync(filename, JSON.stringify(phone));
	assert.throws(
		() => loadSupport(root),
		/Missing first stock for first-feature/u
	);
});

void test("support composition rejects stock facts for an application without stock comparison", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(root, "site/src/data/support/phones/first.json");
	const phone = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	phone.viewer = { note: "", status: "verified", stock: "available" };
	writeFileSync(filename, JSON.stringify(phone));
	assert.throws(
		() => loadSupport(root),
		/Stock comparison does not apply to viewer/u
	);
});

void test("support composition rejects a stock note without a stock fact", (context) => {
	const root = supportCheckout(context);
	const filename = path.join(root, "site/src/data/support/phones/first.json");
	const phone = z
		.record(z.string(), z.unknown())
		.parse(JSON.parse(readFileSync(filename, "utf-8")));
	phone.viewer = { note: "", status: "verified", stockNote: "Unqualified." };
	writeFileSync(filename, JSON.stringify(phone));
	assert.throws(() => loadSupport(root), /Stock note requires a stock fact/u);
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
