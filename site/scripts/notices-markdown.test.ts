import assert from "node:assert/strict";
import test from "node:test";
import { satteri } from "@astrojs/markdown-satteri";
import { parseFragment } from "parse5";
import { noticesMarkdown } from "../src/notices-markdown.ts";

const cases = [
	{
		filename: "../THIRD_PARTY_NOTICES.md",
		headings: [{ depth: 2, slug: "license", text: "License" }],
		name: "root notices defer their leading title to the page and retain body headings",
		tags: ["h2", "p"],
	},
	{
		filename: "../OTHER_NOTICES.md",
		headings: [
			{ depth: 1, slug: "notices", text: "Notices" },
			{ depth: 2, slug: "license", text: "License" },
		],
		name: "other Markdown documents retain their leading title",
		tags: ["h1", "h2", "p"],
	},
];

for (const { filename, headings, name, tags } of cases) {
	void test(name, async () => {
		const renderer = await satteri({
			mdastPlugins: [noticesMarkdown],
		}).createRenderer({ syntaxHighlight: false });
		const result = await renderer.render(
			"# Notices\n\n## License\n\nPreserved copyright statement.\n",
			{ fileURL: new URL(filename, new URL("../", import.meta.url)) }
		);
		const elements = parseFragment(result.code).childNodes.filter(
			(node) => "tagName" in node
		);
		assert.deepEqual(
			elements.map((element) => element.tagName),
			tags
		);
		assert.deepEqual(result.metadata.headings, headings);
		assert.match(result.code, /Preserved copyright statement\./u);
	});
}
