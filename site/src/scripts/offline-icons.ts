export interface OfflineIcon {
	id: string;
	paths: string[];
}

export const installOfflineIcons = (icons: OfflineIcon[]): void => {
	const namespace = "http://www.w3.org/2000/svg";
	const sprite = document.createElementNS(namespace, "svg");
	sprite.setAttribute("aria-hidden", "true");
	sprite.setAttribute("focusable", "false");
	sprite.setAttribute("width", "0");
	sprite.setAttribute("height", "0");
	sprite.style.position = "absolute";
	const definitions = document.createElementNS(namespace, "defs");
	for (const icon of icons) {
		const group = document.createElementNS(namespace, "g");
		group.id = icon.id;
		for (const geometry of icon.paths) {
			const shape = document.createElementNS(namespace, "path");
			shape.setAttribute("d", geometry);
			group.append(shape);
		}
		definitions.append(group);
	}
	sprite.append(definitions);
	document.body.prepend(sprite);
};
