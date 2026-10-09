import { defineConfig } from "oxlint";
import core from "ultracite/oxlint/core";

export default defineConfig({
	extends: [core],
	ignorePatterns: [
		...(core.ignorePatterns ?? []),
		// ESLint and Astro check own component templates and frontmatter.
		"**/*.astro",
		".astro/**",
		"dist/**",
		"public/**",
		"src/assets/**",
		"src/fonts/**",
	],
	options: {
		reportUnusedDisableDirectives: "error",
		typeAware: true,
	},
});
