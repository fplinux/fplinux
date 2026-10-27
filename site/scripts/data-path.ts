import { execFileSync } from "node:child_process";
import path from "node:path";
import { repositoryRoot } from "../src/data/phones.ts";

export const managedDataRoot = (): string => {
	const output = execFileSync(
		path.join(repositoryRoot, "fplinux"),
		["data-path", "--check"],
		{
			cwd: repositoryRoot,
			encoding: "utf-8",
			stdio: ["ignore", "pipe", "inherit"],
		}
	);
	const selected = output.endsWith("\n") ? output.slice(0, -1) : output;
	if (
		!path.isAbsolute(selected) ||
		["\r", "\n", "\0"].some((character) => selected.includes(character))
	) {
		throw new Error("FPLinux data-path did not return one absolute directory.");
	}
	return selected;
};
