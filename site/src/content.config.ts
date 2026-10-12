import { defineCollection } from "astro:content";
import { z } from "astro/zod";
import { docsSchema } from "@astrojs/starlight/schema";
import { phoneDocsLoader } from "./data/phone-docs-loader.ts";

export const collections = {
	docs: defineCollection({
		loader: phoneDocsLoader(),
		schema: docsSchema({
			extend: z.object({
				phone: z.string().optional(),
				// Repository directories that hold the code this page describes.
				source: z.array(z.string()).optional(),
			}),
		}),
	}),
};
