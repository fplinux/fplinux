// Loaded explicitly so repository formatting keeps its own configuration.
import preset from "ultracite/prettier";

const config = { ...preset };
const plugins = ["prettier-plugin-astro"];
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
		{
			files: "*.mdx",
			options: {
				plugins: [
					...plugins,
					"@tofrankie/prettier-plugin-compact-markdown-table",
				],
				proseWrap: "never",
				tableLayout: "compact",
			},
		},
		{ files: "*.jsonc", options: { trailingComma: "none" } },
	],
	plugins,
	proseWrap: "preserve",
	useTabs: true,
};
