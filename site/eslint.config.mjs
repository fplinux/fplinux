import typescript from "@typescript-eslint/eslint-plugin";
import astro from "ultracite/eslint/astro";

export default [
	...astro,
	{
		ignores: [
			"**/*.json",
			"**/*.jsonc",
			".astro/**",
			"dist/**",
			"public/**",
			"scripts/**",
			"src/assets/**",
			"src/fonts/**",
		],
	},
	{
		linterOptions: { reportUnusedDisableDirectives: "error" },
	},
	{
		files: ["**/*.astro"],
		languageOptions: {
			parserOptions: {
				extraFileExtensions: [".astro"],
				project: true,
				tsconfigRootDir: import.meta.dirname,
			},
		},
		plugins: { "@typescript-eslint": typescript },
		rules: {
			...typescript.configs["recommended-type-checked-only"].rules,
			"sort-keys": "error",
		},
	},
	{
		files: ["src/components/Hero.astro"],
		// Page frontmatter owns the markup; composed screen previews share one label.
		rules: {
			"astro/jsx-a11y/prefer-tag-over-role": "off",
			"astro/no-set-html-directive": "off",
		},
	},
	{
		files: ["src/components/Hero.astro", "src/components/FeatureCatalog.astro"],
		// Image components and repeated sections create these selectors' elements.
		rules: { "astro/no-unused-css-selector": "off" },
	},
];
