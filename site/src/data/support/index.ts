import { readFileSync } from "node:fs";
import path from "node:path";
import { z } from "astro/zod";
import { loadPhones, repositoryRoot } from "../phones.ts";

const stateSchema = z.object({
	hardware: z.enum(["present", "absent", "unknown"]).optional(),
	note: z.string(),
	status: z.enum(["verified", "partial", "untested", "unsupported"]),
	stock: z.enum(["available", "unavailable", "unknown"]).optional(),
	stockNote: z.string().optional(),
});
const implementationSchema = z.enum(["implemented", "unimplemented"]);

export type SupportStatus = z.infer<typeof stateSchema>["status"];
export type ImplementationStatus = z.infer<typeof implementationSchema>;

const catalogSchema = z.object({
	features: z.array(
		z.object({
			group: z.string(),
			guide: z.string(),
			id: z.string(),
			kind: z.enum(["application", "hardware", "system"]),
			label: z.string(),
			scope: z.enum(["both", "detail", "overview"]),
			stock: z.boolean().optional(),
		})
	),
	groups: z.array(z.string()),
});

export const loadSupport = (root = repositoryRoot) => {
	const directory = path.join(root, "site/src/data/support");
	const catalog = catalogSchema.parse(
		JSON.parse(readFileSync(path.join(directory, "catalog.json"), "utf-8"))
	);
	const featureIds = new Set(catalog.features.map((feature) => feature.id));
	if (featureIds.size !== catalog.features.length) {
		throw new Error("Support catalog must have distinct feature IDs.");
	}
	const { phones: documentedPhones } = loadPhones(root);
	const platforms = [
		...new Map(
			documentedPhones.map(({ platform }) => [
				platform.id,
				{ id: platform.id, name: platform.name },
			])
		).values(),
	];
	const platformStates = platforms.map(({ id }) => {
		const implementations = z
			.record(z.string(), implementationSchema)
			.parse(
				JSON.parse(
					readFileSync(path.join(directory, "platforms", `${id}.json`), "utf-8")
				)
			);
		for (const feature of Object.keys(implementations)) {
			if (!featureIds.has(feature)) {
				throw new Error(`Unknown ${id} implementation feature: ${feature}`);
			}
		}
		for (const feature of featureIds) {
			if (implementations[feature] === undefined) {
				throw new Error(`Missing ${id} implementation for ${feature}`);
			}
		}
		return { id, implementations };
	});
	const phoneStates = documentedPhones.map(({ id }) => {
		const states = z
			.record(z.string(), stateSchema)
			.parse(
				JSON.parse(
					readFileSync(path.join(directory, "phones", `${id}.json`), "utf-8")
				)
			);
		for (const feature of Object.keys(states)) {
			if (!featureIds.has(feature)) {
				throw new Error(`Unknown ${id} support feature: ${feature}`);
			}
		}
		return { id, states };
	});
	const features = catalog.features.map((feature) => {
		const implementations: Record<
			string,
			z.infer<typeof implementationSchema>
		> = {};
		for (const { id, implementations: states } of platformStates) {
			implementations[id] = states[feature.id];
		}
		const phones: Record<string, z.infer<typeof stateSchema>> = {};
		for (const { id, states } of phoneStates) {
			const state = states[feature.id];
			if (state === undefined) {
				throw new Error(`Missing ${id} support for ${feature.id}`);
			}
			if (state.stockNote !== undefined && state.stock === undefined) {
				throw new Error(
					`Stock note requires a stock fact for ${id}: ${feature.id}`
				);
			}
			if (feature.stock === true && state.stock === undefined) {
				throw new Error(`Missing ${id} stock for ${feature.id}`);
			}
			if (feature.stock !== true && state.stock !== undefined) {
				throw new Error(
					`Stock comparison does not apply to ${feature.id}: ${id}`
				);
			}
			phones[id] = state;
		}
		return { ...feature, implementations, phones };
	});
	return { features, groups: catalog.groups, platforms };
};
