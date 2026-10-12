import path from "node:path";
import { docsLoader } from "@astrojs/starlight/loaders";
import type { Loader, LoaderContext } from "astro/loaders";
import { loadPhones, phonePageData, repositoryRoot } from "./phones.ts";

export const phoneDocsLoader = (): Loader => ({
	load: async (context) => {
		const docs = docsLoader();
		let catalog = loadPhones();
		const wrappedContext: LoaderContext = {
			...context,
			generateDigest: (contents) =>
				context.generateDigest({ catalog: catalog.phones, contents }),
			parseData: async ({ data, ...entry }) =>
				await context.parseData({
					...entry,
					data: { ...data, ...phonePageData(entry.id, data, catalog.phones) },
				}),
		};
		await docs.load(wrappedContext);
		if (!context.watcher) {
			return;
		}
		const externalSources = () =>
			new Set(
				["site/src/data/phones.json", ...catalog.sources].map((source) =>
					path.join(repositoryRoot, source)
				)
			);
		context.watcher.add([...externalSources()]);
		const reload = async (changedPath: string) => {
			if (!externalSources().has(changedPath)) {
				return;
			}
			try {
				catalog = loadPhones();
				context.watcher?.add([...externalSources()]);
				await docs.load({ ...wrappedContext, watcher: undefined });
				context.logger.info("Reloaded phone manifests");
			} catch (error) {
				context.logger.error(String(error));
			}
		};
		context.watcher.on("change", (changedPath) => {
			void reload(changedPath);
		});
	},
	name: "phone-docs-loader",
});
