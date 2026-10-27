import { createHash } from "node:crypto";
import { readFile, readdir, stat } from "node:fs/promises";
import path from "node:path";
import { loadPhones, repositoryRoot } from "../src/data/phones.ts";

const siteFiles = async (directory: string): Promise<string[]> => {
	const entries = await readdir(directory, { withFileTypes: true });
	const selected = await Promise.all(
		entries
			.filter(
				(entry) =>
					!["node_modules", "dist", ".astro", ".data", ".private"].includes(
						entry.name
					)
			)
			.map(async (entry) => {
				const filename = path.join(directory, entry.name);
				if (entry.isSymbolicLink()) {
					throw new Error(`Unexpected site source symbolic link: ${filename}`);
				}
				return entry.isDirectory() ? await siteFiles(filename) : [filename];
			})
	);
	return selected.flat();
};

export const documentationDigest = async (
	root = repositoryRoot
): Promise<string> => {
	const digest = createHash("sha256");
	const { sources: manifests } = loadPhones(root);
	const filenames = [
		path.join(root, "THIRD_PARTY_NOTICES.md"),
		...(await siteFiles(path.join(root, "site"))),
		...manifests.map((source) => path.join(root, source)),
	].toSorted();
	const sources = await Promise.all(
		filenames.map(async (filename) => ({
			contents: await readFile(filename),
			filename,
			info: await stat(filename),
		}))
	);
	for (const { contents, filename, info } of sources) {
		const mode = Buffer.alloc(4);
		// oxlint-disable-next-line no-bitwise, numeric-separators-style -- The receipt encodes POSIX executable permissions.
		mode.writeUInt32BE(info.mode & 0o111 ? 0o100755 : 0o100644);
		digest.update(path.relative(root, filename).split(path.sep).join("/"));
		digest.update("\0");
		digest.update(mode);
		digest.update(contents);
		digest.update("\0");
	}
	return digest.digest("hex");
};
