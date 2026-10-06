// SPDX-License-Identifier: GPL-2.0-only
// Retain the source text around parser-owned property and value spans.

import { readFileSync } from "node:fs";
import { createRequire } from "node:module";

const localRequire = createRequire(import.meta.url);
let parser;
try {
  parser = localRequire("jsonc-parser");
} catch (error) {
  if (error.code !== "MODULE_NOT_FOUND") throw error;
  parser = createRequire("/opt/quality/node-tools/package.json")(
    "jsonc-parser",
  );
}

const { parseTree, createScanner, SyntaxKind, printParseErrorCode } = parser;
const relative = process.argv[2];
const text = readFileSync(0, "utf8");
const errors = [];
const tree = parseTree(text, errors, {
  disallowComments: !relative.endsWith(".jsonc"),
  allowTrailingComma: relative.endsWith(".jsonc"),
});
if (errors.length || !tree) {
  const detail = errors[0];
  throw new Error(
    detail
      ? `${printParseErrorCode(detail.error)} at offset ${detail.offset}`
      : "empty JSON document",
  );
}

const descriptionFields = [
  "$schema",
  "name",
  "title",
  "version",
  "private",
  "description",
  "type",
  "license",
  "author",
  "homepage",
  "repository",
  "bugs",
  "engines",
  "packageManager",
  "scripts",
  "dependencies",
  "devDependencies",
  "peerDependencies",
  "optionalDependencies",
  "hashes",
  "settings",
];
const prettierFields = [
  "$schema",
  "plugins",
  "printWidth",
  "tabWidth",
  "useTabs",
  "semi",
  "singleQuote",
  "quoteProps",
  "jsxSingleQuote",
  "trailingComma",
  "bracketSpacing",
  "bracketSameLine",
  "arrowParens",
  "proseWrap",
  "htmlWhitespaceSensitivity",
  "endOfLine",
  "embeddedLanguageFormatting",
  "singleAttributePerLine",
  "overrides",
];
const independentMaps = new Set([
  "dependencies",
  "devDependencies",
  "peerDependencies",
  "optionalDependencies",
  "hashes",
  "settings",
]);

function fieldOrder(path) {
  if (relative === ".vscode/settings.json") return "lexical";
  if (path.length && independentMaps.has(path.at(-1))) return "lexical";
  if (relative === ".markdownlint-cli2.jsonc") {
    if (!path.length)
      return [
        "$schema",
        "globs",
        "ignores",
        "customRules",
        "config",
        "fix",
        "outputFormatters",
      ];
    if (path[0] === "config") return "lexical";
  }
  if (relative === ".prettierrc.json") {
    if (!path.length || (path[0] === "overrides" && path.at(-1) === "options"))
      return prettierFields;
    if (path[0] === "overrides" && path.length === 2)
      return ["files", "excludeFiles", "options"];
  }
  return path.length ? [] : descriptionFields;
}

function splitGap(start, end) {
  const gap = text.slice(start, end);
  const scanner = createScanner(gap);
  let comma = -1;
  let newline = -1;
  for (
    let token = scanner.scan();
    token !== SyntaxKind.EOF;
    token = scanner.scan()
  ) {
    if (token === SyntaxKind.CommaToken) comma = scanner.getTokenOffset();
    if (token === SyntaxKind.LineBreakTrivia && newline < 0)
      newline = scanner.getTokenOffset();
  }
  const withoutComma =
    comma < 0 ? gap : gap.slice(0, comma) + gap.slice(comma + 1);
  if (newline < 0)
    return { trailing: withoutComma, leading: "", comma: comma >= 0 };
  const split = newline - (comma >= 0 && comma < newline ? 1 : 0);
  return {
    trailing: withoutComma.slice(0, split),
    leading: withoutComma.slice(split),
    comma: comma >= 0,
  };
}

function render(node, path) {
  if (node.type === "array") {
    let position = node.offset;
    let result = "";
    for (const item of node.children ?? []) {
      result +=
        text.slice(position, item.offset) + render(item, [...path, "*"]);
      position = item.offset + item.length;
    }
    return result + text.slice(position, node.offset + node.length);
  }
  if (node.type !== "object" || !node.children?.length)
    return text.slice(node.offset, node.offset + node.length);

  const properties = node.children;
  const names = properties.map((property) => property.children[0].value);
  if (new Set(names).size !== names.length)
    throw new Error("duplicate JSON object key");
  const order = fieldOrder(path);
  const permutation = properties.map((_, index) => index);
  if (order === "lexical") {
    permutation.sort((left, right) =>
      names[left] < names[right] ? -1 : names[left] > names[right] ? 1 : 0,
    );
  } else if (order.length) {
    const ranks = new Map(order.map((name, index) => [name, index]));
    permutation.sort(
      (left, right) =>
        (ranks.get(names[left]) ?? order.length) -
        (ranks.get(names[right]) ?? order.length),
    );
  }

  const blocks = [];
  let leading = text.slice(node.offset + 1, properties[0].offset);
  let suffix = "";
  let trailingComma = false;
  for (let index = 0; index < properties.length; index++) {
    const property = properties[index];
    const value = property.children[1];
    const end = property.offset + property.length;
    const next = properties[index + 1]?.offset ?? node.offset + node.length - 1;
    const gap = splitGap(end, next);
    const body =
      text.slice(property.offset, value.offset) +
      render(value, [...path, names[index]]);
    blocks.push({ leading, body, trailing: gap.trailing });
    leading = gap.leading;
    if (index === properties.length - 1) {
      suffix = gap.leading;
      trailingComma = gap.comma;
    }
  }
  return (
    "{" +
    permutation
      .map((index, position) => {
        const block = blocks[index];
        const comma =
          position < properties.length - 1 || trailingComma ? "," : "";
        return block.leading + block.body + comma + block.trailing;
      })
      .join("") +
    suffix +
    "}"
  );
}

process.stdout.write(
  text.slice(0, tree.offset) +
    render(tree, []) +
    text.slice(tree.offset + tree.length),
);
