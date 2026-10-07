import { defineConfig } from 'oxlint';
import core from 'ultracite/oxlint/core';

export default defineConfig({
	extends: [core],
	ignorePatterns: [
		...(core.ignorePatterns ?? []),
		// ESLint and Astro check own component templates and frontmatter.
		'**/*.astro',
		'.astro/**',
		'dist/**',
		'public/**',
		'scripts/**',
		'src/assets/**',
		'src/fonts/**',
	],
	rules: {
		// Configuration fields follow their role, matching the source-format contract.
		'sort-keys': 'off',
	},
	overrides: [
		{
			files: ['astro.config.mjs'],
			// Inline JSDoc types the variadic page helper.
			rules: { 'no-inline-comments': 'off' },
		},
	],
});
