import typescript from "@typescript-eslint/eslint-plugin";
import parser from "@typescript-eslint/parser";
import { configs } from "eslint-plugin-astro";

export default [
	...configs["flat/all"],
	{
		ignores: [
			"**/*.json",
			"**/*.jsonc",
			".astro/**",
			"**/.data/**",
			"**/.private/**",
			"dist/**",
			"public/**",
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
				parser,
				project: true,
				tsconfigRootDir: import.meta.dirname,
			},
		},
		plugins: { "@typescript-eslint": typescript },
		rules: {
			...typescript.configs["recommended-type-checked-only"].rules,
			// Prettier formats component frontmatter.
			"astro/semi": "off",
			"sort-keys": "error",
		},
	},
	{
		// The processor's client-side scripts do not exist on disk.
		...typescript.configs["flat/disable-type-checked"],
		files: ["**/*.astro/*.js", "**/*.astro/*.ts"],
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
