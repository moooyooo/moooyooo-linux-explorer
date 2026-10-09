"""Exercise malformed project states without visiting real user folders."""

import copy
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_spec import ROOT, check_project, load_json


class ProjectContractTests(unittest.TestCase):
    def setUp(self):
        self.schema = load_json(ROOT / "schemas/project.schema.json")
        self.project = load_json(ROOT / "examples/sample-project.json")

    def test_sample_and_empty_workspace_are_valid(self):
        check_project(self.project, self.schema)
        self.project["panes"] = []
        self.project["active_pane_id"] = None
        check_project(self.project, self.schema)

    def test_duplicate_ids_and_broken_active_reference_are_rejected(self):
        self.project["panes"][1]["pane_id"] = self.project["panes"][0]["pane_id"]
        with self.assertRaises(ValueError):
            check_project(self.project, self.schema)
        self.setUp()
        self.project["active_pane_id"] = self.project["project_id"]
        with self.assertRaises(ValueError):
            check_project(self.project, self.schema)

    def test_unknown_version_fields_and_wrong_types_are_rejected(self):
        cases = [("schema_version", 2), ("schema_version", True), ("revision", 0), ("revision", 10 ** 400), ("name", ""), ("command", "ignored command")]
        for key, value in cases:
            with self.subTest(field=key, value=value):
                project = copy.deepcopy(self.project)
                project[key] = value
                with self.assertRaises(ValueError):
                    check_project(project, self.schema)

    def test_path_kinds_and_traversal_are_checked(self):
        cases = [("home_relative", "../sample"), ("home_relative", "/sample"), ("home_relative", "https://example.test"), ("home_relative", "a\0b"), ("absolute", "sample"), ("remote", "sample")]
        for kind, path in cases:
            with self.subTest(kind=kind):
                project = copy.deepcopy(self.project)
                project["panes"][0]["location"] = {"kind": kind, "path": path}
                with self.assertRaises(ValueError):
                    check_project(project, self.schema)
        self.project["panes"][0]["location"]["path"] = ""
        check_project(self.project, self.schema)

    def test_out_of_bounds_geometry_and_columns_are_rejected(self):
        self.project["panes"][1]["rect"]["width"] = 0.8
        with self.assertRaises(ValueError):
            check_project(self.project, self.schema)
        self.setUp()
        self.project["panes"][0]["view"]["columns"] = ["size"]
        with self.assertRaises(ValueError):
            check_project(self.project, self.schema)

    def test_duplicate_json_keys_and_invalid_constants_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix="mlex-spec-") as directory:
            path = Path(directory) / "sample.json"
            for text in ('{"revision": 1, "revision": 2}', '{"revision": NaN}', '{"revision": Infinity}'):
                path.write_text(text)
                with self.assertRaises(ValueError):
                    load_json(path)

    def test_resource_limits_are_enforced(self):
        self.project["panes"] = [copy.deepcopy(self.project["panes"][0]) for _ in range(65)]
        with self.assertRaises(ValueError):
            check_project(self.project, self.schema)


if __name__ == "__main__":
    unittest.main()
