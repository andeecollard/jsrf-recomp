# drawManyDefault filter probe

Instrumented the default-mode list walk at 0x110A0, including recovered alias
bodies. Installer: `diagnostics/jsrf_first_fault/instrument_draw_many.py`,
with `--gen` and `--remove`; validates all anchors before writing. Existing
drawOne instrumentation and Claude's changes are preserved.

Enable `RECOMP_DRAW_MANY_TRACE=1`. Optional `RECOMP_DRAW_MANY_ID=44` retains
only rows for global object ID 44, while all visit/dispatch/exclusion-test
counts remain unfiltered. The live unfiltered smoke exhausted 512 rows after
the intro; this demonstrated the need for the filter. Overflow is reported.
At most 512 rows per report, at least five seconds between reports, sampled
every 1024 events on the guest thread. Counters remain cumulative.

After four saved-register pushes, seven arguments occupy stack+0x14..0x2C.
Rows show the masks at +14 (any), +20 (all), +24 (child-any), +28 (none),
and +2C (extra transform). Object flags are +4 and child mask is +C.
All host diagnostic reads use checked guest mappings.

`reason` is a predicate classification from the observed operands, NOT proof
of the translated branch outcome: 1=any mask empty, 2=child mask empty,
3=all mask not contained, 4=exclusion mask nonempty, 0=none of these at this
site. Visit sites 110D0/111B1 classify the three initial predicates. Sites
110EB/111D0 sample exclusion after the intervening matrix helper. Actual
dispatch reach is independently counted at 110F0,11179,11186,111EC,111F9.
The target is vtable+0xC. This observes reaching the call sequence, not proof
of resolved execution, emitted vertices, or asynchronous GPU/Metal work.
Other game modes and draw-tree routes remain outside drawManyDefault.

Build and functional test passed:
```
cmake --build build-macos/jsrf-first-fault/build --target jsrf_first_fault -j4
python3 diagnostics/jsrf_first_fault/test_draw_many_probe.py
```
Existing linker common-section alignment warning remains. Installer was
tested twice for byte-identical install and exact removal, including aliases.
Mock-memory test checks predicate labels, independent dispatch totals, disabled
silence, and positive-control totals with an unmatched ID filter.

45-second smoke reused Claude's disposable HDD and existing Opening probe.
Log: build-macos/jsrf-first-fault/render-investigation/draw-many-smoke/stderr.log.
At t=47.97 (guest trace clock), visits=287688 dispatch=25996
exclusion_tests=15021; rows=512 invalid=0 overflow=247496. At least 86400 GPU
draws were reported. It exited by the harness's 45-second bounded stop.
Example retained dispatch: PC=110F0 object=04900980 id=247 vt=001CDA60
flags=00200010 any=10 all=0 child_any=7 none=0 extra=80000
target=000BBA40 count=132. This is positive-control smoke evidence, not a
tutorial causation finding. No xemu/recomp guest-state differential established.

Next: capture ID 44 at the visible tutorial fault, checking actual dispatch
reach against the sampled masks and registry identity. No visibility or
progression patches were made.
