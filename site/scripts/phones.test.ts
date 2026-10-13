import assert from "node:assert/strict";
import {
	mkdtempSync,
	mkdirSync,
	readFileSync,
	rmSync,
	writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { loadPhones, phonePageData } from "../src/data/phones.ts";
import { documentationDigest } from "./documentation-inputs.ts";

const phoneCheckout = (root: string) => {
	const contents: Record<string, string> = {
		"THIRD_PARTY_NOTICES.md": "Synthetic redistribution notice\n",
		"platforms/example/platform.toml":
			'[identity]\nvendor = "Example"\nsoc = "CHIP"\naliases = ["ALIAS"]\n',
		"site/src/data/phones.json":
			'[{"id":"second","slug":"phones/curated-first"},{"id":"first","slug":"phones/curated-second","render":"preview","renderScale":2}]\n',
		"targets/first/display/panel.toml": "width = 123\nheight = 456\n",
		"targets/first/target.toml":
			'platform = "example"\n[identity]\nbrand = "First"\nproduct = "Phone"\nhardware_codes = ["HW-1"]\n[adapter]\nboot_instructions = "Hold the first key."\n[display]\npanel = "display/panel.toml"\n[microsd]\n',
		"targets/second/display/panel.toml": "width = 456\nheight = 123\n",
		"targets/second/target.toml":
			'platform = "example"\n[identity]\nbrand = "Second"\nproduct = "Phone"\nhardware_codes = []\n[adapter]\nboot_instructions = "Hold the second key."\n[display]\npanel = "display/panel.toml"\n',
	};
	for (const [relative, data] of Object.entries(contents)) {
		const filename = path.join(root, relative);
		mkdirSync(path.dirname(filename), { recursive: true });
		writeFileSync(filename, data);
	}
};

void test("phone consumers use manifest identity, geometry and build availability in editorial order", (context) => {
	const root = mkdtempSync(path.join(tmpdir(), "fplinux-phones-"));
	context.after(() => {
		rmSync(root, { force: true, recursive: true });
	});
	phoneCheckout(root);
	const { phones } = loadPhones(root);
	assert.deepEqual(
		phones.map((phone) => ({
			boot: phone.bootInstructions,
			display: phone.display,
			name: phone.name,
			profiles: phone.profiles,
			route: phone.slug,
		})),
		[
			{
				boot: "Hold the second key.",
				display: { height: 123, width: 456 },
				name: "Second Phone",
				profiles: ["default"],
				route: "phones/curated-first",
			},
			{
				boot: "Hold the first key.",
				display: { height: 456, width: 123 },
				name: "First Phone (HW-1)",
				profiles: ["default", "microsd-uboot"],
				route: "phones/curated-second",
			},
		]
	);
	assert.deepEqual(
		phonePageData(
			"phones/curated-second",
			{ qualification: "Unknown" },
			phones
		),
		{
			description:
				"Boot key, controls, storage, power and limitations of FPLinux on the First Phone (HW-1).",
			phone: "first",
			qualification: "Unknown",
			sidebar: { order: 2 },
			source: ["targets/first"],
			title: "First Phone (HW-1)",
		}
	);
	const manifest = path.join(root, "targets/first/target.toml");
	writeFileSync(
		manifest,
		readFileSync(manifest, "utf-8").replace(
			'product = "Phone"',
			'product = "Renamed"'
		)
	);
	assert.equal(
		phonePageData("phones/curated-second", {}, loadPhones(root).phones).title,
		"First Renamed (HW-1)"
	);
});

void test("documentation receipts track phone sources and retain unrelated driver reuse", async (context) => {
	const root = mkdtempSync(path.join(tmpdir(), "fplinux-doc-inputs-"));
	context.after(() => {
		rmSync(root, { force: true, recursive: true });
	});
	phoneCheckout(root);
	const original = await documentationDigest(root);
	const driver = path.join(root, "targets/first/linux/driver.c");
	mkdirSync(path.dirname(driver), { recursive: true });
	writeFileSync(driver, "/* unrelated implementation */\n");
	assert.equal(await documentationDigest(root), original);
	for (const relative of [
		"THIRD_PARTY_NOTICES.md",
		"targets/first/target.toml",
		"platforms/example/platform.toml",
		"targets/first/display/panel.toml",
	]) {
		const filename = path.join(root, relative);
		const contents = readFileSync(filename, "utf-8");
		writeFileSync(filename, `${contents}# updated input\n`);
		// oxlint-disable-next-line no-await-in-loop -- Each causal input edit is restored before the next case.
		assert.notEqual(await documentationDigest(root), original);
		writeFileSync(filename, contents);
	}
});
