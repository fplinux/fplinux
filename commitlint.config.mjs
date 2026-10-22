// SPDX-License-Identifier: GPL-2.0-only
// The pinned build environment installs the Node quality tools outside the source
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
  "apidemo",
  "audio",
  "bluetooth",
  "bootstrap",
  "bq-2830l-tank-connect-max",
  "build",
  "cache",
  "calculator",
  "cli",
  "clocks",
  "console",
  "deps",
  "device-data",
  "graphics",
  "host",
  "inoi-240-modern-4g",
  "inoi-244-modern-4g",
  "input",
  "launcher",
  "maxvi-k15n-4g",
  "media",
  "nokia-110-4g",
  "nokia-ta1618",
  "notes",
  "quality",
  "release",
  "repo",
  "rootfs",
  "runtime",
  "sc9820e",
  "session",
  "settings",
  "site",
  "snake",
  "storage",
  "terminal",
  "ui",
  "ums9117",
  "uniwa-v1000-4g",
  "wifi",
  "xiaomi-qin-1s-plus",
];

export default {
  ...conventional,
  rules: {
    ...conventional.rules,
    "body-empty": [2, "never"],
    "body-leading-blank": [2, "always"],
    "scope-empty": [2, "never"],
    "scope-enum": [2, "always", SCOPES],
  },
};
