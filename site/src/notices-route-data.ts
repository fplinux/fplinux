import { defineRouteMiddleware } from "@astrojs/starlight/route-data";
import { getHeadings } from "../../THIRD_PARTY_NOTICES.md";

export const onRequest = defineRouteMiddleware((context) => {
	const route = context.locals.starlightRoute;
	if (route.entry.id !== "about/notices") {
		return;
	}
	// Imported Markdown renders outside the page's own heading collection.
	const headings = getHeadings();
	route.headings.push(...headings);
	route.toc?.items.push(
		...headings.map((heading) => ({ ...heading, children: [] }))
	);
});
