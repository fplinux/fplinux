// SPDX-License-Identifier: GPL-2.0-only
// The pinned container image installs the Node quality tools outside the source
// tree, so resolve the shared preset from there when the local lookup fails.
const NODE_TOOLS = "/opt/quality/node-tools/node_modules";

let conventional;
try {
  conventional = (await import("@commitlint/config-conventional")).default;
} catch (error) {
  if (error == null || error.code !== "ERR_MODULE_NOT_FOUND") {
    throw error;
  }
  const preset = (
    await import(`${NODE_TOOLS}/@commitlint/config-conventional/lib/index.js`)
  ).default;
  const parserPath = `${NODE_TOOLS}/conventional-changelog-conventionalcommits/src/index.js`;
  const parserFactory = (await import(parserPath)).default;
  conventional = {
    ...preset,
    parserPreset: {
      name: "conventional-changelog-conventionalcommits",
      path: parserPath,
      parserOpts: parserFactory().parser,
    },
  };
}

const SCOPES = [
  "bootstrap",
  "build",
  "cli",
  "console",
  "deps",
  "input",
  "nokia-ta1618",
  "quality",
  "release",
  "repo",
  "rootfs",
  "ums9117",
];

export default {
  ...conventional,
  rules: {
    ...conventional.rules,
    "scope-empty": [2, "never"],
    "scope-enum": [2, "always", SCOPES],
  },
};
