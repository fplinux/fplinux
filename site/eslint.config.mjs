import astro from 'ultracite/eslint/astro';

export default [
	...astro,
	{
		ignores: [
			'**/*.json',
			'**/*.jsonc',
			'.astro/**',
			'dist/**',
			'public/**',
			'scripts/**',
			'src/assets/**',
			'src/fonts/**',
		],
	},
	{
		files: ['src/components/Hero.astro'],
		// Page frontmatter owns the markup; composed screen previews share one label.
		rules: {
			'astro/no-set-html-directive': 'off',
			'astro/jsx-a11y/prefer-tag-over-role': 'off',
		},
	},
	{
		files: ['src/components/Hero.astro', 'src/components/FeatureCatalog.astro'],
		// Image components and repeated sections create these selectors' elements.
		rules: { 'astro/no-unused-css-selector': 'off' },
	},
];
