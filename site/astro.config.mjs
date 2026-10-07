// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import starlightLinksValidator from 'starlight-links-validator';
import starlightImageZoom from 'starlight-image-zoom';
import { pluginCollapsibleSections } from '@expressive-code/plugin-collapsible-sections';

const pages = (/** @type {string[]} */ ...slugs) =>
	slugs.map((slug) => ({ slug }));

export default defineConfig({
	site: 'https://fplinux.github.io',
	base: '/fplinux',
	integrations: [
		starlight({
			title: 'FPLinux',
			description: 'Linux for feature phones built on the Unisoc UMS9117.',
			logo: {
				light: './src/assets/brand/fplinux-lockup.svg',
				dark: './src/assets/brand/fplinux-lockup-dark.svg',
				replacesTitle: true,
			},
			favicon: '/favicon.svg',
			head: [
				{
					tag: 'link',
					attrs: { rel: 'icon', href: '/fplinux/favicon.ico', sizes: '48x48' },
				},
			],
			social: [
				{
					icon: 'github',
					label: 'GitHub',
					href: 'https://github.com/fplinux/fplinux',
				},
			],
			editLink: {
				baseUrl: 'https://github.com/fplinux/fplinux/edit/main/site/',
			},
			customCss: ['./src/styles/custom.css'],
			components: {
				Hero: './src/components/Hero.astro',
				PageTitle: './src/components/PageTitle.astro',
			},
			plugins: [starlightLinksValidator(), starlightImageZoom()],
			expressiveCode: { plugins: [pluginCollapsibleSections()] },
			sidebar: [
				{ label: 'Get started', items: pages('start/build', 'start/run') },
				{
					label: 'Features',
					items: pages(
						'features',
						'use/terminal',
						'use/connect',
						'use/audio',
						'use/fm-radio',
						'use/bluetooth',
						'use/camera',
						'use/display-and-lights',
						'use/microsd',
						'use/power',
						'use/cpu-and-temperature'
					),
				},
				{
					label: 'Phones',
					items: pages(
						'phones',
						'phones/nokia-3210-4g',
						'phones/inoi-240-modern-4g',
						'phones/inoi-244-modern-4g',
						'phones/maxvi-k15n-4g'
					),
				},
				{
					label: 'Guides',
					items: pages(
						'guides/device-data',
						'guides/dependencies',
						'guides/microsd-system-card',
						'guides/packages',
						'guides/standalone-archive'
					),
				},
				{
					label: 'Applications',
					items: pages(
						'apps',
						'apps/brightness',
						'apps/armada',
						'apps/tyrquake',
						'apps/ffmpeg',
						'apps/image-tools/rotate',
						'apps/image-tools/jpeg',
						'apps/image-tools/present'
					),
				},
				{
					label: 'Develop',
					items: [
						...pages(
							'develop/contributing',
							'develop/debugging',
							'develop/packaging',
							'develop/device-interfaces',
							'develop/input',
							'develop/raw-image-formats',
							'develop/logging'
						),
						{
							label: 'Code style',
							items: pages(
								'develop/code-style',
								'develop/code-style/c',
								'develop/code-style/python',
								'develop/code-style/shell',
								'develop/code-style/formats'
							),
						},
					],
				},
				{
					label: 'Porting',
					items: pages(
						'porting',
						'porting/checklist',
						'porting/bring-up',
						'porting/linux-integration',
						'porting/ums9117',
						'porting/identity'
					),
				},
				{
					label: 'Help',
					items: pages(
						'help/troubleshooting',
						'help/report',
						'help/faq',
						'help/glossary'
					),
				},
				{ label: 'About', items: pages('about', 'about/notices') },
			],
		}),
	],
});
