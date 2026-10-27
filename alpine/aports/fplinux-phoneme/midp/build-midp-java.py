#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the fplinux-phoneme system classes and the generated MIDP C sources.

Inputs are the patched phoneME tree (--src), the CLDC classes.zip and tools
built by the CLDC make, a JDK 8 and a host preverify. The Java selection is
java-sources.toml next to this script; the LCDUI look and feel comes from
--lf (default: lf/ next to this script). Output (--out):

  classes.zip        preverified CLDC + MIDP + LF + --extra-java classes
  c/                 generated C sources and headers for the MIDP natives:
                     NativesTable.cpp, LocalizedStringsBase.c,
                     midp_constants_data.{h,c}, midp_property_callouts.h,
                     properties_static_data.c, perm_number.h
  build.log          every command run and its result
  work/              intermediate files (kept for inspection)

The script uses the Python standard library only and never touches the
network. Output is deterministic for identical inputs: file lists are sorted
and classes.zip has sorted entries with a fixed timestamp.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tomllib
import zipfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Iterable
    from typing import TextIO

HERE = Path(__file__).resolve().parent
ZIP_DATE = (1980, 1, 1, 0, 0, 0)


class BuildError(Exception):
    """A build step failed; the message says which."""


class Build:
    """One run of the MIDP Java build into an output directory."""

    def __init__(self, args: argparse.Namespace) -> None:
        """Take the resolved command-line inputs and load the selection."""
        self.src: Path = args.src.resolve()
        self.cldc_classes: Path = args.cldc_classes.resolve()
        self.jdk: Path = args.jdk.resolve()
        self.preverify: Path = args.preverify.resolve()
        self.natives_table_gen: Path = args.natives_table_gen.resolve()
        self.extra_java: list[Path] = [p.resolve() for p in args.extra_java]
        self.lf: Path = args.lf.resolve()
        self.out: Path = args.out.resolve()
        self.selection_path: Path = args.selection.resolve()
        with self.selection_path.open("rb") as f:
            self.sel: dict[str, Any] = tomllib.load(f)
        self.work = self.out / "work"
        self.c_out = self.out / "c"
        self.gen_java = self.work / "gen-java"
        self.log: TextIO | None = None

    # ---- helpers -------------------------------------------------------

    def say(self, text: str) -> None:
        """Print a progress line and append it to build.log."""
        print(text, flush=True)
        if self.log is not None:
            self.log.write(text + "\n")
            self.log.flush()

    def run(self, *cmd: str | Path) -> str:
        """Run a command, log its output and fail the build on error."""
        args = [str(c) for c in cmd]
        self.say("+ " + " ".join(args))
        res = subprocess.run(
            args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False
        )
        if res.stdout and self.log is not None:
            self.log.write(res.stdout)
            self.log.flush()
        if res.returncode != 0:
            sys.stdout.write(res.stdout[-8000:])
            msg = f"command failed with status {res.returncode}: {args[0]}"
            raise BuildError(msg)
        return res.stdout

    def tool(self, name: str) -> Path:
        """Return a JDK tool path."""
        return self.jdk / "bin" / name

    def javac(
        self,
        sources: Iterable[Path],
        dest: Path,
        *,
        target13: bool = False,
    ) -> None:
        """Compile sources into dest; target13 builds CLDC class files."""
        dest.mkdir(parents=True, exist_ok=True)
        listing = dest.parent / (dest.name + "-sources.txt")
        listing.write_text("".join(f"{s}\n" for s in sources))
        flags: list[str | Path] = ["-nowarn", "-encoding", "UTF-8", "-d", dest]
        if target13:
            flags += [
                "-source",
                "1.3",
                "-target",
                "1.3",
                "-Xlint:-options",
                "-g:source,lines",
                "-bootclasspath",
                self.cldc_classes,
            ]
        self.run(self.tool("javac"), "-J-Duser.language=en", *flags, f"@{listing}")

    def transform(self, *args: str | Path) -> None:
        """Run the Configurator's CodeTransformer with the given steps."""
        self.run(
            self.tool("java"),
            "-cp",
            self.work / "tools/configurator",
            "com.sun.xml.transform.CodeTransformer",
            *args,
        )

    @staticmethod
    def java_files(root: Path) -> list[Path]:
        """Return the *.java files below root, sorted."""
        return sorted(p for p in root.rglob("*.java") if p.is_file())

    def fresh_dir(self, path: Path) -> None:
        """Create an empty directory; only paths below --out are removed."""
        if self.out not in path.parents:
            msg = f"refusing to clear {path}: not below {self.out}"
            raise BuildError(msg)
        if path.exists():
            shutil.rmtree(path)
        path.mkdir(parents=True)

    def check_inputs(self) -> None:
        """Fail early when an input path is missing."""
        for label, path in (
            ("--src", self.src / "midp/src"),
            ("--cldc-classes", self.cldc_classes),
            ("--jdk", self.tool("javac")),
            ("--preverify", self.preverify),
            ("--natives-table-gen", self.natives_table_gen),
            ("--lf", self.lf),
        ):
            if not path.exists():
                msg = f"{label}: {path} does not exist"
                raise BuildError(msg)
        for d in self.extra_java:
            if not d.is_dir():
                msg = f"--extra-java: {d} is not a directory"
                raise BuildError(msg)

    # ---- steps ---------------------------------------------------------

    def build_tools(self) -> None:
        """Compile the Configurator, the l10n generator and Jpp."""
        tools = self.work / "tools"
        self.javac(
            self.java_files(self.src / "tools/configurator/classes"), tools / "configurator"
        )
        self.javac(self.java_files(self.src / "midp/src/tool/l10ngen/classes"), tools / "l10ngen")
        self.javac([self.src / "tools/jpp/Jpp.java"], tools / "jpp")

    def configure(self) -> None:
        """Run the MIDP Configurator and the l10n generator."""
        cfg = self.sel["configuration"]
        inputs = [self.src / p for p in cfg["upstream"]]
        inputs += [HERE / p for p in cfg.get("project", [])]
        for p in inputs:
            if not p.is_file():
                msg = f"configuration input missing: {p}"
                raise BuildError(msg)
        xml = self.work / "xml"
        xml.mkdir(parents=True)
        xsl = self.src / "tools/configurator/xsl"
        prev = self.src / "midp/src/configuration/configuration_xml/share/empty.xml"
        args: list[str | Path] = []
        for stage in range(1, 5):
            dst = xml / f"merged_pass{stage}.xml"
            args += ["-xml", prev, "-xsl", xsl / f"share/merge_pass{stage}.xsl"]
            if stage == 1:
                args += ["-params", "filesList", " ".join(str(p) for p in inputs)]
            args += ["-out", dst]
            prev = dst
        packages = xml / "packages.txt"
        args += ["-xml", prev, "-xsl", xsl / "cldc/listPackages.xsl", "-out", packages]
        self.transform(*args)
        merged = xml / "merged.xml"
        shutil.copyfile(prev, merged)

        gen_java = self.gen_java
        args = []
        for name in sorted(set(packages.read_text().split())):
            if name.startswith("com.sun.midp.l10n."):
                continue  # the l10n generator writes these classes
            target = gen_java / (name.replace(".", "/") + ".java")
            target.parent.mkdir(parents=True, exist_ok=True)
            args += [
                "-xml",
                merged,
                "-xsl",
                xsl / "cldc/constantsJava.xsl",
                "-params",
                "packageName",
                name,
                "-out",
                target,
            ]
        perm = gen_java / "com/sun/midp/security/PermissionsStrings.java"
        perm.parent.mkdir(parents=True, exist_ok=True)
        args += ["-xml", merged, "-xsl", xsl / "cldc/permissions.xsl"]
        args += ["-params", "output", "java", "-out", perm]
        c = self.c_out
        native_steps: list[tuple[str, list[str], str]] = [
            ("constantsNative.xsl", [], "midp_constants_data.h"),
            ("stringConstantsNative.xsl", [], "midp_constants_data.c"),
            ("propertyCallouts.xsl", [], "midp_property_callouts.h"),
            ("propertiesNative.xsl", ["arrayNamePrefix", "midp"], "properties_static_data.c"),
            ("permissions.xsl", ["output", "native"], "perm_number.h"),
        ]
        for sheet, params, out_name in native_steps:
            args += ["-xml", merged, "-xsl", xsl / "cldc" / sheet]
            if params:
                args += ["-params", *params]
            args += ["-out", c / out_name]
        self.transform(*args)

        l10n = self.work / "gen-l10n"
        l10n.mkdir()
        self.run(
            self.tool("java"),
            "-cp",
            self.work / "tools/l10ngen",
            "com.sun.midp.l10n.generator.LocalizedStringsGenerator",
            "-xml",
            merged,
            "-out",
            l10n,
        )
        for p in sorted(l10n.iterdir()):
            if p.suffix == ".java":
                dst = gen_java / "com/sun/midp/l10n" / p.name
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(p, dst)
            elif p.suffix == ".c":
                shutil.copyfile(p, c / p.name)

    def collect_sources(self) -> list[Path]:
        """Map package-relative path to source file; later layers override."""
        jsel = self.sel["java"]
        exclude_dirs = set(jsel.get("exclude_dirs", []))
        exclude_names = set(jsel.get("exclude_names", []))
        defines = [f"-D{d}" for d in jsel.get("jpp_defines", [])]
        jpp_out = self.work / "jpp"
        sources: dict[str, tuple[Path, str]] = {}

        def add(rel: str, path: Path, layer: str) -> None:
            old = sources.get(rel)
            if old is not None and old[1] == layer and old[0] != path:
                msg = f"duplicate class source {rel}: {old[0]} and {path}"
                raise BuildError(msg)
            sources[rel] = (path, layer)

        jpp_jobs: list[tuple[Path, Path]] = []
        for r in jsel["roots"]:
            root = self.src / r
            if not root.is_dir():
                msg = f"java root missing: {root}"
                raise BuildError(msg)
            for p in sorted(root.rglob("*")):
                if p.suffix not in {".java", ".jpp"} or not p.is_file():
                    continue
                rel = p.relative_to(root)
                if exclude_dirs.intersection(rel.parts[:-1]) or p.name in exclude_names:
                    continue
                if p.suffix == ".jpp":
                    target = jpp_out / rel.with_suffix(".java")
                    jpp_jobs.append((p, target))
                    add(rel.with_suffix(".java").as_posix(), target, "upstream")
                else:
                    add(rel.as_posix(), p, "upstream")
        narrow_ex = set(jsel.get("narrow_exclude_names", []))
        for r in jsel.get("narrow_roots", []):
            root = self.src / r
            if not root.is_dir():
                msg = f"java narrow root missing: {root}"
                raise BuildError(msg)
            for p in self.java_files(root):
                if p.name not in narrow_ex:
                    add(package_path(p), p, "upstream")
        for f in jsel.get("files", []):
            p = self.src / f
            if not p.is_file():
                msg = f"java file missing: {p}"
                raise BuildError(msg)
            add(package_path(p), p, "upstream")

        for src, target in jpp_jobs:
            target.parent.mkdir(parents=True, exist_ok=True)
            jpp = self.work / "tools/jpp"
            self.run(self.tool("java"), "-cp", jpp, "Jpp", *defines, src, "-o", target)

        for p in self.java_files(self.gen_java):
            add(p.relative_to(self.gen_java).as_posix(), p, "generated")

        # The LF directory and the --extra-java directories may replace an
        # upstream class (e.g. the LF owns GameCanvasLFImpl); each override
        # is reported in the log.
        layers = [("lf", self.lf)] + [("extra", d) for d in self.extra_java]
        for layer, root in layers:
            for p in self.java_files(root):
                rel = p.relative_to(root).as_posix()
                old = sources.get(rel)
                if old is not None and old[1] != layer:
                    self.say(f"override: {rel} by {layer} {p} (was {old[0]})")
                add(rel, p, layer)
        self.say(f"java sources: {len(sources)}")
        counts: dict[str, int] = {}
        for _, layer in sources.values():
            counts[layer] = counts.get(layer, 0) + 1
        self.say("java sources by layer: " + ", ".join(f"{k}={counts[k]}" for k in sorted(counts)))
        return [sources[k][0] for k in sorted(sources)]

    def compile_and_preverify(self, sources: list[Path]) -> Path:
        """Compile, preverify and package the system classes."""
        classes = self.work / "classes"
        self.javac(sources, classes, target13=True)
        verified = self.work / "preverified"
        verified.mkdir()
        self.run(self.preverify, "-classpath", self.cldc_classes, "-d", verified, classes)
        entries: dict[str, bytes] = {}
        with zipfile.ZipFile(self.cldc_classes) as z:
            for info in z.infolist():
                if not info.is_dir():
                    entries[info.filename] = z.read(info)
        cldc_count = len(entries)
        for p in sorted(verified.rglob("*")):
            if p.is_file():
                rel = p.relative_to(verified).as_posix()
                if rel in entries:
                    msg = f"class clashes with CLDC classes.zip: {rel}"
                    raise BuildError(msg)
                entries[rel] = p.read_bytes()
        # Resources next to --extra-java sources (non-Java files) go in too.
        for root in self.extra_java:
            for p in sorted(root.rglob("*")):
                if p.is_file() and p.suffix != ".java":
                    entries.setdefault(p.relative_to(root).as_posix(), p.read_bytes())
        out = self.out / "classes.zip"
        tmp = self.out / "classes.zip.tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name in sorted(entries):
                info = zipfile.ZipInfo(name, ZIP_DATE)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                z.writestr(info, entries[name])
        tmp.replace(out)
        n_class = sum(1 for n in entries if n.endswith(".class"))
        self.say(
            f"classes.zip: {out.stat().st_size} bytes, {len(entries)} entries, "
            f"{n_class} classes ({cldc_count} from CLDC)"
        )
        return out

    def natives_table(self, classes_zip: Path) -> None:
        """Generate NativesTable.cpp for every native method of classes_zip."""
        table = self.c_out / "NativesTable.cpp"
        self.run(
            self.tool("java"),
            "-jar",
            self.natives_table_gen,
            "-writer",
            "CLDC_HI_Natives",
            "-o",
            table,
            classes_zip,
        )
        # NativesTableGen stamps the current date; drop it for reproducible
        # output.
        lines = table.read_text().splitlines(keepends=True)
        table.write_text("".join(x for x in lines if not x.startswith(" * Generated on ")))

    def main(self) -> None:
        """Run every step; outputs go below --out."""
        self.check_inputs()
        self.out.mkdir(parents=True, exist_ok=True)
        self.fresh_dir(self.work)
        self.fresh_dir(self.c_out)
        with (self.out / "build.log").open("w") as log:
            self.log = log
            self.say(f"selection: {self.selection_path}")
            self.say(f"src: {self.src}")
            self.say(f"lf: {self.lf}")
            for d in self.extra_java:
                self.say(f"extra-java: {d}")
            self.build_tools()
            self.configure()
            sources = self.collect_sources()
            classes_zip = self.compile_and_preverify(sources)
            self.natives_table(classes_zip)
            self.say("generated C: " + " ".join(sorted(p.name for p in self.c_out.iterdir())))
            self.say("OK")
            self.log = None


