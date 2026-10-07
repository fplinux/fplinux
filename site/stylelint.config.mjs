import config from "ultracite/stylelint";
import prettier from "./config/prettier.config.mjs";

export default {
	...config,
	ignoreFiles: [
		".astro/**",
		"dist/**",
		"node_modules/**",
		"public/**",
		"scripts/**",
	],
	overrides: [{ customSyntax: "postcss-html", files: ["**/*.astro"] }],
	rules: {
		...config.rules,
		// Keep declaration order, including fallbacks and shorthand overrides.
		"order/order": null,
		"order/properties-alphabetical-order": null,
		"order/properties-order": null,
		"prettier/prettier": [true, prettier],
		// Starlight owns the page-title ID.
		"selector-id-pattern": ["^(?:[a-z][a-z0-9]*(?:-[a-z0-9]+)*|_top)$"],
	},
};
