import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
	cp,
	mkdir,
	readFile,
	readdir,
	rm,
	stat,
	writeFile,
} from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { build } from "esbuild";
import type { DefaultTreeAdapterMap } from "parse5";
import { parse, parseFragment, serialize } from "parse5";
import { base as siteBase, site as siteURL } from "../config/site.mjs";
import { compressFont } from "./offline-font.ts";
import { indexText } from "./offline-search-index.ts";
import { documentationDigest } from "./documentation-inputs.ts";
import type { OfflineIcon } from "../src/scripts/offline-icons.ts";
import type { SearchPage } from "../src/scripts/offline-search.ts";

type Node = DefaultTreeAdapterMap["node"];
type Element = DefaultTreeAdapterMap["element"];
const site = fileURLToPath(new URL("../", import.meta.url));
const raw = path.resolve(site, "../.cache/out/site-offline-raw");
const output = path.resolve(site, "../.cache/out/site-offline");
const base = `${siteBase.replace(/\/$/u, "")}/`;
const { origin } = new URL(siteURL);

const files = async (directory: string): Promise<string[]> => {
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

const elements = (node: Node): Element[] => {
	const own = "tagName" in node ? [node] : [];
	return [
		...own,
		...("childNodes" in node ? node.childNodes.flatMap(elements) : []),
	];
};

const allElements = (node: Node): Element[] => {
	const own = "tagName" in node ? [node] : [];
	const children = "childNodes" in node ? node.childNodes : [];
	const content = "content" in node ? [node.content] : [];
	return [...own, ...[...children, ...content].flatMap(allElements)];
};

const attr = (element: Element, name: string): string | undefined =>
	element.attrs.find((value) => value.name === name)?.value;

const remove = (element: Element): void => {
	const parent = element.parentNode;
	if (parent) {
		parent.childNodes = parent.childNodes.filter((child) => child !== element);
	}
};

const localURL = async (value: string, from: string): Promise<string> => {
	const local = value.startsWith(`${origin}${base}`)
		? value.slice(origin.length)
		: value;
	if (!local.startsWith(base)) {
		return local;
	}
	const url = new URL(local, origin);
	const destination = path.join(
		output,
		decodeURIComponent(url.pathname.slice(base.length))
	);
	const info = await stat(destination);
	const filename = info.isDirectory()
		? path.join(destination, "index.html")
		: destination;
	const relative = path
		.relative(path.dirname(from), filename)
		.split(path.sep)
		.map(encodeURIComponent)
		.join("/");
	return `${relative}${url.search}${url.hash}`;
};

const prepareHTML = async (filename: string) => {
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
				`import ${JSON.stringify(path.join(raw, src.slice(base.length)))};`
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

const writeSharedStyles = async (
	pages: Awaited<ReturnType<typeof prepareHTML>>[]
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

const iconPaths = (svg: Element): string[] | undefined => {
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

const writeSharedIcons = async (
	pages: Awaited<ReturnType<typeof prepareHTML>>[]
) => {
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

const buildRuntime = async (source: string): Promise<string> => {
	const result = await build({
		bundle: true,
		format: "iife",
		minify: true,
		platform: "browser",
		stdin: {
			contents: source,
			resolveDir: raw,
			sourcefile: "offline-runtime.js",
		},
		target: "es2022",
		write: false,
	});
	const content = result.outputFiles[0].text;
	const runtime = `offline/runtime-${createHash("sha256").update(content).digest("hex").slice(0, 12)}.js`;
	await writeFile(path.join(output, runtime), content);
	return runtime;
};

const rewriteAttributes = async (
	node: Element,
	filename: string
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
				value.value = await localURL(value.value, filename);
			}
			if (value.name === "srcset") {
				const candidates = await Promise.all(
					value.value.split(",").map(async (candidate) => {
						const [url, ...descriptor] = candidate.trim().split(/\s+/u);
						return `${await localURL(url, filename)} ${descriptor.join(" ")}`.trim();
					})
				);
				value.value = candidates.join(", ");
			}
		})
	);
};

const writeHTML = async (
	page: Awaited<ReturnType<typeof prepareHTML>>,
	runtime: string,
	sharedStyles: Map<string, string>,
	sharedIcons: Awaited<ReturnType<typeof writeSharedIcons>>
): Promise<void> => {
	const { allNodes, document, filename, nodes } = page;
	await Promise.all(
		allNodes.map(async (node) => {
			await rewriteAttributes(node, filename);
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

const rewriteCSS = async (filename: string): Promise<string[]> => {
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
			const relative = await localURL(url, filename);
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

const cssURLs = (source: string): string[] =>
	[...source.matchAll(/url\(\s*['"]?(?<url>[^)'"\s]+)['"]?\s*\)/gu)].map(
		(match) => match.groups?.url ?? ""
	);

const pruneUnusedFiles = async (pages: string[]): Promise<number> => {
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

const inputsDigest = await documentationDigest();
execFileSync(
	process.execPath,
	[path.join(site, "node_modules/.bin/astro"), "build", "--outDir", raw],
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
await rm(output, { force: true, recursive: true });
await cp(raw, output, { recursive: true });
await mkdir(path.join(output, "offline"));
const generated = await files(output);
const pages = await Promise.all(
	generated.filter((file) => file.endsWith(".html")).map(prepareHTML)
);
const sharedStyles = await writeSharedStyles(pages);
const sharedIcons = await writeSharedIcons(pages);
const imports = [...new Set(pages.flatMap((page) => page.imports))];
const inline = [...new Set(pages.flatMap((page) => page.inline))];
const runtime = await buildRuntime(
	`${imports.join("\n")}\n${inline.join("\n")}`
);
await Promise.all(
	pages.map(async (page) => {
		await writeHTML(page, runtime, sharedStyles, sharedIcons);
	})
);
const embeddedFontGroups = await Promise.all(
	[
		...generated.filter((file) => file.endsWith(".css")),
		...[...sharedStyles.values()].map((filename) =>
			path.join(output, filename)
		),
	].map(rewriteCSS)
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
const pruned = await pruneUnusedFiles(pages.map((page) => page.filename));
if ((await documentationDigest()) !== inputsDigest) {
	throw new Error(
		"Site sources changed during the offline export; rebuild it."
	);
}
interface ExportRecord {
	mode: number;
	sha256: string;
	size: number;
}
const outputFiles = await files(output);
const records: Record<string, ExportRecord> = Object.fromEntries(
	await Promise.all(
		outputFiles.map(async (filename): Promise<[string, ExportRecord]> => {
			const contents = await readFile(filename);
			const info = await stat(filename);
			return [
				path.relative(output, filename).split(path.sep).join("/"),
				{
					// oxlint-disable-next-line no-bitwise -- Retain the generated file's POSIX permissions.
					mode: info.mode & 0o777,
					sha256: createHash("sha256").update(contents).digest("hex"),
					size: contents.length,
				},
			];
		})
	)
);
await writeFile(
	path.resolve(output, "../site-offline-receipt.json"),
	`${JSON.stringify({ files: records, inputs_sha256: inputsDigest })}\n`
);
console.log(
	`Offline documentation: ${output}/index.html (${index.length} indexed pages, 1 runtime bundle, ${pruned} unused files removed)`
);
