import assert from "node:assert/strict";
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
	const page = parseFragment(`
		<main>
			<p>Actual page content stays searchable.</p>
			<nav><a>Next page navigation</a></nav>
			<footer><a>Previous page navigation</a></footer>
			<div class="callout fp-source">Source note with false hit</div>
			<div data-pagefind-ignore>Ignored page furniture</div>
			<script>Script false hit</script>
			<style>.hidden { content: "style false hit"; }</style>
			<template><p>Inert template false hit</p></template>
		</main>
	`);

	assert.equal(indexText(page), "Actual page content stays searchable.");
});
