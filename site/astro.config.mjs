// @ts-check
import { defineConfig } from "astro/config";
import { satteri } from "@astrojs/markdown-satteri";
import starlight from "@astrojs/starlight";
import starlightLinksValidator from "starlight-links-validator";
import starlightImageZoom from "starlight-image-zoom";
import { pluginCollapsibleSections } from "@expressive-code/plugin-collapsible-sections";
import { base, site } from "./config/site.mjs";
import { noticesMarkdown } from "./src/notices-markdown.ts";

const offline = process.env.FPLINUX_SITE_OFFLINE === "1";

// oxlint-disable-next-line no-inline-comments -- The rest parameter needs its JSDoc type.
const pages = (/** @type {string[]} */ ...slugs) =>
	slugs.map((slug) => ({ slug }));

export default defineConfig({
	base,
	integrations: [
		starlight({
			components: {
				Hero: "./src/components/Hero.astro",
				PageTitle: "./src/components/PageTitle.astro",
				...(offline ? { Search: "./src/components/OfflineSearch.astro" } : {}),
			},
			customCss: ["./src/styles/custom.css"],
			description: "Linux for feature phones built on the Unisoc UMS9117.",
			editLink: {
				baseUrl: "https://github.com/fplinux/fplinux/edit/main/site/",
			},
			expressiveCode: { plugins: [pluginCollapsibleSections()] },
			favicon: "/favicon.svg",
			head: [
				{
					attrs: { href: `${base}/favicon.ico`, rel: "icon", sizes: "48x48" },
					tag: "link",
				},
			],
			logo: {
				dark: "./src/assets/brand/fplinux-lockup-dark.svg",
				light: "./src/assets/brand/fplinux-lockup.svg",
				replacesTitle: true,
			},
			pagefind: !offline,
			plugins: [starlightLinksValidator(), starlightImageZoom()],
			routeMiddleware: "./src/notices-route-data.ts",
			sidebar: [
				{ items: pages("start/build", "start/run"), label: "Get started" },
				{
					items: pages(
						"features",
						"use/terminal",
						"use/connect",
						"use/audio",
						"use/fm-radio",
						"use/bluetooth",
						"use/camera",
						"use/display-and-lights",
						"use/microsd",
						"use/power",
						"use/cpu-and-temperature"
					),
					label: "Features",
				},
				{
					items: [{ autogenerate: { directory: "phones" } }],
					label: "Phones",
				},
				{
					items: pages(
						"guides/device-data",
						"guides/dependencies",
						"guides/microsd-system-card",
						"guides/packages",
						"guides/standalone-archive"
					),
					label: "Guides",
				},
				{
					items: pages(
						"apps",
						"apps/brightness",
						"apps/armada",
						"apps/tyrquake",
						"apps/ffmpeg",
						"apps/image-tools/rotate",
						"apps/image-tools/jpeg",
						"apps/image-tools/present"
					),
					label: "Applications",
				},
				{
					items: [
						...pages(
							"develop/contributing",
							"develop/debugging",
							"develop/packaging",
							"develop/device-interfaces",
							"develop/input",
							"develop/raw-image-formats",
							"develop/logging"
						),
						{
							items: pages(
								"develop/code-style",
								"develop/code-style/c",
								"develop/code-style/python",
								"develop/code-style/shell",
								"develop/code-style/formats"
							),
							label: "Code style",
						},
					],
					label: "Develop",
				},
				{
					items: pages(
						"porting",
						"porting/checklist",
						"porting/bring-up",
						"porting/linux-integration",
						"porting/ums9117",
						"porting/identity"
					),
					label: "Porting",
				},
				{
					items: pages(
						"help/troubleshooting",
						"help/report",
						"help/faq",
						"help/glossary"
					),
					label: "Help",
				},
				{ items: pages("about", "about/notices"), label: "About" },
			],
			social: [
				{
					href: "https://github.com/fplinux/fplinux",
					icon: "github",
					label: "GitHub",
				},
			],
			title: "FPLinux",
		}),
	],
	markdown: { processor: satteri({ mdastPlugins: [noticesMarkdown] }) },
	prefetch: offline ? false : undefined,
	site,
});
