#!/usr/bin/env python3
"""Validate the public design fixture; not the application's runtime loader."""

from __future__ import annotations

import json
import math
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 2 * 1024 * 1024


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError("non-JSON numeric constant")


def load_json(path):
    with path.open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError("JSON exceeds 2 MiB")
    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object, parse_constant=invalid_constant)


def validate(value, rule, schema, path="$", depth=0):
    """Interpret only the keywords used by this repository's schema."""
    if depth > 32:
        raise ValueError("schema nesting limit exceeded")
    if "$ref" in rule:
        prefix = "#/$defs/"
        if not rule["$ref"].startswith(prefix):
            raise ValueError("only local definition references are supported")
        return validate(value, schema["$defs"][rule["$ref"][len(prefix):]], schema, path, depth + 1)
    if "oneOf" in rule:
        successes = 0
        for variant in rule["oneOf"]:
            try:
                validate(value, variant, schema, path, depth + 1)
                successes += 1
            except ValueError:
                pass
        if successes != 1:
            raise ValueError(f"{path}: expected exactly one matching variant")
        return
    checks = {
        "object": lambda x: type(x) is dict,
        "array": lambda x: type(x) is list,
        "string": lambda x: type(x) is str,
        "integer": lambda x: type(x) is int,
        "number": lambda x: type(x) is int or (type(x) is float and math.isfinite(x)),
        "boolean": lambda x: type(x) is bool,
        "null": lambda x: x is None,
    }
    if "type" in rule and not checks[rule["type"]](value):
        raise ValueError(f"{path}: incorrect type")
    if "const" in rule and value != rule["const"]:
        raise ValueError(f"{path}: incorrect constant")
    if "enum" in rule and value not in rule["enum"]:
        raise ValueError(f"{path}: unsupported value")
    if type(value) is dict:
        if not set(rule.get("required", ())).issubset(value):
            raise ValueError(f"{path}: required field missing")
        properties = rule.get("properties", {})
        if rule.get("additionalProperties") is False and set(value) - set(properties):
            raise ValueError(f"{path}: unknown field")
        for key, item in value.items():
            validate(item, properties.get(key, {}), schema, f"{path}.{key}", depth + 1)
    if type(value) is list:
        if not rule.get("minItems", 0) <= len(value) <= rule.get("maxItems", MAX_BYTES):
            raise ValueError(f"{path}: array size outside limits")
        if rule.get("uniqueItems") and len({json.dumps(v, sort_keys=True) for v in value}) != len(value):
            raise ValueError(f"{path}: duplicate array item")
        for index, item in enumerate(value):
            validate(item, rule.get("items", {}), schema, f"{path}[{index}]", depth + 1)
    if type(value) is str:
        if not rule.get("minLength", 0) <= len(value) <= rule.get("maxLength", MAX_BYTES):
            raise ValueError(f"{path}: string length outside limits")
        if "pattern" in rule and not re.search(rule["pattern"], value):
            raise ValueError(f"{path}: invalid string shape")
    if type(value) in (int, float):
        if (type(value) is float and not math.isfinite(value)) or not rule.get("minimum", -math.inf) <= value <= rule.get("maximum", math.inf):
            raise ValueError(f"{path}: numeric value outside limits")


def check_project(project, schema):
    validate(project, schema, schema)
    panes = project["panes"]
    ids = [pane["pane_id"] for pane in panes]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate pane ID")
    if (panes and project["active_pane_id"] not in ids) or (not panes and project["active_pane_id"] is not None):
        raise ValueError("invalid active pane reference")
    for pane in panes:
        location = pane["location"]
        path = location["path"]
        if any(ord(char) < 32 or ord(char) == 127 for char in path):
            raise ValueError("control character in location")
        if any(part in (".", "..") for part in path.split("/")):
            raise ValueError("relative traversal segment in location")
        if location["kind"] == "absolute" and not path.startswith("/"):
            raise ValueError("absolute location must start with slash")
        if location["kind"] == "home_relative" and (path.startswith("/") or "://" in path):
            raise ValueError("home-relative location must not be absolute or a URL")
        rect = pane["rect"]
        if rect["x"] + rect["width"] > 1.000000001 or rect["y"] + rect["height"] > 1.000000001:
            raise ValueError("pane extends beyond normalized workspace")
        if "name" not in pane["view"]["columns"]:
            raise ValueError("the name column is required")


def main():
    try:
        schema = load_json(ROOT / "schemas/project.schema.json")
        check_project(load_json(ROOT / "examples/sample-project.json"), schema)
        for path in ROOT.rglob("*.md"):
            for target in re.findall(r"\]\(([^)]+)\)", path.read_text()):
                if "://" in target or target.startswith("#"):
                    continue
                local = (path.parent / target.split("#", 1)[0]).resolve()
                if not local.is_relative_to(ROOT) or not local.exists():
                    raise ValueError(f"{path.relative_to(ROOT)}: broken local document link")
        print("PASS project schema, semantic constraints, sample, and local document links")
        return 0
    except (ValueError, OSError, KeyError, RecursionError) as error:
        print(f"FAIL specification: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
