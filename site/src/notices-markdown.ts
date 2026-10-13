import type { MdastPluginEntry } from "satteri";

const noticesURL = new URL("../../THIRD_PARTY_NOTICES.md", import.meta.url);

export const noticesMarkdown: MdastPluginEntry = ({ fileURL }) => {
	if (fileURL?.href !== noticesURL.href) {
		return null;
	}
	return {
		before: (root, context) => {
			const [first] = root.children;
			if (first?.type === "heading" && first.depth === 1) {
				context.removeNode(first);
			}
		},
		name: "notices-page-title",
	};
};
