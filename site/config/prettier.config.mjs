// Loaded explicitly so repository formatting keeps its own configuration.
import preset from "ultracite/prettier";

const config = { ...preset };
// Plain CSS needs no Tailwind formatting plugin.
delete config.tailwindFunctions;

export default {
	...config,
	overrides: [
		{
			files: ["*.md", "*.mdx", "*.json", "*.jsonc", "*.yml", "*.yaml"],
			options: { useTabs: false },
		},
		{
			files: ["*.md", "*.mdx"],
			options: { embeddedLanguageFormatting: "off" },
		},
		{ files: "*.jsonc", options: { trailingComma: "none" } },
	],
	plugins: ["prettier-plugin-astro"],
	proseWrap: "preserve",
	useTabs: true,
};
