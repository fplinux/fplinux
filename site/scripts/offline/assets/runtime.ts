import { createHash } from "node:crypto";
import { writeFile } from "node:fs/promises";
import path from "node:path";
import { build } from "esbuild";

export const buildRuntime = async (
	source: string,
	output: string
): Promise<string> => {
	const result = await build({
		bundle: true,
		format: "iife",
		minify: true,
		platform: "browser",
		stdin: {
			contents: source,
			resolveDir: output,
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
