import { stat } from "node:fs/promises";
import path from "node:path";
import { base as siteBase, site as siteURL } from "../../config/site.mjs";

export const base = `${siteBase.replace(/\/$/u, "")}/`;
const { origin } = new URL(siteURL);

export const localURL = async (
	value: string,
	from: string,
	output: string
): Promise<string> => {
	const local = value.startsWith(`${origin}${base}`)
		? value.slice(origin.length)
		: value;
	if (!local.startsWith(base)) {
		return local;
	}
	const url = new URL(local, origin);
	const destination = path.join(
		output,
		decodeURIComponent(url.pathname.slice(base.length))
	);
	const info = await stat(destination);
	const filename = info.isDirectory()
		? path.join(destination, "index.html")
		: destination;
	const relative = path
		.relative(path.dirname(from), filename)
		.split(path.sep)
		.map(encodeURIComponent)
		.join("/");
	return `${relative}${url.search}${url.hash}`;
};
