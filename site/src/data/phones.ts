import { readFileSync } from "node:fs";
import path from "node:path";
import { z } from "astro/zod";
import { parse } from "smol-toml";

export const repositoryRoot = path.resolve(process.cwd(), "..");

const identifier = z.string().regex(/^[a-z0-9][a-z0-9._-]*$/u);
const pageSchema = z.array(
	z.object({
		id: identifier,
		render: z.string().optional(),
		renderScale: z.number().int().positive().optional(),
		slug: z.string().startsWith("phones/"),
	})
);
const targetSchema = z.object({
	adapter: z.object({ boot_instructions: z.string().min(1) }),
	display: z.object({ panel: z.string().min(1) }),
	identity: z.object({
		brand: z.string().min(1),
		hardware_codes: z.array(z.string()),
		product: z.string().min(1),
	}),
	microsd: z.object({}).optional(),
	platform: identifier,
});
const platformSchema = z.object({
	identity: z.object({
		aliases: z.array(z.string()),
		soc: z.string().min(1),
		vendor: z.string().min(1),
	}),
});
const panelSchema = z.object({
	height: z.number().int().positive(),
	width: z.number().int().positive(),
});

export interface Phone {
	bootInstructions: string;
	display: { height: number; width: number };
	hardwareCodes: string[];
	id: string;
	name: string;
	platform: { aliases: string[]; id: string; name: string };
	profiles: string[];
	render?: string;
	renderScale?: number;
	slug: string;
}

export const readPhonePages = (root = repositoryRoot) =>
	pageSchema.parse(
		JSON.parse(
			readFileSync(path.join(root, "site/src/data/phones.json"), "utf-8")
		)
	);

export const loadPhones = (root = repositoryRoot) => {
	const sources = new Set<string>();
	const readManifest = (relative: string) => {
		sources.add(relative);
		return parse(readFileSync(path.join(root, relative), "utf-8"));
	};
	const pages = readPhonePages(root);
	if (
		new Set(pages.map((page) => page.id)).size !== pages.length ||
		new Set(pages.map((page) => page.slug)).size !== pages.length
	) {
		throw new Error("Phone pages must have distinct target IDs and routes.");
	}
	const phones: Phone[] = pages.map((page) => {
		const directory = `targets/${page.id}`;
		const target = targetSchema.parse(readManifest(`${directory}/target.toml`));
		const platform = platformSchema.parse(
			readManifest(`platforms/${target.platform}/platform.toml`)
		);
		const panelPath = path.posix.join(directory, target.display.panel);
		const display = panelSchema.parse(readManifest(panelPath));
		const { brand, hardware_codes: hardwareCodes, product } = target.identity;
		const name = `${brand} ${product}${hardwareCodes.length ? ` (${hardwareCodes.join(", ")})` : ""}`;
		const profiles = ["default"];
		if (target.microsd !== undefined) {
			profiles.push(
				target.platform === "sc9820e" ? "microsd-root" : "microsd-uboot"
			);
		}
		return {
			...page,
			bootInstructions: target.adapter.boot_instructions,
			display,
			hardwareCodes,
			name,
			platform: {
				aliases: platform.identity.aliases,
				id: target.platform,
				name: `${platform.identity.vendor} ${platform.identity.soc}`,
			},
			profiles,
		};
	});
	return { phones, sources: [...sources].toSorted() };
};

export const phonePageData = (
	id: string,
	data: Record<string, unknown>,
	phones: Phone[]
): Record<string, unknown> => {
	const index = phones.findIndex((phone) => phone.slug === id);
	if (index === -1) {
		return data;
	}
	const phone = phones[index];
	return {
		...data,
		description: `Boot key, controls, storage, power and limitations of FPLinux on the ${phone.name}.`,
		phone: phone.id,
		sidebar: { order: index + 1 },
		source: [`targets/${phone.id}`],
		title: phone.name,
	};
};

export const currentPhone = (id: string): Phone => {
	const phone = loadPhones().phones.find((entry) => entry.id === id);
	if (!phone) {
		throw new Error(`Unknown phone: ${id}`);
	}
	return phone;
};
