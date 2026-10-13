import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { format, resolveConfig } from "prettier";

const configPath = fileURLToPath(
	new URL("../config/prettier.config.mjs", import.meta.url)
);
const expected = await readFile(
	new URL("fixtures/mdx-format.expected.mdx.fixture", import.meta.url),
	"utf-8"
);

const formatDocument = async (
	source: string,
	extension = "mdx"
): Promise<string> => {
	const filepath = fileURLToPath(
		new URL(
			`../src/content/docs/formatting-example.${extension}`,
			import.meta.url
		)
	);
	const options = await resolveConfig(filepath, { config: configPath });
	assert.ok(options);
	return await format(source, { ...options, filepath });
};

await test("MDX formatting joins prose and compacts tables while preserving document structure", async () => {
	const input = await readFile(
		new URL("fixtures/mdx-format.input.mdx.fixture", import.meta.url),
		"utf-8"
	);
	assert.equal(await formatDocument(input), expected);
	assert.equal(await formatDocument(expected), expected);
});

await test("extending one table cell changes only its own row", async () => {
	const longer =
		"a substantially longer command that does not widen any other row";
	const output = await formatDocument(
		expected.replace("short command", longer)
	);
	const before = expected.split("\n");
	const after = output.split("\n");
	assert.equal(after.length, before.length);
	assert.deepEqual(
		after.filter((line, index) => line !== before[index]),
		[`| ${longer} | \`a  \\| b\` and **text** | Ready |`]
	);
});

await test("ordinary Markdown retains its existing prose and table formatting", async () => {
	const input = await readFile(
		new URL("fixtures/markdown-format.input.md.fixture", import.meta.url),
		"utf-8"
	);
	const markdownExpected = await readFile(
		new URL("fixtures/markdown-format.expected.md.fixture", import.meta.url),
		"utf-8"
	);
	assert.equal(await formatDocument(input, "md"), markdownExpected);
});
