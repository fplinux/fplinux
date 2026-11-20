import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { serialize } from "parse5";
import type { Element } from "../html/nodes.ts";
import { base, localURL } from "../paths.ts";
import { compressFont } from "./font.ts";

export const writeSharedStyles = async (
	pages: { nodes: Element[] }[],
	output: string
): Promise<Map<string, string>> => {
	const counts = new Map<string, number>();
	for (const { nodes } of pages) {
		for (const node of nodes) {
			if (node.tagName === "style" && node.attrs.length === 0) {
				const css = serialize(node);
				counts.set(css, (counts.get(css) ?? 0) + 1);
			}
		}
	}
	const repeated = [...counts.entries()].filter(([, copies]) => copies > 1);
	const styles = await Promise.all(
		repeated.map(async ([css]): Promise<[string, string]> => {
			const filename = `offline/styles-${createHash("sha256").update(css).digest("hex").slice(0, 12)}.css`;
			await writeFile(path.join(output, filename), css);
			return [css, filename];
		})
	);
	return new Map(styles);
};

export const rewriteCSS = async (
	filename: string,
	output: string
): Promise<string[]> => {
	const source = await readFile(filename, "utf-8");
	const urls = [
		...source.matchAll(
			/url\((?<quote>['"]?)(?<url>\/[^)'"\s]+)\k<quote>\)(?<format>\s*format\([^)]+\))?/gu
		),
	].filter((match) => match.groups?.url.startsWith(base) ?? false);
	const replacements = await Promise.all(
		urls.map(async (match) => {
			const url = match.groups?.url;
			if (url === undefined) {
				throw new Error("Missing CSS URL capture");
			}
			const relative = await localURL(url, filename, output);
			let value = relative;
			let fontPath: string | undefined;
			let format = match.groups?.format ?? "";
			if (/\.(?:ttf|woff2?)(?:$|\?)/u.test(relative)) {
				fontPath = path.join(path.dirname(filename), relative);
				const font = await readFile(fontPath);
				const compressed = await compressFont(font);
				value = `data:font/woff2;base64,${Buffer.from(compressed).toString("base64")}`;
				format = 'format("woff2")';
			}
			return { after: `url("${value}")${format}`, before: match[0], fontPath };
		})
	);
	let rewritten = source;
	for (const replacement of replacements) {
		rewritten = rewritten.replace(replacement.before, replacement.after);
	}
	await writeFile(filename, rewritten);
	return replacements.flatMap(({ fontPath }) =>
		fontPath === undefined ? [] : [fontPath]
	);
};

export const cssURLs = (source: string): string[] =>
	[...source.matchAll(/url\(\s*['"]?(?<url>[^)'"\s]+)['"]?\s*\)/gu)].map(
		(match) => match.groups?.url ?? ""
	);
