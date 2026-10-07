# SPDX-License-Identifier: GPL-2.0-only
"""Order known YAML document fields without rewriting their values."""

from __future__ import annotations

from pathlib import PurePosixPath
from typing import TYPE_CHECKING, cast

from ruamel.yaml import YAML
from ruamel.yaml.error import YAMLError
from ruamel.yaml.nodes import MappingNode, ScalarNode, SequenceNode
from ruamel.yaml.tokens import AliasToken

if TYPE_CHECKING:
    from ruamel.yaml.nodes import Node

_BINDING_FIELDS = (
    "$id",
    "$schema",
    "title",
    "maintainers",
    "description",
    "select",
    "allOf",
    "properties",
    "patternProperties",
    "required",
    "dependencies",
    "additionalProperties",
    "unevaluatedProperties",
    "examples",
)
_WORKFLOW_FIELDS = (
    "name",
    "run-name",
    "on",
    "permissions",
    "concurrency",
    "env",
    "defaults",
    "jobs",
)
_JOB_FIELDS = (
    "name",
    "needs",
    "if",
    "runs-on",
    "permissions",
    "environment",
    "concurrency",
    "strategy",
    "container",
    "services",
    "timeout-minutes",
    "continue-on-error",
    "defaults",
    "env",
    "outputs",
    "uses",
    "with",
    "secrets",
    "steps",
)
_STEP_FIELDS = (
    "name",
    "id",
    "if",
    "uses",
    "run",
    "with",
    "working-directory",
    "shell",
    "env",
    "continue-on-error",
    "timeout-minutes",
)
_ISSUE_FIELDS = ("name", "description", "title", "labels", "assignees", "projects", "body")
_ISSUE_ITEM_FIELDS = ("type", "id", "attributes", "validations")
_ISSUE_ATTRIBUTE_FIELDS = (
    "label",
    "description",
    "placeholder",
    "value",
    "render",
    "options",
)


def _field_order(relative: str, path: tuple[str, ...]) -> tuple[str, ...]:  # noqa: PLR0911 - Distinct document owners.
    if "/linux/bindings/" in relative and not path:
        return _BINDING_FIELDS
    if relative.startswith(".github/workflows/"):
        if not path:
            return _WORKFLOW_FIELDS
        if len(path) == 2 and path[0] == "jobs":
            return _JOB_FIELDS
        if len(path) == 4 and path[0] == "jobs" and path[2:] == ("steps", "*"):
            return _STEP_FIELDS
    if relative.startswith(".github/ISSUE_TEMPLATE/"):
        if not path:
            return _ISSUE_FIELDS
        if path == ("body", "*"):
            return _ISSUE_ITEM_FIELDS
        if path == ("body", "*", "attributes"):
            return _ISSUE_ATTRIBUTE_FIELDS
    return ()


def _start(node: Node) -> int:
    return cast("int", node.start_mark.index)


def _end(node: Node, text: str) -> int:
    end = cast("int", node.end_mark.index)
    if isinstance(node, MappingNode | SequenceNode) and not node.flow_style:
        line_start = text.rfind("\n", 0, end) + 1
        if not text[line_start:end].strip():
            return line_start
    return end


def _value_end(node: Node, text: str) -> int:
    """Find the last value token, excluding comments before its next sibling."""
    if isinstance(node, MappingNode) and not node.flow_style and node.value:
        pairs = cast("list[tuple[Node, Node]]", node.value)
        return _value_end(pairs[-1][1], text)
    if isinstance(node, SequenceNode) and not node.flow_style and node.value:
        items = cast("list[Node]", node.value)
        return _value_end(items[-1], text)
    return _end(node, text)


