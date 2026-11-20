import { createHash } from "node:crypto";
import { writeFile } from "node:fs/promises";
import path from "node:path";
import { build } from "esbuild";
import type { OfflineIcon } from "../../../src/scripts/offline-icons.ts";
import type { Element } from "../html/nodes.ts";

export const iconPaths = (svg: Element): string[] | undefined => {
	const paths: string[] = [];
	for (const child of svg.childNodes) {
		if ("value" in child && child.value.trim() === "") {
			continue;
		}
		if (
			!("tagName" in child) ||
			child.tagName !== "path" ||
			child.attrs.length !== 1 ||
			child.attrs[0].name !== "d"
		) {
			return undefined;
		}
		paths.push(child.attrs[0].value);
	}
	return paths.length === 0 ? undefined : paths;
};

export interface SharedIcons {
	references: Map<string, string>;
	script: string;
}

export const writeSharedIcons = async (
	pages: { allNodes: Element[] }[],
	site: string,
	output: string
): Promise<SharedIcons> => {
	const counts = new Map<string, { copies: number; paths: string[] }>();
	for (const { allNodes } of pages) {
		for (const node of allNodes) {
			if (node.tagName !== "svg") {
				continue;
			}
			const paths = iconPaths(node);
			if (paths !== undefined) {
				const key = JSON.stringify(paths);
				counts.set(key, { copies: (counts.get(key)?.copies ?? 0) + 1, paths });
			}
		}
	}
	const references = new Map<string, string>();
	const icons: OfflineIcon[] = [...counts.entries()]
		.filter(([, value]) => value.copies > 1)
		.map(([key, { paths }]) => {
			const id = `fplinux-offline-icon-${createHash("sha256").update(key).digest("hex").slice(0, 12)}`;
			references.set(key, id);
			return { id, paths };
		});
	const result = await build({
		bundle: true,
		format: "iife",
		minify: true,
		platform: "browser",
		stdin: {
			contents: `import { installOfflineIcons } from "./src/scripts/offline-icons.ts"; installOfflineIcons(${JSON.stringify(icons)});`,
			resolveDir: site,
			sourcefile: "offline-icons-entry.ts",
		},
		target: "es2022",
		write: false,
	});
	const content = result.outputFiles[0].text;
	const script = `offline/icons-${createHash("sha256").update(content).digest("hex").slice(0, 12)}.js`;
	await writeFile(path.join(output, script), content);
	return { references, script };
};
