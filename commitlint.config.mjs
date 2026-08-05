// SPDX-License-Identifier: GPL-2.0-only
// The pinned toolchain installs the Node quality tools outside the source
// tree, so resolve the shared preset from there when the local lookup fails.
const TOOLCHAIN = "/opt/quality/node-tools/node_modules";

let conventional;
try {
  conventional = (await import("@commitlint/config-conventional")).default;
} catch (error) {
  if (error == null || error.code !== "ERR_MODULE_NOT_FOUND") {
    throw error;
  }
  const preset = (
    await import(`${TOOLCHAIN}/@commitlint/config-conventional/lib/index.js`)
  ).default;
  const parserPath = `${TOOLCHAIN}/conventional-changelog-conventionalcommits/src/index.js`;
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

export default conventional;
