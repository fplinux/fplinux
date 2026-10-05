# SPDX-License-Identifier: GPL-2.0-only
"""Normalize bounded text formats without changing executable sequences."""

from __future__ import annotations

import re
import shlex
from dataclasses import dataclass
from pathlib import PurePosixPath

_DTS_TOKEN = re.compile(
    r"(?P<comment>/\*.*?\*/|//[^\n]*)"
    r'|(?P<string>"(?:\\.|[^"\\])*")'
    r"|(?P<space>\s+)"
    r"|(?P<atom>[A-Za-z0-9_#.,+?@/-]+)"
    r"|(?P<symbol>[{};=<>\[\]():&!~|*%^])",
    re.DOTALL,
)
_PREPROCESSOR = re.compile(
    r"#(?:include|define|undef|if|ifdef|ifndef|elif|else|endif|error|pragma|line)\b"
)
_PROPERTY = re.compile(r"[#a-z][a-z0-9#_,.+?-]*\Z")
_LABEL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
_NODE_NAME = re.compile(r"[A-Za-z0-9_,.+?@-]+\Z")
_LINE_SUFFIX = re.compile(r"[ \t]*(?://[^\n]*|/\*[^\n]*\*/[ \t]*)?\n?\Z")
_CONFIG_VALUE = re.compile(
    r'CONFIG_[A-Z0-9_]+=(?:[ymn]|0[xX][0-9A-Fa-f]+|-?[0-9]+|"(?:\\.|[^"\\])*")[ \t]*\Z'
)


@dataclass(frozen=True)
class _Token:
    start: int
    end: int
    kind: str
    value: str


@dataclass(frozen=True)
class _Entry:
    start: int
    end: int
    name: str | None = None


def _directive_end(text: str, start: int) -> int:
    end = start
    while end < len(text):
        newline = text.find("\n", end)
        if newline < 0:
            return len(text)
        end = newline + 1
        if text[newline - 1 : newline] != "\\":
            break
    return end


def _tokens(text: str) -> list[_Token] | None:
    tokens: list[_Token] = []
    position = 0
    while position < len(text):
        line_start = text.rfind("\n", 0, position) + 1
        first_on_line = not text[line_start:position].strip()
        if first_on_line and (
            _PREPROCESSOR.match(text, position) or text.startswith("/include/", position)
        ):
            end = _directive_end(text, position)
            token = _Token(position, end, "directive", text[position:end])
            tokens.append(token)
            position = end
            continue
        match = _DTS_TOKEN.match(text, position)
        if match is None:
            return None
        kind = match.lastgroup
        if kind is None:
            return None
        token = _Token(position, match.end(), kind, match.group())
        if kind not in {"space", "comment"}:
            tokens.append(token)
        position = match.end()
    return tokens


def _brace_pairs(tokens: list[_Token]) -> dict[int, int] | None:
    stack: list[int] = []
    pairs: dict[int, int] = {}
    for index, token in enumerate(tokens):
        if token.value == "{":
            stack.append(index)
        elif token.value == "}":
            if not stack:
                return None
            pairs[stack.pop()] = index
    return None if stack else pairs


def _property_key(name: str) -> tuple[int, tuple[tuple[int, int, str], ...]]:
    groups = {"device_type": 0, "compatible": 1, "reg": 2, "ranges": 3, "status": 6}
    group = groups.get(name, 5 if "," in name else 4)
    parts = re.split(r"([0-9]+)", name)
    natural = tuple((1, int(part), "") if part.isdigit() else (0, 0, part) for part in parts)
    return group, natural


def _node_header(values: list[str]) -> bool:
    if values == ["/"]:
        return True
    if len(values) == 2 and values[0] == "&":
        return _LABEL.fullmatch(values[1]) is not None
    if len(values) == 4 and values[:2] == ["&", "{"] and values[-1] == "}":
        return values[2].startswith("/")
    position = 0
    while position + 1 < len(values) and values[position + 1] == ":":
        if not _LABEL.fullmatch(values[position]):
            return False
        position += 2
    return position + 1 == len(values) and _NODE_NAME.fullmatch(values[position]) is not None


