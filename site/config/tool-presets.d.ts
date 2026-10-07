// The JavaScript presets expose configuration objects without declarations.
declare module "ultracite/prettier" {
	import type { Config } from "prettier";

	const config: Config & {
		tailwindFunctions?: string[];
	};
	export default config;
}

declare module "ultracite/stylelint" {
	import type { Config } from "stylelint";

	const config: Config;
	export default config;
}

declare module "ultracite/eslint/astro" {
	import type { Linter } from "eslint";

	const config: Linter.Config[];
	export default config;
}
