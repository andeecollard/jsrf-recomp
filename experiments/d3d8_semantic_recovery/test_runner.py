import importlib.util
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("d3d8_semantic_run", HERE / "run.py")
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)
TO_SEMIF_SPEC = importlib.util.spec_from_file_location("d3d8_to_semif", HERE / "to_semif.py")
TO_SEMIF = importlib.util.module_from_spec(TO_SEMIF_SPEC)
TO_SEMIF_SPEC.loader.exec_module(TO_SEMIF)


class RunnerTests(unittest.TestCase):
    def test_fixture_labels_and_ids(self):
        cases = RUN.load_cases(HERE / "cases.jsonl")
        self.assertGreaterEqual(len(cases), 12)
        self.assertEqual(len({case["id"] for case in cases}), len(cases))
        self.assertTrue(all(case["expected"] in RUN.OPTIONS for case in cases))

    def test_extract_plain_json(self):
        got = RUN.extract_json('{"choice":"swap","confidence":0.9}')
        self.assertEqual(got["choice"], "swap")

    def test_extract_fenced_json(self):
        text = '```json\n{"choice":"clear","confidence":0.8}\n```'
        self.assertEqual(RUN.extract_json(text)["choice"], "clear")

    def test_semif_evidence_excludes_answer_bearing_metadata(self):
        case = {"id": "draw_indexed", "expected": "draw_indexed",
                "label_source": "known", "methods": ["NV097_ARRAY_ELEMENT32"]}
        row = next(TO_SEMIF.convert([case]))
        self.assertEqual(row["id"], "draw_indexed")
        self.assertEqual(row["state"], {"methods": ["NV097_ARRAY_ELEMENT32"]})


if __name__ == "__main__":
    unittest.main()
