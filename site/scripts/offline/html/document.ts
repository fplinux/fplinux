import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { parse, parseFragment, serialize } from "parse5";
import type { SearchPage } from "../../../src/scripts/offline-search.ts";
import type { SharedIcons } from "../assets/icons.ts";
import { iconPaths } from "../assets/icons.ts";
import { base, localURL } from "../paths.ts";
import type { Element } from "./nodes.ts";
import { allElements, attr, elements, remove } from "./nodes.ts";
import { indexText } from "./search-index.ts";

export const prepareHTML = async (filename: string, output: string) => {
	const document = parse(await readFile(filename, "utf-8"));
	const nodes = elements(document);
	const heading = nodes.find((node) => node.tagName === "h1");
	const main = nodes.find((node) => node.tagName === "main");
	const entry: SearchPage | undefined =
		main && heading && path.basename(filename) !== "404.html"
			? {
					text: indexText(main),
					title: indexText(heading),
					url: path
						.relative(output, filename)
						.split(path.sep)
						.map(encodeURIComponent)
						.join("/"),
				}
			: undefined;
	// Template scripts remain inert; only active modules enter the runtime.
	const modules = nodes.filter(
		(node) => node.tagName === "script" && attr(node, "type") === "module"
	);
	const imports: string[] = [];
	const inline: string[] = [];
	for (const module of modules) {
		const src = attr(module, "src");
		if (src !== undefined && src !== "") {
			if (!src.startsWith(base)) {
				throw new Error(`Unexpected script URL: ${src}`);
			}
			imports.push(
				`import ${JSON.stringify(path.join(output, src.slice(base.length)))};`
			);
		} else {
			inline.push(`(() => {${serialize(module)}})();`);
		}
		remove(module);
	}
	return {
		allNodes: allElements(document),
		document,
		entry,
		filename,
		imports,
		inline,
		nodes,
	};
};

type OfflinePage = Awaited<ReturnType<typeof prepareHTML>>;

const rewriteAttributes = async (
	node: Element,
	filename: string,
	output: string
): Promise<void> => {
	if (
		node.tagName === "link" &&
		["canonical", "sitemap", "modulepreload"].includes(attr(node, "rel") ?? "")
	) {
		remove(node);
		return;
	}
	await Promise.all(
		node.attrs.map(async (value) => {
			if (["href", "src", "poster"].includes(value.name)) {
				value.value = await localURL(value.value, filename, output);
			}
			if (value.name === "srcset") {
				const candidates = await Promise.all(
					value.value.split(",").map(async (candidate) => {
						const [url, ...descriptor] = candidate.trim().split(/\s+/u);
						return `${await localURL(url, filename, output)} ${descriptor.join(" ")}`.trim();
					})
				);
				value.value = candidates.join(", ");
			}
		})
	);
};

export const writeHTML = async (
	page: OfflinePage,
	output: string,
	runtime: string,
	sharedStyles: Map<string, string>,
	sharedIcons: SharedIcons
): Promise<void> => {
	const { allNodes, document, filename, nodes } = page;
	await Promise.all(
		allNodes.map(async (node) => {
			await rewriteAttributes(node, filename, output);
		})
	);
	const body = nodes.find((node) => node.tagName === "body");
	if (body === undefined) {
		throw new Error(`Missing body: ${filename}`);
	}
	const relative = (value: string) =>
		path
			.relative(path.dirname(filename), path.join(output, value))
			.split(path.sep)
			.join("/");
	for (const node of allNodes) {
		if (node.tagName === "svg") {
			const paths = iconPaths(node);
			const id =
				paths === undefined
					? undefined
					: sharedIcons.references.get(JSON.stringify(paths));
			if (id !== undefined) {
				const fragment = parseFragment(node, `<use href="#${id}"></use>`, {});
				node.childNodes = fragment.childNodes;
				for (const child of node.childNodes) {
					child.parentNode = node;
				}
			}
		}
	}
	for (const node of nodes) {
		if (node.tagName === "style" && node.attrs.length === 0) {
			const stylesheet = sharedStyles.get(serialize(node));
			if (stylesheet !== undefined) {
				node.nodeName = "link";
				node.tagName = "link";
				node.attrs = [
					{ name: "href", value: relative(stylesheet) },
					{ name: "rel", value: "stylesheet" },
				];
				node.childNodes = [];
			}
		}
	}
	const bootstrap = parseFragment(
		`<script src="${relative(sharedIcons.script)}"></script>`
	);
	for (const child of bootstrap.childNodes) {
		child.parentNode = body;
	}
	body.childNodes.unshift(...bootstrap.childNodes);
	const additions = parseFragment(
		`<script data-offline-index src="${relative("offline/search-index.js")}" defer></script><script src="${relative(runtime)}" defer></script>`
	);
	for (const child of additions.childNodes) {
		child.parentNode = body;
		body.childNodes.push(child);
	}
	await writeFile(filename, serialize(document));
};