def _leading_start(key: Node, previous: Node, text: str) -> int:
    boundary = text.rfind("\n", 0, _start(key)) + 1
    minimum = _value_end(previous, text)
    column = cast("int", key.start_mark.column)
    while boundary > minimum:
        line_start = text.rfind("\n", 0, boundary - 1) + 1
        line = text[line_start:boundary].rstrip("\r\n")
        if line_start < minimum:
            break
        if line.strip() and not (line.startswith(" " * column + "#") and len(line) > column):
            break
        boundary = line_start
    return boundary


def _replace_children(node: Node, path: tuple[str, ...], relative: str, text: str) -> str:
    start, end = _start(node), _end(node, text)
    replacements: list[tuple[int, int, str]] = []
    if isinstance(node, MappingNode):
        pairs = cast("list[tuple[Node, Node]]", node.value)
        for key, value in pairs:
            name = key.value if isinstance(key, ScalarNode) else ""
            replacements.append(
                (_start(value), _end(value, text), _render(value, (*path, name), relative, text)),
            )
    elif isinstance(node, SequenceNode):
        items = cast("list[Node]", node.value)
        replacements.extend(
            (_start(value), _end(value, text), _render(value, (*path, "*"), relative, text))
            for value in items
        )
    pieces: list[str] = []
    position = start
    for child_start, child_end, replacement in replacements:
        pieces.extend((text[position:child_start], replacement))
        position = child_end
    pieces.append(text[position:end])
    return "".join(pieces)


def _render(node: Node, path: tuple[str, ...], relative: str, text: str) -> str:
    order = _field_order(relative, path)
    if not isinstance(node, MappingNode) or not order or not node.value:
        return _replace_children(node, path, relative, text)
    pairs = cast("list[tuple[Node, Node]]", node.value)
    ranks = {name: index for index, name in enumerate(order)}
    names = [key.value if isinstance(key, ScalarNode) else "" for key, _ in pairs]
    permutation = sorted(range(len(pairs)), key=lambda index: ranks.get(names[index], len(order)))
    if permutation == list(range(len(pairs))):
        return _replace_children(node, path, relative, text)
    if node.flow_style:
        msg = f"{relative}: field ordering requires a block YAML mapping"
        raise ValueError(msg)

    boundaries = [_start(pairs[0][0])]
    boundaries.extend(
        _leading_start(pairs[index][0], pairs[index - 1][1], text)
        for index in range(1, len(pairs))
    )
    boundaries.append(_end(node, text))
    indent = " " * cast("int", pairs[0][0].start_mark.column)
    blocks: list[str] = []
    for index, ((_, value), name) in enumerate(zip(pairs, names, strict=True)):
        block_start, block_end = boundaries[index : index + 2]
        child_start, child_end = _start(value), _end(value, text)
        block = (
            text[block_start:child_start]
            + _render(value, (*path, name), relative, text)
            + text[child_end:block_end]
        )
        if index:
            block = block[len(indent) :]
        blocks.append(block)
    result: list[str] = []
    for position, index in enumerate(permutation):
        block = blocks[index]
        result.append((indent if position else "") + block)
        if not block.endswith("\n"):
            result.append("\n")
    return "".join(result)


def normalize_yaml(relative: str, contents: bytes) -> bytes:
    """Order binding and project configuration fields; retain consumer sequences."""
    relative = relative.removesuffix(".in")
    if PurePosixPath(relative).suffix not in {".yaml", ".yml"}:
        return contents
    text = contents.decode("utf-8")
    yaml = YAML(typ="rt", pure=True)
    try:
        yaml.load(text)
        if any(isinstance(token, AliasToken) for token in yaml.scan(text)):
            msg = f"{relative}: YAML aliases are not supported by field ordering"
            raise ValueError(msg)
        node = cast("Node | None", yaml.compose(text))
    except YAMLError as error:
        msg = f"{relative}: invalid YAML: {error}"
        raise ValueError(msg) from error
    if node is None:
        return contents
    normalized = (
        text[: _start(node)] + _render(node, (), relative, text) + text[_end(node, text) :]
    )
    return normalized.encode("utf-8")
