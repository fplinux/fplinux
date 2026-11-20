import { readFile, readdir, rm } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { parse, serialize } from "parse5";
import { cssURLs } from "../assets/styles.ts";
import { allElements, attr } from "../html/nodes.ts";

export const files = async (directory: string): Promise<string[]> => {
	const entries = await readdir(directory, { withFileTypes: true });
	const nested = await Promise.all(
		entries.map(async (entry) => {
			const filename = path.join(directory, entry.name);
			if (entry.isSymbolicLink()) {
				throw new Error(`Unexpected symbolic link: ${filename}`);
			}
			return entry.isDirectory() ? await files(filename) : [filename];
		})
	);
	return nested.flat().toSorted();
};

export const pruneUnusedFiles = async (
	pages: string[],
	output: string
): Promise<number> => {
	const generated = await files(output);
	const available = new Set(generated);
	const reachable = new Set(pages);
	const pending = [...pages];
	// Bundled classic scripts are self-contained; HTML and CSS own local assets.
	const dependencies = new Map(
		await Promise.all(
			generated.map(async (filename): Promise<[string, string[]]> => {
				let references: string[] = [];
				if (filename.endsWith(".html") || filename.endsWith(".svg")) {
					const nodes = allElements(parse(await readFile(filename, "utf-8")));
					references = nodes.flatMap((node) => [
						...node.attrs
							.filter((value) => ["href", "src", "poster"].includes(value.name))
							.map((value) => value.value),
						...(attr(node, "srcset") ?? "")
							.split(",")
							.map((candidate) => candidate.trim().split(/\s+/u)[0]),
						...cssURLs(attr(node, "style") ?? ""),
						...(node.tagName === "style" ? cssURLs(serialize(node)) : []),
					]);
				} else if (filename.endsWith(".css")) {
					references = cssURLs(await readFile(filename, "utf-8"));
				}
				return [filename, references];
			})
		)
	);
	for (const filename of pending) {
		for (const reference of dependencies.get(filename) ?? []) {
			if (reference === "") {
				continue;
			}
			const url = new URL(reference, pathToFileURL(filename));
			if (url.protocol !== "file:") {
				continue;
			}
			url.search = "";
			url.hash = "";
			const destination = fileURLToPath(url);
			if (!available.has(destination)) {
				throw new Error(`Missing local resource: ${filename} → ${reference}`);
			}
			if (!reachable.has(destination)) {
				reachable.add(destination);
				pending.push(destination);
			}
		}
	}
	const unused = generated.filter((filename) => !reachable.has(filename));
	await Promise.all(
		unused.map(async (filename) => {
			await rm(filename);
		})
	);
	return unused.length;
};
