"""Vtable seed feedback must distinguish aliases from measured bodies."""

import json

from tools.func_id.output import _write_seed_file


def test_alias_extent_does_not_hide_independent_vtable_seed(tmp_path):
    functions = [
        {"start": "0x0007BDD0", "end": "0x0007D5D0",
         "detection_method": "tail_jump_alias"},
    ]
    discovered = [{"start": "0x0007BE30", "method": "vtable_thunk"}]
    path = tmp_path / "seeds.json"
    assert _write_seed_file(path, functions, discovered) == 1
    assert json.loads(path.read_text())[0]["start"] == "0x0007BE30"


def test_alias_does_not_unprotect_real_enclosing_function(tmp_path):
    functions = [
        {"start": "0x00001000", "end": "0x00001100",
         "detection_method": "call_target"},
        {"start": "0x00001020", "end": "0x00001100",
         "detection_method": "tail_jump_alias"},
    ]
    discovered = [{"start": "0x00001040", "method": "vtable_thunk"}]
    path = tmp_path / "seeds.json"
    assert _write_seed_file(path, functions, discovered) == 0
    assert json.loads(path.read_text()) == []