class _DtsOrder:
    """Sort complete property lines while keeping syntax barriers in place."""

    def __init__(self, text: str, tokens: list[_Token], pairs: dict[int, int]) -> None:
        self.text = text
        self.tokens = tokens
        self.pairs = pairs
        self.edits: list[tuple[int, int, str]] = []

    def _line_span(self, entry: _Entry) -> tuple[int, int] | None:
        start = self.text.rfind("\n", 0, entry.start) + 1
        newline = self.text.find("\n", entry.end)
        end = len(self.text) if newline < 0 else newline + 1
        if self.text[start : entry.start].strip():
            return None
        if not _LINE_SUFFIX.fullmatch(self.text[entry.end : end]):
            return None
        return start, end

    def _sort_run(self, entries: list[_Entry]) -> None:
        if not entries:
            return
        spans = [self._line_span(entry) for entry in entries]
        if any(span is None for span in spans):
            return
        chunks: list[tuple[str, str]] = []
        for entry, span in zip(entries, spans, strict=True):
            if entry.name is None or span is None:
                return
            chunks.append((entry.name, self.text[span[0] : span[1]]))
        ordered = sorted(chunks, key=lambda item: _property_key(item[0]))
        pieces: list[str] = []
        for name, chunk in ordered:
            if name == "status" and pieces:
                pieces.append("\n")
            pieces.append(chunk)
        first, last = spans[0], spans[-1]
        if first is not None and last is not None:
            self.edits.append((first[0], last[1], "".join(pieces)))

    def _leading_start(self, boundary: int, start: int) -> tuple[int, bool]:
        gap = self.text[boundary:start]
        comments = [match for match in _DTS_TOKEN.finditer(gap) if match.lastgroup == "comment"]
        if not comments:
            return start, bool(gap.strip())
        first = comments[-1]
        if gap[first.end() :].count("\n") != 1:
            return start, True
        for preceding in reversed(comments[:-1]):
            if gap[preceding.end() : first.start()].count("\n") != 1:
                break
            first = preceding
        comment_start = boundary + first.start()
        line_start = self.text.rfind("\n", 0, comment_start) + 1
        if line_start < boundary or self.text[line_start:comment_start].strip():
            return start, True
        return line_start, bool(self.text[boundary:line_start].strip())

    def _body_runs(self, entries: list[_Entry], start: int) -> None:
        names = [entry.name for entry in entries if entry.name is not None]
        if len(names) != len(set(names)):
            return
        run: list[_Entry] = []
        previous_end = start
        for entry in entries:
            span = self._line_span(entry)
            if entry.name is None or span is None:
                self._sort_run(run)
                run = []
            else:
                leading_start, barrier = self._leading_start(previous_end, span[0])
                if barrier:
                    self._sort_run(run)
                    run = []
                run.append(_Entry(leading_start, entry.end, entry.name))
            previous_end = entry.end if span is None else span[1]
        self._sort_run(run)

    def body(self, start: int, stop: int, *, properties: bool) -> bool:
        entries: list[_Entry] = []
        index = start
        while index < stop:
            first = self.tokens[index]
            if first.kind == "directive":
                entries.append(_Entry(first.start, first.end))
                index += 1
                continue
            cursor = index
            assigned = False
            while cursor < stop:
                token = self.tokens[cursor]
                if token.kind == "directive":
                    return False
                if token.value == "=":
                    assigned = True
                if token.value == "{":
                    close = self.pairs[cursor]
                    if cursor > index and self.tokens[cursor - 1].value == "&":
                        cursor = close + 1
                        continue
                    if assigned:
                        return False
                    if close + 1 >= stop or self.tokens[close + 1].value != ";":
                        return False
                    header = [part.value for part in self.tokens[index:cursor]]
                    if _node_header(header) and not self.body(cursor + 1, close, properties=True):
                        return False
                    entries.append(_Entry(first.start, self.tokens[close + 1].end))
                    index = close + 2
                    break
                if token.value == ";":
                    name = None
                    if (
                        properties
                        and _PROPERTY.fullmatch(first.value)
                        and (cursor == index + 1 or self.tokens[index + 1].value == "=")
                    ):
                        name = first.value
                    entries.append(_Entry(first.start, token.end, name))
                    index = cursor + 1
                    break
                cursor += 1
            else:
                return False
        if properties:
            boundary = self.tokens[start - 1].end
            self._body_runs(entries, boundary)
        return True

    def result(self) -> str:
        text = self.text
        for start, end, replacement in sorted(self.edits, reverse=True):
            text = text[:start] + replacement + text[end:]
        return text


def _final_newline(text: str) -> str:
    if text and not text.endswith(("\n", "\\")):
        return text + "\n"
    return text


def _dts(text: str) -> str:
    tokens = _tokens(text)
    if tokens is None:
        return text
    pairs = _brace_pairs(tokens)
    if pairs is None:
        return text
    order = _DtsOrder(text, tokens, pairs)
    if not order.body(0, len(tokens), properties=False):
        return text
    return _final_newline(order.result())


def _shell(text: str) -> str:
    # A final newline can be literal heredoc data until its terminator is known.
    if "<<" in text:
        return text
    try:
        shlex.split(text, comments=True)
    except ValueError:
        return text
    return _final_newline(text)


def _declarations(text: str, *, fragment: bool) -> str:
    result: list[str] = []
    for line in text.splitlines(keepends=True):
        contents = line.removesuffix("\n")
        stripped = contents.strip(" \t")
        safe = not stripped or stripped.startswith(("#", ";"))
        if fragment:
            safe = safe or _CONFIG_VALUE.fullmatch(stripped) is not None
        else:
            safe = (
                safe
                or (stripped.startswith("[") and stripped.endswith("]"))
                or bool(re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]*[ \t]*[=:].*", stripped))
            )
        if safe:
            contents = contents.rstrip(" \t")
        result.append(contents + ("\n" if line.endswith("\n") else ""))
    return _final_newline("".join(result))


def normalize_text(relative: str, contents: bytes) -> bytes:
    """Return canonical bytes for the recognized safe subset of a text source.

    DTS properties move only within complete line runs. Directives, child nodes,
    section comments and unknown statements stay in place; contiguous leading
    comments travel with their property. Duplicate properties disable sorting
    in their containing body. Strings and value tokens are kept verbatim.
    Executable configuration and assembly only gain a final newline; their
    assignments, recipes, help text and literal whitespace are preserved.
    """
    if b"\x00" in contents or b"\r" in contents:
        return contents
    try:
        text = contents.decode("utf-8")
    except UnicodeDecodeError:
        return contents
    effective = PurePosixPath(relative.removesuffix(".in"))
    name = effective.name
    if effective.suffix in {".dts", ".dtsi"}:
        normalized = _dts(text)
    elif name == "APKBUILD":
        normalized = _shell(text)
    elif name == ".editorconfig" or effective.suffix == ".ini":
        normalized = _declarations(text, fragment=False)
    elif effective.suffix == ".fragment":
        normalized = _declarations(text, fragment=True)
    elif (
        "Kconfig" in name
        or "Makefile" in name
        or name == "Config.in"
        or PurePosixPath(relative).name == "Config.in"
        or effective.suffix in {".s", ".S", ".mk"}
        or relative.endswith(".txt.in")
    ):
        normalized = _final_newline(text)
    else:
        normalized = text
    return normalized.encode("utf-8")