def package_path(java_file: Path) -> str:
    """Return pkg/dirs/Name.java from the package declaration of a source."""
    pkg = ""
    with java_file.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if s.startswith("package ") and s.endswith(";"):
                pkg = s[len("package ") : -1].strip()
                break
    prefix = pkg.replace(".", "/") + "/" if pkg else ""
    return prefix + java_file.name


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the command line."""
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n", maxsplit=1)[0])
    ap.add_argument("--src", type=Path, required=True, help="patched phoneME tree root")
    ap.add_argument(
        "--cldc-classes", type=Path, required=True, help="CLDC classes.zip from the CLDC build"
    )
    ap.add_argument("--jdk", type=Path, required=True, help="JDK 8 home")
    ap.add_argument("--preverify", type=Path, required=True, help="host preverify binary")
    ap.add_argument(
        "--natives-table-gen",
        type=Path,
        required=True,
        help="NativesTableGen.jar from the CLDC dist/bin",
    )
    ap.add_argument(
        "--extra-java",
        type=Path,
        action="append",
        default=[],
        help="extra Java source root (repeatable)",
    )
    ap.add_argument(
        "--lf",
        type=Path,
        default=HERE / "lf",
        help="LCDUI look-and-feel source root (default: lf)",
    )
    ap.add_argument(
        "--selection",
        type=Path,
        default=HERE / "java-sources.toml",
        help="Java selection file (default: java-sources.toml)",
    )
    ap.add_argument("--out", type=Path, required=True, help="output directory")
    return ap.parse_args(argv)


if __name__ == "__main__":
    os.environ.setdefault("LC_ALL", "C")
    try:
        Build(parse_args(sys.argv[1:])).main()
    except BuildError as e:
        sys.exit(f"build-midp-java: {e}")
