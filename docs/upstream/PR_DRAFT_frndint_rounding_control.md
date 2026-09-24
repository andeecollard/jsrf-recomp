TITLE: fix(lifter): frndint rounds under the guest's x87 control word

---

`frndint` is lifted as

```c
fp_top() = rint(fp_top()); /* frndint */
```

`rint()` rounds in the **host's** rounding mode. The guest's `fldcw` never reaches the host: it only writes `g_fp_control_word`. So every lifted `frndint` rounds to nearest, whatever RC the guest loaded.

That matters because of how MSVC's CRT spells `floor()` and `ceil()`. Neither is its own instruction. Each is `_ctrlfp` setting RC to down or up, a call to `_frnd`, which is a bare `fld; frndint`, and `_ctrlfp` again to restore the old mode:

```
floor(x):  _ctrlfp(RC=down)  ->  _frnd(x) = frndint  ->  _ctrlfp(old)
ceil(x):   _ctrlfp(RC=up)    ->  _frnd(x) = frndint  ->  _ctrlfp(old)
```

Lifted, the `fldcw` lands in `g_fp_control_word` and the `frndint` ignores it. `floor(2.7)` returns 3 and `ceil(2.3)` returns 2. Any title linked against that CRT gets a `floor`/`ceil` that is really `round`.

### How it showed up

In JSRF the skeletal-animation sampler takes its keyframe pair from `floor` and `ceil` of the frame position, and wraps only the upper key back to key 0. In the last half-key before the loop point, `floor` rounded up and the lower key was one past the end of the table. A looping animation drew one frame of garbage bones per loop, and a cutscene lost its whole scene for one frame a second.

This is the same class of bug #47 fixed for `FIST`/`FISTP`, which already honour the RC bits through `recomp_fist`. `frndint` was the one rounding instruction left on the host's mode.

### The change

- `templates/runtime/recomp_types.h`: new `recomp_frndint(value, control)` applies RC bits 10-11: nearest-even, down, up, toward zero. NaN and the infinities pass through unchanged. A zero result keeps the operand's sign, as FRNDINT's does: `frndint(-0.3)` is `-0.0`, not `+0.0`. `recomp_fist` now rounds through it instead of carrying its own copy of the same switch. Its range check and integer-indefinite result are unchanged.
- `tools/recomp/lifter.py`: `frndint` emits `fp_top() = recomp_frndint(fp_top(), g_fp_control_word);`.

No host `fesetround` is involved, so nothing depends on the host FPU state or leaks into host code.

### Testing

`test_lifter_frndint.py` checks the emitted statement. It also compiles the header with `-Wall -Wextra -Werror` and runs the helper, because a text check cannot tell a correct nearest-even from one that rounds halves up. The run covers each rounding mode on positive and negative fractions and on nearest-even halves (2.5 -> 2, 3.5 -> 4, -1.5 -> -2). It also covers integral and huge values, NaN, infinity, the sign of a zero result, and `recomp_fist` through the shared path (including its integer indefinite). `test_lifter_fpu_reverse.py`'s expected `frndint` spelling changes to match.

**Negative controls, run rather than assumed:**
- With the original `lifter.py` and `recomp_types.h` restored, both new tests fail, and so does the updated line in `test_lifter_fpu_reverse.py`.
- With a `recomp_frndint` that just returns `rint(value)`, i.e. the old behaviour behind the new name, the runtime test fails on its first check: `floor`-mode 59.6 gives 60.

`python -m pytest tools/ -q` gives 388 passed, 2 skipped. A clean `main` (766ecef) on the same machine (macOS, arm64) gives 386 passed, 2 skipped. The two extra passes are the new tests, and nothing else changes.

A lifted binary picks this up only after its generated C is regenerated.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_017QvivF8psGzR3Ws8tv21Ut
