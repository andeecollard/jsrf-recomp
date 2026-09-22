"""
Main disassembly orchestrator.

Coordinates all analysis passes and produces the final output.
"""

import os
import time
from pathlib import Path
from typing import List, Optional

from . import config
from .loader import load_image, BinaryImage, SectionInfo, _find_analysis_json
from .engine import DisasmEngine
from .functions import FunctionDetector
from .xrefs import build_xrefs, XRefTracker
from .labels import (
    LabelManager, populate_kernel_labels, populate_entry_point,
    extract_strings, populate_string_labels,
)
from .output import OutputWriter, print_stats
from .cache import AnalysisCache


class Disassembler:
    """
    Top-level disassembly orchestrator.

    Usage:
        d = Disassembler("path/to/default.xbe")
        d.run()
    """

    def __init__(self, xbe_path: str,
                 analysis_json: Optional[str] = None,
                 output_dir: Optional[str] = None,
                 text_only: bool = False,
                 stats_only: bool = False,
                 verbose: bool = False,
                 force: bool = False,
                 extra_sections: Optional[list] = None,
                 seed_functions: Optional[list] = None,
                 function_bounds: Optional[list] = None,
                 seed_provenance: Optional[dict] = None):
        self.xbe_path = xbe_path
        self.analysis_json = analysis_json
        self.output_dir = output_dir or config.DEFAULT_OUTPUT_DIR
        self.text_only = text_only
        self.stats_only = stats_only
        self.verbose = verbose
        self.force = force
        self.extra_sections = extra_sections or []
        self.seed_functions = seed_functions or []
        self.function_bounds = function_bounds or []
        self.seed_provenance = seed_provenance or {}

        # Components (initialized during run)
        self.image: Optional[BinaryImage] = None
        self.engine: Optional[DisasmEngine] = None
        self.labels: Optional[LabelManager] = None
        self.xrefs: Optional[XRefTracker] = None
        self.func_detector: Optional[FunctionDetector] = None
        self.strings: List[dict] = []

    def _write_jump_tables(self) -> None:
        """Every switch table the engine measured, with its arms.

        The lifter reads a table straight out of the XBE and has to guess
        where it ends, so it stops at the first entry outside the function
        it is translating. A table whose cases alternate between local arms
        and other functions -- JSRF's 0x001FA008 in .rdata, 21 entries, half
        of them one shared handler -- then resolves to one arm and is thrown
        away. The engine knows the exact length (see resync_jump_tables), and
        this hands it over: tools.recomp reads the file from --disasm-dir.
        """
        import json
        tables = {}
        for tbl, end in sorted(list(self.engine.jump_tables.items())
                               + list(self.engine.short_jump_tables.items())):
            tables[f"0x{tbl:08X}"] = {
                "end": f"0x{end:08X}",
                "entries": [f"0x{e:08X}"
                            for e in self.engine.jump_table_entries(tbl)],
                "sites": [f"0x{a:08X}"
                          for a in self.engine.jump_table_sites(tbl)],
            }
        with open(os.path.join(self.output_dir, "jump_tables.json"), "w") as f:
            json.dump(tables, f, indent=1)
            f.write("\n")

    def run(self) -> bool:
        """
        Execute the full disassembly pipeline.

        Returns True on success.
        """
        t_start = time.time()

        # Check cache
        cache = AnalysisCache(self.output_dir)
        if not self.force:
            json_path = self._find_analysis_json()
            if json_path and cache.is_valid(self.xbe_path, json_path,
                                             self.text_only,
                                             self.extra_sections,
                                             self.seed_functions):
                last_time = cache.get_last_run_time()
                print(f"Cache hit - results unchanged (last run: "
                      f"{last_time:.1f}s)")
                if self.stats_only:
                    self._load_and_print_cached_stats()
                return True

        # Phase 1: Load
        if self.verbose:
            print("Phase 1: Loading binary image...")
        self.image = load_image(self.xbe_path, self.analysis_json)
        if self.verbose:
            print(f"  Loaded: {self.image.filepath}")
            print(f"  Base: 0x{self.image.base_address:08X}  "
                  f"Entry: 0x{self.image.entry_point:08X}")
            print(f"  Sections: {len(self.image.sections)}  "
                  f"Kernel imports: {len(self.image.kernel_imports)}")

        # Determine sections to analyze
        sections = self._get_target_sections()
        if self.verbose:
            print(f"  Target sections: {', '.join(s.name for s in sections)}")

        # Phase 2: Labels (pre-populate known symbols)
        if self.verbose:
            print("\nPhase 2: Populating labels...")
        self.labels = LabelManager()
        populate_entry_point(self.labels, self.image)
        ki_count = populate_kernel_labels(self.labels, self.image)
        if self.verbose:
            print(f"  Kernel import labels: {ki_count}")

        # Extract strings from .rdata
        self.strings = extract_strings(self.image)
        str_count = populate_string_labels(self.labels, self.strings)
        if self.verbose:
            print(f"  String labels: {str_count}")
            print(f"  Total labels: {self.labels.count()}")

        # Phase 3: Disassembly (linear sweep)
        if self.verbose:
            print("\nPhase 3: Linear sweep disassembly...")
        self.engine = DisasmEngine(self.image)

        total_insns = 0
        for sec in sections:
            if self.verbose:
                print(f"  {sec.name}: 0x{sec.virtual_addr:08X} "
                      f"({sec.virtual_size / 1024:.1f} KB)...", end="", flush=True)

            def progress(done, total):
                if self.verbose:
                    pct = done * 100 // total
                    print(f"\r  {sec.name}: 0x{sec.virtual_addr:08X} "
                          f"({sec.virtual_size / 1024:.1f} KB)... "
                          f"{pct}%", end="", flush=True)

            n = self.engine.linear_sweep(sec, progress_callback=progress)
            total_insns += n
            if self.verbose:
                print(f"\r  {sec.name}: {n:,d} instructions")

        if self.verbose:
            print(f"  Total: {total_insns:,d} instructions")

        # Realign after embedded switch tables before anything reads the
        # instruction stream. See DisasmEngine.resync_jump_tables().
        n_tables = self.engine.resync_jump_tables()
        if self.verbose:
            print(f"  Resynced past {n_tables:,d} embedded jump table(s)")

        # Phase 4: Cross-references
        if self.verbose:
            print("\nPhase 4: Building cross-references...")
        self.xrefs = build_xrefs(self.engine, self.image)
        if self.verbose:
            counts = self.xrefs.count_by_type()
            print(f"  Total xrefs: {self.xrefs.count():,d}")
            for xtype, count in sorted(counts.items()):
                print(f"    {xtype}: {count:,d}")

        # Phase 5: Function detection
        if self.verbose:
            print("\nPhase 5: Detecting functions...")
        self.func_detector = FunctionDetector(
            self.engine, self.image, self.xrefs, self.labels)
        # Declared extents, before any pass runs: they are what the seed
        # demotion tests against as well as being unsplittable themselves.
        self.func_detector._forced_bounds = list(self.function_bounds)
        self.func_detector._seed_provenance = dict(self.seed_provenance)

        # Add seed functions from vtable scanner or other sources.
        #
        # A candidate with no decoded instruction at its address is dropped by
        # _build_functions (num_insns == 0), so a seed that lands where the
        # linear sweep came out of phase is silently discarded -- the exact
        # failure Engine.decode_at was written for, but until now only wired
        # into _pass_call_targets and the tail-jump pass. Measured on JSRF:
        # 0x0007BE30 is 16-aligned, preceded by four nop padding bytes, opens
        # with the MSVC SEH prologue (push -1; push <handler>; mov eax, fs:[0];
        # push eax; mov fs:[0], esp), was discovered by the vtable scanner AND
        # observed as an unresolved indirect call 100 times at runtime -- and
        # was thrown away because the data run before it put the sweep out of
        # step. Realign there instead, exactly as the call-target pass does.
        if self.seed_functions:
            realigned = undecodable = 0
            mid_instruction = 0
            for addr in self.seed_functions:
                # Decode there first if the sweep stepped over it. A seed is an
                # explicit claim that a function starts at this address, and it
                # is usually the only evidence available -- seeds exist for
                # entry points that nothing in the image references. But
                # _build_functions needs instructions at the address to build a
                # body from, and where the sweep came out of phase there are
                # none, so the seed was dropped silently.
                #
                # Same treatment _pass_call_targets already gives a call target
                # it has to realign, and for the same reason.
                # A seed landing *inside* an instruction the sweep already
                # decoded is not a stream out of phase -- it is a bad seed.
                # Accepting it manufactures a boundary mid-instruction, and the
                # new "function" then clamps the end of the real one it sits
                # in, which loses that function its epilogue.
                #
                # Checked before the realign test, not inside it: an earlier
                # seed's decode_at can already have laid a chain through this
                # address, which made the address look like a legitimate
                # boundary and skipped the guard entirely.
                #
                # Half-Life 2 has two such slots out of 12,288, and one
                # (0x00202C2E, six bytes into a 9-byte mov) cut sub_00202BB9
                # short at 0x00202C31 instead of 0x00202D4F. That function has
                # 250 callers, and every one got back an unrestored
                # ebx/esi/ebp/edi and a leaked frame -- which drifts esp until
                # some later `pop esi` lifts a float off the stack and it gets
                # used as a `this` pointer.
                covering = self.engine.instruction_covering(addr)
                if covering is not None:
                    # ...unless the seed decodes as a function prologue, in
                    # which case the sweep is the one out of phase. It drifts
                    # whenever it walks zero padding or a data table as
                    # instructions and runs off the end into real code:
                    # default.xbe's XPP section decodes 001C950600558D at
                    # 0x00069533, which swallows the `push ebp` at 0x00069538
                    # that a tail jump targets. Rejecting that seed left the
                    # target stubbed, and the stub returned without the
                    # callee's `ret 8` -- which walked esp off by 4 and moved
                    # the loader's object pointer out from under its own
                    # vtable.
                    #
                    # A prologue is the evidence that separates the two cases:
                    # the bad HL2 seed at 0x00202C2E is six bytes into a mov
                    # and decodes as nothing of the kind.
                    if self.engine.probes_as_prologue(addr):
                        if self.engine.decode_at(addr):
                            realigned += 1
                            self.func_detector._add_candidate(
                                addr, 0.95, "seed_vtable_thunk")
                            continue
                    mid_instruction += 1
                    continue
                if addr not in self.engine.instructions:
                    if self.engine.decode_at(addr):
                        realigned += 1
                    else:
                        undecodable += 1
                        continue
                self.func_detector._add_candidate(addr, 0.95, "seed_vtable_thunk")
            if realigned:
                print(f"  Realigned {realigned} seeded address(es) the sweep "
                      f"stepped over")
            if mid_instruction:
                print(f"  Rejected {mid_instruction} seed(s) landing inside an "
                      f"already-decoded instruction")
            if self.verbose:
                print(f"  Seeded {len(self.seed_functions)} function addresses")
                if realigned or undecodable:
                    print(f"    realigned {realigned}, "
                          f"{undecodable} undecodable")

        num_funcs = self.func_detector.detect_all(sections)

        # The drop report is evidence, not a side effect: an interior seed that
        # something really references is the subset that needs a secondary
        # entry, and it is only visible here.
        dropped = getattr(self.func_detector, "dropped_seeds", None)
        kept = getattr(self.func_detector, "kept_interior_seeds", None)
        if dropped is not None or kept is not None:
            import json as _json
            os.makedirs(self.output_dir, exist_ok=True)
            _path = os.path.join(self.output_dir, "interior_seeds.json")
            with open(_path, "w") as _f:
                _json.dump({"dropped": dropped or [], "kept": kept or []},
                           _f, indent=2)
            print(f"  Wrote {_path}")
        if self.verbose:
            summary = self.func_detector.summary()
            print(f"  Total functions: {num_funcs:,d}")
            for method, count in sorted(
                    summary["by_detection_method"].items()):
                print(f"    {method}: {count:,d}")

        # Phase 6: Recursive descent validation
        if self.verbose:
            print("\nPhase 6: Recursive descent validation...")
        start_addrs = [self.image.entry_point]
        start_addrs.extend(self.func_detector.functions.keys())
        section_bounds = [
            (s.virtual_addr, s.virtual_addr + s.virtual_size)
            for s in sections
        ]
        reachable = self.engine.recursive_descent(start_addrs, section_bounds)
        if self.verbose:
            coverage = len(reachable) / total_insns * 100 if total_insns else 0
            print(f"  Reachable instructions: {len(reachable):,d} "
                  f"({coverage:.1f}%)")

        elapsed = time.time() - t_start

        # Print stats
        if self.stats_only or self.verbose:
            print_stats(self.engine, self.func_detector, self.xrefs,
                        self.labels, self.strings, self.image)
            print(f"\n  Elapsed: {elapsed:.2f}s")

        # Phase 7: Output
        if not self.stats_only:
            if self.verbose:
                print(f"\nPhase 7: Writing output to {self.output_dir}/...")
            writer = OutputWriter(
                self.output_dir, self.engine, self.func_detector,
                self.xrefs, self.labels, self.image, self.strings)
            writer.write_all(sections_to_disasm=sections, verbose=self.verbose)
            self._write_jump_tables()

            # Save cache
            json_path = self._find_analysis_json()
            if json_path:
                cache.save(self.xbe_path, json_path, self.text_only, elapsed,
                           self.extra_sections, self.seed_functions)

            if self.verbose:
                print(f"\n  Output written to {self.output_dir}/")

        print(f"Done in {elapsed:.2f}s")
        return True

    def _get_target_sections(self) -> List[SectionInfo]:
        """Determine which sections to disassemble."""
        if self.text_only:
            sections = []
            text = self.image.get_section(".text")
            if text is None:
                raise ValueError("No .text section found")
            sections.append(text)
        else:
            # All executable sections with code
            sections = list(self.image.get_code_sections())

        # Add extra sections (for sections marked non-executable but containing code)
        if self.extra_sections:
            existing_names = {s.name for s in sections}
            for name in self.extra_sections:
                if name in existing_names:
                    continue
                sec = self.image.get_section(name)
                if sec is None:
                    print(f"  Warning: extra section '{name}' not found in XBE")
                elif sec.raw_size == 0:
                    print(f"  Warning: extra section '{name}' has no raw data")
                else:
                    print(f"  Adding extra section '{name}' (VA=0x{sec.virtual_addr:08X}, "
                          f"size={sec.raw_size} bytes, executable={sec.executable})")
                    sections.append(sec)

        return sections

    def _find_analysis_json(self) -> Optional[str]:
        """Find the analysis JSON file path."""
        if self.analysis_json:
            return self.analysis_json

        found = _find_analysis_json(Path(self.xbe_path))
        return str(found) if found else None

    def _load_and_print_cached_stats(self) -> None:
        """Load and print stats from cached summary.json."""
        summary_path = Path(self.output_dir) / "summary.json"
        if not summary_path.exists():
            print("  (no cached summary available)")
            return

        import json
        with open(summary_path) as f:
            summary = json.load(f)

        print(f"\n{'=' * 60}")
        print(f"  Cached Disassembly Summary")
        print(f"{'=' * 60}")
        print(f"  Binary: {summary.get('binary', 'N/A')}")
        print(f"  Instructions: {summary.get('total_instructions', 0):,d}")
        print(f"  Functions: {summary.get('total_functions', 0):,d}")
        print(f"  Cross-references: {summary.get('total_xrefs', 0):,d}")
        print(f"  Labels: {summary.get('total_labels', 0):,d}")
        print(f"  Strings: {summary.get('total_strings', 0):,d}")
        print(f"{'=' * 60}")
