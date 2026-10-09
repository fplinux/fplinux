import type { DefaultTreeAdapterMap } from "parse5";

type Node = DefaultTreeAdapterMap["node"];
type Element = DefaultTreeAdapterMap["element"];

const attr = (element: Element, name: string): string | undefined =>
	element.attrs.find((value) => value.name === name)?.value;

const text = (node: Node): string => {
	if ("value" in node) {
		return node.value;
	}
	if (
		"tagName" in node &&
		(attr(node, "data-pagefind-ignore") !== undefined ||
			["script", "style", "nav", "footer"].includes(node.tagName) ||
			(attr(node, "class") ?? "").split(/\s+/u).includes("fp-source"))
	) {
		return "";
	}
	return "childNodes" in node ? node.childNodes.map(text).join(" ") : "";
};

export const indexText = (node: Node): string =>
	text(node).replaceAll(/\s+/gu, " ").trim();
