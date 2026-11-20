import type { DefaultTreeAdapterMap } from "parse5";

export type Node = DefaultTreeAdapterMap["node"];
export type Element = DefaultTreeAdapterMap["element"];

export const elements = (node: Node): Element[] => {
	const own = "tagName" in node ? [node] : [];
	return [
		...own,
		...("childNodes" in node ? node.childNodes.flatMap(elements) : []),
	];
};

export const allElements = (node: Node): Element[] => {
	const own = "tagName" in node ? [node] : [];
	const children = "childNodes" in node ? node.childNodes : [];
	const content = "content" in node ? [node.content] : [];
	return [...own, ...[...children, ...content].flatMap(allElements)];
};

export const attr = (element: Element, name: string): string | undefined =>
	element.attrs.find((value) => value.name === name)?.value;

export const remove = (element: Element): void => {
	const parent = element.parentNode;
	if (parent) {
		parent.childNodes = parent.childNodes.filter((child) => child !== element);
	}
};
