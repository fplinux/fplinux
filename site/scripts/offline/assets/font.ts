import { compress } from "wawoff2";

export const compressFont = async (input: Uint8Array): Promise<Uint8Array> =>
	await compress(input);
