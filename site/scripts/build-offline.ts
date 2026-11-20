import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import {
	mkdir,
	mkdtemp,
	mkdtempDisposable,
	rename,
	rm,
	rmdir,
	writeFile,
} from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { managedDataRoot } from "./data-path.ts";
import { documentationDigest } from "./documentation-inputs.ts";
import { writeSharedIcons } from "./offline/assets/icons.ts";
import { buildRuntime } from "./offline/assets/runtime.ts";
import { rewriteCSS, writeSharedStyles } from "./offline/assets/styles.ts";
import { prepareHTML, writeHTML } from "./offline/html/document.ts";
import {
	files,
	pruneUnusedFiles,
} from "./offline/verification/reachability.ts";

const site = fileURLToPath(new URL("../", import.meta.url));

const publishOfflineSite = async (
	candidate: string,
	destination: string
): Promise<void> => {
	if (!existsSync(destination)) {
		await rename(candidate, destination);
		return;
	}
	const backup = await mkdtemp(
		path.join(path.dirname(destination), ".site-offline-previous-")
	);
	const previous = path.join(backup, "site");
	try {
		await rename(destination, previous);
	} catch (error) {
		await rmdir(backup);
		throw error;
	}
	try {
		await rename(candidate, destination);
	} catch (error) {
		await rename(previous, destination);
		await rmdir(backup);
		throw error;
	}
	await rm(backup, { recursive: true });
};

const destination = path.join(managedDataRoot(), "out/site-offline");
const parent = path.dirname(destination);
const checkout = path.resolve(site, "..");
const inputsDigest = await documentationDigest(checkout);
await mkdir(parent, { recursive: true });
await using staging = await mkdtempDisposable(
	path.join(parent, ".site-offline-")
);
const output = path.join(staging.path, "site");
execFileSync(
	process.execPath,
	[path.join(site, "node_modules/.bin/astro"), "build", "--outDir", output],
	{
		cwd: site,
		env: {
			...process.env,
			ASTRO_TELEMETRY_DISABLED: "1",
			FPLINUX_SITE_OFFLINE: "1",
		},
		stdio: "inherit",
	}
);
await mkdir(path.join(output, "offline"));
const generated = await files(output);
const pages = await Promise.all(
	generated
		.filter((file) => file.endsWith(".html"))
		.map(async (filename) => await prepareHTML(filename, output))
);
const sharedStyles = await writeSharedStyles(pages, output);
const sharedIcons = await writeSharedIcons(pages, site, output);
const imports = [...new Set(pages.flatMap((page) => page.imports))];
const inline = [...new Set(pages.flatMap((page) => page.inline))];
const runtime = await buildRuntime(
	`${imports.join("\n")}\n${inline.join("\n")}`,
	output
);
await Promise.all(
	pages.map(async (page) => {
		await writeHTML(page, output, runtime, sharedStyles, sharedIcons);
	})
);
const embeddedFontGroups = await Promise.all(
	[
		...generated.filter((file) => file.endsWith(".css")),
		...[...sharedStyles.values()].map((filename) =>
			path.join(output, filename)
		),
	].map(async (filename) => await rewriteCSS(filename, output))
);
const embeddedFonts = [...new Set(embeddedFontGroups.flat())];
await Promise.all(
	embeddedFonts.map(async (filename) => {
		await rm(filename);
	})
);
const index = pages
	.map((page) => page.entry)
	.filter((entry) => entry !== undefined);
await writeFile(
	path.join(output, "offline/search-index.js"),
	`window.fplinuxOfflineIndex=${JSON.stringify(index)};\n`
);
const pruned = await pruneUnusedFiles(
	pages.map((page) => page.filename),
	output
);
if ((await documentationDigest(checkout)) !== inputsDigest) {
	throw new Error(
		"Site sources changed during the offline export; rebuild it."
	);
}
await publishOfflineSite(output, destination);
console.log(
	`Offline documentation: ${destination}/index.html (${index.length} indexed pages, 1 runtime bundle, ${pruned} unused files removed)`
);
