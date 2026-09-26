#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Replay a test-owned fphelper_t117 scenario for one exact stock image.

The test copies this program into a host-tool directory as fphelper_t117 and
writes scenario.json beside it. The scenario names the SHA-256 of the only
image the tool accepts and, for each command line after the image path, the
text to print, the files to write into the working directory and the exit
status. Any other image or command fails. The fake cannot show that the real
tool finds these tables in a real stock image.
"""

import hashlib
import json
import pathlib
import sys


def main() -> None:
    scenario = json.loads(pathlib.Path(__file__).with_name("scenario.json").read_text())
    image, *command = sys.argv[1:]
    if hashlib.sha256(pathlib.Path(image).read_bytes()).hexdigest() != scenario["image_sha256"]:
        sys.stderr.write("fake fphelper_t117: unexpected stock image\n")
        raise SystemExit(1)
    response = scenario["commands"].get(" ".join(command))
    if response is None:
        sys.stderr.write(f"fake fphelper_t117: unexpected command: {' '.join(command)}\n")
        raise SystemExit(1)
    for name, contents in response.get("files", {}).items():
        pathlib.Path(name).write_bytes(bytes.fromhex(contents))
    sys.stdout.write(response.get("stdout", ""))
    sys.stderr.write(response.get("stderr", ""))
    raise SystemExit(response.get("exit_status", 0))


if __name__ == "__main__":
    main()
