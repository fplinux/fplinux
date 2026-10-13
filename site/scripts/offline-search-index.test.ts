import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { parseFragment } from "parse5";
import { indexText } from "./offline-search-index.ts";

void test("search text keeps titles and formatted Unicode prose", () => {
	const title = parseFragment("<h1>Guide to <em>café</em></h1>");
	const body = parseFragment(
		"<main><p>Read the <strong>formatted guide</strong> in naïve prose.</p></main>"
	);

	assert.equal(indexText(title), "Guide to café");
	assert.equal(indexText(body), "Read the formatted guide in naïve prose.");
});

void test("search text excludes navigation, source chrome, ignored elements, and templates", () => {
	const page = parseFragment(
		readFileSync(
			new URL("fixtures/offline-search-page.html.fixture", import.meta.url),
			"utf-8"
		)
	);

	assert.equal(indexText(page), "Actual page content stays searchable.");
});
