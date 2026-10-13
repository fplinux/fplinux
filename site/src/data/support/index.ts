import { readFileSync } from "node:fs";
import path from "node:path";
import { z } from "astro/zod";
import { readPhonePages, repositoryRoot } from "../phones.ts";

const stateSchema = z.object({
	hardware: z.string().optional(),
	note: z.string(),
	status: z.string(),
});
const catalogSchema = z.object({
	features: z.array(
		z.object({
			group: z.string(),
			guide: z.string(),
			id: z.string(),
			implementation: z.string(),
			kind: z.enum(["application", "hardware", "system"]),
			label: z.string(),
			scope: z.enum(["both", "detail", "overview"]),
			verification: z.string(),
		})
	),
	groups: z.array(z.string()),
});

type PhoneState = z.infer<typeof stateSchema>;

export const loadSupport = (root = repositoryRoot) => {
	const directory = path.join(root, "site/src/data/support");
	const catalog = catalogSchema.parse(
		JSON.parse(readFileSync(path.join(directory, "catalog.json"), "utf-8"))
	);
	const featureIds = new Set(catalog.features.map((feature) => feature.id));
	if (featureIds.size !== catalog.features.length) {
		throw new Error("Support catalog must have distinct feature IDs.");
	}
	const states = readPhonePages(root).map(({ id }) => {
		const phone = z
			.record(z.string(), stateSchema)
			.parse(
				JSON.parse(
					readFileSync(path.join(directory, "phones", `${id}.json`), "utf-8")
				)
			);
		for (const feature of Object.keys(phone)) {
			if (!featureIds.has(feature)) {
				throw new Error(`Unknown ${id} support feature: ${feature}`);
			}
		}
		return { id, phone };
	});
	const features = catalog.features.map((feature) => {
		const phones: Record<string, PhoneState> = {};
		for (const { id, phone } of states) {
			const state = phone[feature.id];
			if (state === undefined) {
				throw new Error(`Missing ${id} support for ${feature.id}`);
			}
			phones[id] = state;
		}
		return { ...feature, phones };
	});
	return { ...catalog, features };
};
