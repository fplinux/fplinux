# SPDX-License-Identifier: GPL-2.0-only
"""Persistent boot and flashing for UMS9117 targets.

The modules here implement, test and demonstrate the flash feature the
UMS9117 library documents but does not ship: a host-driven installer
that installs an FPLinux boot chain into the phone's NAND boot
regions, with a mandatory backup and a one-command restore.

* :mod:`.frames`, :mod:`.dhtb`, :mod:`.nandsim`, :mod:`.client` are
  the wire protocol, boot-image container, device simulator and host
  client.
* :mod:`.install` is the installer flow (discover, backup, install,
  verify, restore) plus the CLI dry run.
* :mod:`.selftest` is a dependency-free self-check of everything
  above, including the end-to-end install.

Two plain files complete the set: ``flash_stage_sketch.c``, the flash
command layer an on-hardware stage would run (its reply behaviour is
pinned step by step by ``selftest.py``), and ``check_sketch.py``, a
small auditor that cross-checks the sketch against the t117_fdl
headers of a spreadtrum_flash checkout.
"""
