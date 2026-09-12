"""
Function boundary detection for the disassembler.

Implements multi-pass function detection with confidence scoring:
1. Known addresses (entry point)
2. Standard prologues (push ebp; mov ebp, esp)
3. CC padding boundaries (CC run after ret)
4. Call targets (destinations of call instructions)
5. Cross-validation and overlap resolution
"""

import bisect
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from . import config
from .engine import DisasmEngine, Instruction
from .loader import BinaryImage, SectionInfo
from .xrefs import XRefTracker, XRefType
from .labels import LabelManager, Label, LabelType


@dataclass
class Function:
    """A detected function with boundaries and metadata."""
    start: int
    end: int           # Address after last instruction
    name: str
    section: str = ""
    confidence: float = 0.0
    detection_method: str = ""

    # Call graph data
    calls_to: List[int] = field(default_factory=list)     # Functions this calls
    called_by: List[int] = field(default_factory=list)     # Functions that call this

    # Instruction stats
    num_instructions: int = 0
    has_prologue: bool = False

    @property
    def size(self) -> int:
        return self.end - self.start

    def to_dict(self) -> dict:
        return {
            "start": f"0x{self.start:08X}",
            "end": f"0x{self.end:08X}",
            "size": self.size,
            "name": self.name,
            "section": self.section,
            "confidence": self.confidence,
            "detection_method": self.detection_method,
            "num_instructions": self.num_instructions,
            "has_prologue": self.has_prologue,
            "calls_to": [f"0x{a:08X}" for a in self.calls_to],
            "called_by": [f"0x{a:08X}" for a in self.called_by],
        }


class FunctionDetector:
    """
    Multi-pass function boundary detector.

    Identifies function start addresses through multiple heuristics,
    then determines function boundaries by following instruction flow
    until the next function or a terminal instruction.
    """

    def __init__(self, engine: DisasmEngine, image: BinaryImage,
                 xrefs: XRefTracker, labels: LabelManager):
        self.engine = engine
        self.image = image
        self.xrefs = xrefs
        self.labels = labels

        # Candidate function starts: address -> (confidence, method)
        self._candidates: Dict[int, Tuple[float, str]] = {}

        # Final function list
        self.functions: Dict[int, Function] = {}

        # Tail-jump targets landing inside another function: addr -> that
        # function's end. Kept out of self._candidates so they cannot truncate
        # the function they land in.
        self._alias_entries: Dict[int, int] = {}
        # [(start, end)] extents no pass may split. Populated from
        # --function-bounds; see _pass_demote_interior_seeds.
        self._forced_bounds: List[Tuple[int, int]] = []

    def detect_all(self, sections: Optional[List[SectionInfo]] = None) -> int:
        """
        Run all detection passes and build the function database.

        Args:
            sections: Sections to analyze. If None, uses all executable sections.

        Returns:
            Number of functions detected.
        """
        if sections is None:
            sections = self.image.get_code_sections()

        # Pass 1: Known addresses
        self._pass_known_addresses()

        # Pass 2: Prologue patterns
        for sec in sections:
            self._pass_prologues(sec)

        # Pass 3: CC padding boundaries
        for sec in sections:
            self._pass_cc_boundaries(sec)

        # Pass 4: Call targets
        self._pass_call_targets(sections)

        # Pass 4b: Function addresses installed into indirect-call slots
        self._pass_indirect_call_slots()

        # Pass 5: Build functions from candidates
        self._build_functions(sections)

        # Pass 6: Global callback slots. Optimized code often installs a code
        # address with `mov [absolute_slot], immediate` and reaches it only via
        # a later `call [absolute_slot]`. Neither the ordinary direct-call pass
        # nor prologue scanning can see such a function. Build once before this
        # pass so a candidate that lands in an existing body can be rejected
        # instead of truncating that body.
        callback_count = self._pass_global_callback_targets(sections)
        if callback_count:
            print(f"  global-callback pass: +{callback_count} standalone")
            self.functions.clear()
            self._build_functions(sections)

        # Pass 7: Tail-jump targets. A function reached only by "jmp" and never
        # by "call" is invisible to every pass above, so it is emitted as a stub
        # that returns without unwinding the frame its jumping caller built --
        # silently corrupting the simulated stack for everything upstream. Halo
        # had 127 of these; one of them (the CRT two-arg error handler) leaked
        # 0x24 bytes per call and turned a 6-iteration init loop into 21,938
        # allocations that exhausted the heap.
        #
        # Needs the bodies from pass 5 to tell a tail jump from an ordinary
        # intra-function branch, so it runs after and rebuilds. Iterate: a newly
        # found function can itself tail-jump somewhere new.
        for _round in range(8):
            before = len(self._candidates)
            if not self._pass_tail_jump_targets(sections):
                break
            print(f"  tail-jump pass {_round}: "
                  f"+{len(self._candidates) - before} standalone, "
                  f"{len(self._alias_entries)} aliases")
            self.functions.clear()
            self._build_functions(sections)

        # Function addresses taken as an immediate. Runs once, after the
        # bodies exist: the test is whether the target lands in a gap, which
        # needs the gaps to be known. One rebuild picks up what it finds.
        if self._pass_imm_ref_targets(sections):
            self.functions.clear()
            self._build_functions(sections)

        # Then the same for addresses that only ever exist as table entries.
        # After the immediate pass, so its results narrow the gaps first. These
        # become aliases rather than function starts, so no rebuild: aliases are
        # materialised by _build_alias_entries once boundaries are final.
        self._pass_data_ptr_targets(sections)

        # A function that begins immediately after a ret, with no padding.
        #
        # Last of the gap-testing passes, because it is the only one that
        # splits: the others add a start in a gap or claim a body as an alias,
        # while this one can land inside a function that nothing has claimed
        # yet and cut it in two. The data-table pass above claims 3,026 bodies
        # in JSRF against the tail-jump pass's 205, so running before it left
        # almost every table-reached function looking like open gap.
        #
        # That is how sub_00154D70 -- a switch dispatcher reached only through
        # a table, whose cases end in "ret 12" with the next case following
        # immediately -- was split at case 0x00154D82, leaving case 0x00154DAA
        # covered by nothing and emitted as a stub that popped 4 bytes instead
        # of 12.
        if self._pass_gap_prologues(sections):
            self.functions.clear()
            self._build_functions(sections)

        # Seeds that landed inside a function rather than on its start.
        #
        # Two passes, because a seed can be interior in two different senses.
        # _pass_demote_interior_seeds asks whether the address is inside a body
        # the sweep would have found WITHOUT any seed, which is the question
        # that matters and can only be asked once every other pass has run --
        # readStageObj's extent comes from the tail-jump and data-pointer
        # passes above, so asking earlier finds open gap where there is a
        # function, and demotes nothing. _pass_seed_aliases then catches the
        # remainder, where the enclosing body exists only because of some other
        # seed.
        if self._pass_demote_interior_seeds(sections):
            self.functions.clear()
            self._build_functions(sections)
        self._pass_seed_aliases()

        self._build_alias_entries()

        # Populate call graph
        self._build_call_graph()

        return len(self.functions)

    def _pass_gap_prologues(self, sections: List[SectionInfo]) -> bool:
        """A function that starts right after a ret, with no padding between.

        _pass_cc_boundaries only recognises a boundary when the compiler left
        int3 padding to the next alignment. It usually does -- but not when the
        next function happens to start on the boundary already, and then a
        clean prologue sits immediately after the previous function's ret with
        nothing to mark it.

        Nothing else covers that case either. Such a function is not a call
        target if it is only ever reached through a vtable, and the prologue
        pass looks for "push ebp; mov ebp, esp", which an FPO function like
        "sub esp, 0x18" does not have.

        Restricted to addresses in an unclaimed gap, which makes it safe by
        construction rather than by judgement: MSVC parks out-of-line tails
        after a ret too, and those look identical from here. The difference is
        that a tail belongs to the function above it, so _find_function_end has
        already walked over it and it is not in a gap. Half-Life 2 has 20 of
        these; 12 are in gaps and 8 are tails, and the gap test separates them.

        The one that mattered was 0x00476EB0, a material-system method reached
        only through a shader's vtable. Unresolved, the call was skipped rather
        than made, so eax kept a stale value that the caller then used as a
        string pointer.
        """
        # Alias entries count as covered. They are recorded by the tail-jump
        # and data-table passes above but only become Functions in
        # _build_alias_entries, which runs last -- so a body reached solely
        # through an alias is claimed territory that self.functions does not
        # yet describe, and testing self.functions alone calls it a gap.
        #
        # JSRF's 0x00154D70 is one: a switch dispatcher whose cases end in
        # "ret 12" and are followed immediately by the next case, reached only
        # by a tail jump. Splitting it at the case boundary 0x00154D82 left
        # 0x00154DAA -- another case of the same switch -- covered by nothing,
        # so it was emitted as an unresolved stub that popped 4 bytes where the
        # real block returns 12, and the guest faulted a long way downstream.
        bounds = sorted(
            [(f.start, f.end) for f in self.functions.values()]
            + list(self._alias_entries.items()))
        starts = [b[0] for b in bounds]

        # Ranges can overlap: an alias body runs to the end of the function it
        # lands in, so the range nearest below addr is not always the one that
        # covers it. Carry the furthest end seen so far, which answers "does
        # anything starting at or before addr still reach it" in one lookup.
        furthest_end: List[int] = []
        reach = 0
        for _, end in bounds:
            reach = max(reach, end)
            furthest_end.append(reach)

        def in_a_gap(addr: int) -> bool:
            i = bisect.bisect_right(starts, addr) - 1
            return not (i >= 0 and addr < furthest_end[i])

        added = False
        for insn in list(self.engine.instructions.values()):
            if not insn.is_ret:
                continue
            nxt = insn.end_address
            if nxt in self._candidates or nxt in self.functions:
                continue
            section = self.image.get_section_at_va(nxt)
            if section is None or not section.executable:
                continue
            if not in_a_gap(nxt):
                continue                    # an out-of-line tail, not a start
            # A prologue, or a whole small function.
            #
            # MSVC packs runs of constant-returning accessors -- "mov eax,
            # <address>; ret", six bytes each -- back to back with no padding,
            # and they are reached only through vtables. There is no prologue
            # to recognise because there is no frame; the entire function is
            # two instructions. Half-Life 2 has 3,147 of that exact shape, of
            # which 19 land in a gap and are found by nothing else.
            #
            # The gap restriction is what makes this safe. The same two
            # instructions appear 795 times *inside* larger functions as a
            # return path, and splitting one of those would cut a function in
            # half -- but those are covered, so they are not in a gap.
            if not (self.engine.probes_as_prologue(nxt)
                    or self.engine.probes_as_constant_stub(nxt)):
                continue
            self._add_candidate(nxt, config.CONFIDENCE_CC_BOUNDARY,
                                "gap_prologue")
            added = True

        if added:
            print("  functions recovered that follow a ret with no padding")
        return added

    def _pass_demote_interior_seeds(self, sections) -> bool:
        """Seeds inside another function's natural extent become aliases.

        The test cannot be "is it inside a body a seedless build would find".
        readStageObj's own start is a seed too, so withholding seeds turns the
        whole region into open gap and every interior seed then looks like a
        legitimate discovery -- measured: 2 demotions out of 1,334 carves.

        The question is instead where the *owner* would end if nothing cut it
        short. _find_function_end walks fall-through and internal branches to a
        terminator and takes `next_func` only as an upper bound, so calling it
        with no bound gives the natural extent. Any seed strictly inside that
        is not a new function, it is a second entry point into an existing one
        -- exactly what _pass_seed_aliases builds, and callable without
        truncating the code the call wanted to reach.

        Addresses in a boundary-override file are authoritative and tested
        first: that is the escape hatch XenonRecomp settled on for the same
        problem, its analyser being unable to bound functions containing jump
        tables either.
        """
        # OFF BY DEFAULT. RECOMP_SEED_INTERIOR=1 opts in.
        #
        # The pass does what it says -- it healed 1,134 of JSRF's 1,334 carved
        # fragments and made FileManager::readStageObj one function again --
        # and the resulting build SIGBUSes before its first frame. Measured
        # against a control regenerated from the same tree with this line
        # taken: 25 reports and 131 ABI offenders with the pass off, 0 reports
        # and 3 with it on. The 3 were not a fix, they were a corpse.
        #
        # The likely reason is that demoting a seed to an alias is not free. An
        # alias body runs from the seed to the OWNER's end, so it executes the
        # owner's epilogue -- popping registers that the owner's prologue
        # pushed above the alias entry and the alias itself never did. That is
        # the same stack walk the carve caused, in the opposite direction, and
        # 1,160 new aliases is a lot of chances to hit it at startup.
        #
        # So the next attempt should probably not alias at all: an interior
        # seed that is not a real entry point wants dropping, which leaves the
        # ICALL unresolved and loud, rather than aliased and silently wrong.
        # Whichever way, the carve measurement above is real and the mechanism
        # is understood; only the repair is unproven.
        if os.environ.get("RECOMP_SEED_INTERIOR") != "1":
            return False

        seed_set = {addr for addr, (_c, method) in self._candidates.items()
                    if method == "seed_vtable_thunk"}
        if not seed_set and not self._forced_bounds:
            return False

        forced = sorted(self._forced_bounds)
        forced_starts = [b[0] for b in forced]
        demoted = 0

        # Ascending, carrying the owner forward. A run of fragments has to be
        # judged against the function that owns the whole run, not against the
        # fragment immediately before -- that one ends exactly where the next
        # begins, so every seed after the first would look like a fresh start
        # and the carve would survive with only its first cut healed.
        owner = None
        owner_end = 0
        # Seeds that never became functions are still candidates that a later
        # rebuild would promote, so they are judged here too.
        for start in sorted(set(self.functions) | seed_set):
            if start in seed_set:
                j = bisect.bisect_right(forced_starts, start) - 1
                if j >= 0 and forced[j][0] < start < forced[j][1]:
                    self._alias_entries.setdefault(start, forced[j][1])
                    del self._candidates[start]
                    demoted += 1
                    continue
                if owner is not None and owner < start < owner_end:
                    self._alias_entries.setdefault(start, owner_end)
                    del self._candidates[start]
                    demoted += 1
                    continue
            owner = start
            section = self.image.get_section_at_va(start)
            sec_end = (section.virtual_addr + section.virtual_size
                       if section else None)
            owner_end = self._find_function_end(start, None, sec_end)

        if demoted:
            print(f"  seed-interior pass: {demoted} seed(s) demoted to aliases "
                  f"rather than splitting their enclosing function")
        return demoted > 0

    def _pass_seed_aliases(self) -> None:
        """
        Turn seeded addresses that fell inside a function into alias entries.

        A seed is usually a function start the detector could not reach, but an
        indirect-branch target need not be one: Wreckless's D3DX calls
        0x0010E9C3 and 0x0010EA2D, each in the middle of a function, as
        alternate entry points that share the tail.

        Splitting the enclosing function is not an option -- that truncates the
        code the call wanted to reach. Neither is dropping it, which leaves the
        ICALL unresolved and its stub returning without the epilogue, walking
        esp off by the callee's argument bytes. An alias body, exactly as the
        tail-jump pass builds for the same shape, is callable and keeps the
        original intact.

        Runs after the bodies exist, since "inside a function" is not knowable
        before that.
        """
        seeded = sorted(addr for addr, (_conf, method) in self._candidates.items()
                        if method == "seed_vtable_thunk")
        if not seeded:
            return
        bodies = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bodies]
        for addr in seeded:
            if addr in self.functions:
                continue
            i = bisect.bisect_right(starts, addr) - 1
            if i < 0:
                continue
            body_start, body_end = bodies[i]
            if not (body_start < addr < body_end):
                continue
            if addr not in self._alias_entries:
                self._alias_entries[addr] = body_end

    def _add_candidate(self, addr: int, confidence: float, method: str) -> None:
        """Add a function start candidate, keeping highest confidence."""
        existing = self._candidates.get(addr)
        if existing is None or confidence > existing[0]:
            self._candidates[addr] = (confidence, method)

    def _pass_known_addresses(self) -> None:
        """Pass 1: Add known function addresses."""
        # Entry point
        self._add_candidate(
            self.image.entry_point,
            config.CONFIDENCE_KNOWN,
            "entry_point"
        )

    def _pass_prologues(self, section: SectionInfo) -> None:
        """
        Pass 2: Scan for standard function prologues.

        Looks for: push ebp (0x55); mov ebp, esp (0x8BEC or 0x89E5)
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0
        while i < len(data) - 2:
            # Check for push ebp; mov ebp, esp
            if data[i] == 0x55:
                if (i + 2 < len(data) and
                        data[i + 1] == 0x8B and data[i + 2] == 0xEC):
                    addr = va_start + i
                    # Verify this address has a decoded instruction
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue"
                        )
                    i += 3
                    continue
                elif (i + 2 < len(data) and
                      data[i + 1] == 0x89 and data[i + 2] == 0xE5):
                    addr = va_start + i
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue_alt"
                        )
                    i += 3
                    continue
            i += 1

    def _pass_cc_boundaries(self, section: SectionInfo) -> None:
        """
        Pass 3: Find function boundaries at CC padding.

        Pattern: ret instruction, followed by one or more 0xCC bytes,
        followed by the start of the next function.
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0

        while i < len(data):
            # Look for CC padding runs
            if data[i] == config.CC_PADDING:
                cc_start = i
                while i < len(data) and data[i] == config.CC_PADDING:
                    i += 1

                cc_run_length = i - cc_start

                if cc_run_length >= config.MIN_CC_RUN and i < len(data):
                    # Check what the CC run interrupts.
                    #
                    # A `ret` is the obvious terminator, but a tail call ends a
                    # function just as completely: MSVC turns "return f(x)" into
                    # a bare `jmp f` and pads to the next boundary exactly as it
                    # would after a `ret`. Only accepting `ret` left the
                    # function before such a jmp running on through the padding
                    # and swallowing the next function whole -- and a vtable
                    # slot pointing into the middle of that merged range then
                    # has no function to resolve to, so the indirect call is
                    # skipped at runtime rather than made.
                    #
                    # The back-scan has to reach 5 bytes for `jmp rel32`; at 3
                    # it could not have seen one even if it had looked. Only
                    # instructions the sweep actually decoded at that address
                    # are considered, so this cannot invent a misaligned one.
                    before_addr = va_start + cc_start
                    found_ret = False
                    for check_offset in range(1, 8):
                        check_addr = before_addr - check_offset
                        insn = self.engine.get_instruction(check_addr)
                        if (insn and insn.end_address == before_addr
                                and (insn.is_ret or insn.is_jump)):
                            found_ret = True
                            break

                    if found_ret:
                        next_addr = va_start + i
                        if next_addr in self.engine.instructions:
                            self._add_candidate(
                                next_addr,
                                config.CONFIDENCE_CC_BOUNDARY,
                                "cc_boundary"
                            )
            else:
                i += 1

    def _pass_call_targets(self, sections: List[SectionInfo]) -> None:
        """
        Pass 4: Add destinations of direct call instructions as function starts.
        """
        # Build set of valid code ranges
        code_ranges = set()
        for sec in sections:
            for addr in range(sec.virtual_addr,
                              sec.virtual_addr + sec.virtual_size):
                code_ranges.add(addr)

        call_targets = self.engine.get_call_targets()
        realigned = unaligned = 0
        for target in call_targets:
            section = self.image.get_section_at_va(target)
            if not (section and section.executable):
                continue
            # A direct call to an executable address is the strongest evidence
            # of a function start there is -- stronger than a prologue match,
            # which is a guess about bytes. It used to be discarded whenever the
            # linear sweep had stepped over that exact address, which happens
            # wherever the sweep passes through data and comes out of phase.
            # Decode there instead of dropping the candidate; see
            # Engine.decode_at. On Halo 2276 this is 46 functions that were
            # becoming `g_esp += 4` no-op stubs.
            if target not in self.engine.instructions:
                # Manufacturing an instruction here is creating evidence, not
                # reading it, so require corroboration: the bytes there have to
                # decode as a function body, reaching a ret or a tail jump
                # without hitting something undecodable. A call operand read
                # out of data usually does not.
                #
                # Alignment used to be the corroboration. It is a weaker test
                # in both directions, and it silently dropped Wreckless's
                # _mtinitlocks at 0x000F211A -- a CRT function packed right
                # after a jump table at no alignment at all. Stubbed out, it
                # left the lock table all zeroes, and _lock(_LOCKTAB_LOCK)
                # recursed into _mtinitlocknum until the stack overflowed.
                # Targets that already decoded are untouched -- pre-existing
                # behaviour.
                if not self.engine.probes_as_function_body(target):
                    unaligned += 1
                    continue
                if self.engine.decode_at(target):
                    realigned += 1
                else:
                    continue  # genuinely undecodable: not code
            self._add_candidate(
                target,
                config.CONFIDENCE_CALL_TARGET,
                "call_target"
            )
        if realigned or unaligned:
            print(f"  Realigned {realigned} call targets the sweep stepped over"
                  f" ({unaligned} rejected: bytes do not decode as a function)")

    def _pass_indirect_call_slots(self) -> None:
        """
        Pass 4b: An immediate written into a memory slot that is elsewhere the
        operand of an indirect call is a function address.

        A function whose address is only ever taken -- never the operand of a
        direct call, never the target of a jump -- is invisible to every other
        pass. It gets no dispatch entry, and every indirect call to it lands on
        a target the recompiler cannot resolve.

        Only immediates stored into a slot something actually calls through
        count. That is the whole of the evidence: the value is provably used as
        a call target. Promoting every in-image immediate would drag in string
        and table addresses, and a spurious start inside a real function
        truncates it, which is worse than missing one.

        Wreckless: `mov dword ptr [0x1D1938], 0xF643B` installs the
        InitializeCriticalSectionAndSpinCount fallback thunk, which
        `call dword ptr [0x1D1938]` six instructions later invokes. Unresolved,
        the call returned 0, _mtinitlocks read that as failure and bailed after
        lock 0, and _lock(_LOCKTAB_LOCK) then recursed through _mtinitlocknum
        until the stack overflowed.
        """
        slots = set(self.engine.get_indirect_call_refs())
        if not slots:
            return
        found = 0
        for insn in list(self.engine.instructions.values()):
            if insn.memory_ref not in slots or insn.imm_ref is None:
                continue
            target = insn.imm_ref
            section = self.image.get_section_at_va(target)
            if not (section and section.executable):
                continue
            if target not in self.engine.instructions:
                if not self.engine.probes_as_function_body(target):
                    continue
                if not self.engine.decode_at(target):
                    continue
            self._add_candidate(
                target,
                config.CONFIDENCE_CALL_TARGET,
                "indirect_call_slot"
            )
            found += 1
        if found:
            print(f"  {found} function address(es) installed into"
                  f" indirect-call slots")

    def _pass_imm_ref_targets(self, sections: List[SectionInfo]) -> bool:
        """
        An immediate that points into unclaimed executable bytes and decodes as
        a whole function body is a function whose address was taken.

        _pass_indirect_call_slots already promotes an immediate stored into a
        slot that something calls through by name. That is the strong evidence,
        and it misses the ordinary C++ case entirely: a function pointer written
        into an object field and later invoked as `call dword ptr [reg]` has no
        fixed slot to match against.

        Half-Life 2's CUtlRBTree constructor does exactly that --
        `mov dword ptr [esi], 1D0F8h` installs the comparator, and the tree
        calls it through `this`. Both of its comparators sat in a 42-byte hole
        between two detected functions, so neither existed. Every tree search
        called an address the recompiler could not resolve, got 0 back, and
        descended into a node that was never there.

        The evidence here is weaker than a named slot, so the filter carries the
        weight, and it is the *gap* that does most of the work: the target has
        to land in executable bytes that no function already covers, and decode
        from there to a ret or a tail jump without hitting anything invalid. A
        string or table address fails the decode; an offset into a real function
        fails the gap. A spurious start inside a function would truncate it,
        which is worse than missing one -- hence testing coverage rather than
        just membership.

        Runs after _build_functions for that reason, and returns whether it
        added anything so the caller can rebuild.
        """
        bounds = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bounds]

        def inside_a_function(addr: int) -> bool:
            i = bisect.bisect_right(starts, addr) - 1
            return i >= 0 and addr < bounds[i][1]

        # Collect distinct targets first. The instruction dict holds millions of
        # entries and the same address is taken over and over, so probing per
        # instruction rather than per address is the difference between seconds
        # and not finishing.
        # Only the sections actually being analysed as code. An XBE marks
        # .rdata and .data executable, so `section.executable` alone lets a
        # string or a vtable through -- and .rdata disassembles happily into
        # bound/popal/arpl, which then fails to compile. The caller's section
        # list is the real answer to "is this code".
        code_ranges = [(sec.virtual_addr, sec.virtual_addr + sec.virtual_size)
                       for sec in sections]

        def in_code_section(addr: int) -> bool:
            return any(lo <= addr < hi for lo, hi in code_ranges)

        targets = set()
        for insn in self.engine.instructions.values():
            target = insn.imm_ref
            if target is None or target in self.functions:
                continue
            if inside_a_function(target) or not in_code_section(target):
                continue
            targets.add(target)

        found = 0
        for target in sorted(targets):
            # Apply the same realignment guard as explicit seeds. A numeric
            # immediate can point into a decoded instruction: JSRF's
            # 0x1600CA is the C9 byte of `test ecx, ecx`, which also decodes as
            # `leave`. Promoting it split QueryInterface and lost its stack
            # cleanup. Only a recognisable prologue can override the sweep.
            if (self.engine.instruction_covering(target) is not None
                    and not self.engine.probes_as_prologue(target)):
                continue
            # A ret, not merely a terminator: an immediate is weak evidence,
            # so the probe has to reject data that happens to disassemble. The
            # cap also keeps a wrong guess from walking the rest of the section.
            # ...or a virtual-call thunk, which never reaches a ret: it
            # dispatches through the vtable and is gone. Those are taken by
            # address and passed around as values, so an immediate is exactly
            # how they show up.
            if not (self.engine.probes_as_returning_body(target)
                    or self.engine.probes_as_vcall_thunk(target)):
                continue
            if target not in self.engine.instructions:
                if not self.engine.decode_at(target):
                    continue
            self._add_candidate(target, config.CONFIDENCE_IMM_REF,
                                "imm_ref_target")
            found += 1

        if found:
            print(f"  {found} function address(es) taken as an immediate")
        return found > 0

    def _pass_global_callback_targets(
            self, sections: List[SectionInfo]) -> int:
        """Discover code pointers installed in absolute indirect-call slots.

        This deliberately does not promote arbitrary immediates that happen to
        point into executable memory. A candidate needs one decoded ``mov``
        instruction with both sides represented in the existing xref graph:

        * a ``data_imm`` xref to the executable candidate, and
        * a ``data_read`` xref to an absolute memory slot,

        plus a later ``call`` xref through that exact slot. The instruction
        shape makes the memory operand the destination (an immediate cannot be
        a mov destination), while the matching indirect call proves the slot is
        used as a function pointer.

        Targets inside an already detected function body are not made ordinary
        candidates: splitting a known body is riskier than omitting a possible
        shared internal entry point. This pass handles standalone missed
        callbacks, the representation for which it has boundary evidence.
        """
        bodies = sorted((f.start, f.end) for f in self.functions.values())
        body_starts = [start for start, _end in bodies]
        added = 0

        for insn in self.engine.instructions.values():
            if (insn.mnemonic != "mov" or insn.memory_ref is None
                    or insn.imm_ref is None):
                continue

            refs = self.xrefs.get_refs_from(insn.address)
            has_slot_ref = any(
                ref.xref_type == XRefType.DATA_READ
                and ref.to_addr == insn.memory_ref
                for ref in refs)
            has_code_ref = any(
                ref.xref_type == XRefType.DATA_IMM
                and ref.to_addr == insn.imm_ref
                for ref in refs)
            if not (has_slot_ref and has_code_ref):
                continue

            # Require an indirect call through the same absolute slot after the
            # store. Kernel thunks are KERNEL_CALL xrefs and do not qualify.
            slot_is_called = any(
                ref.xref_type == XRefType.CALL
                and ref.from_addr > insn.address
                for ref in self.xrefs.get_refs_to(insn.memory_ref))
            if not slot_is_called:
                continue

            target = insn.imm_ref
            if target in self._candidates:
                continue
            section = self.image.get_section_at_va(target)
            if not (section and section.executable):
                continue

            # Do not split an existing body on dataflow evidence alone.
            body_index = bisect.bisect_right(body_starts, target) - 1
            if body_index >= 0:
                body_start, body_end = bodies[body_index]
                if body_start < target < body_end:
                    continue

            # The linear sweep can lose alignment after embedded data. The
            # store/call chain is strong enough evidence to decode at the exact
            # (possibly unaligned) target, unlike a bare immediate reference.
            if (target not in self.engine.instructions
                    and not self.engine.decode_at(target)):
                continue

            self._add_candidate(
                target,
                config.CONFIDENCE_GLOBAL_CALLBACK,
                "global_callback",
            )
            added += 1

        return added

    def _pass_tail_jump_targets(self, sections: List[SectionInfo]) -> bool:
        """
        Pass 6: Add the target of every unconditional jmp that leaves the body
        of the function containing it.

        Deliberately NOT conditional branches. A jcc is not a terminator, so a
        jcc target past the end of its own function almost always means the
        body was measured short, not that a function starts there -- and
        registering it splits the function instead of extending it. Tried on
        Halo 2276: +15 functions, but two of them landed inside an existing
        body and a third function lost 0xA0 bytes off its end.

                Returns True if any new candidate was added.
        """
        bodies = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bodies]
        added = False

        for insn in self.engine.instructions.values():
            if not insn.is_jump or insn.is_cond_jump:
                continue
            target = insn.jump_target
            if target is None or target in self._candidates:
                continue
            if target not in self.engine.instructions:
                continue

            # The jump is a tail jump only if it leaves its own function.
            i = bisect.bisect_right(starts, insn.address) - 1
            if i < 0:
                continue
            body_start, body_end = bodies[i]
            if insn.address >= body_end:
                continue            # not inside any known function
            if body_start <= target < body_end:
                continue            # ordinary intra-function branch

            section = self.image.get_section_at_va(target)
            if section is None or not section.executable:
                continue

            # Does the target land inside some *other* function? The CRT does
            # this constantly -- _startOneArgErrorHandling jumps into the middle
            # of _startTwoArgErrorHandling to share its tail. Registering that
            # address as an ordinary candidate would truncate the function it
            # lands in, breaking the very code it wanted to reach. Record it as
            # an alias entry instead: same end address, translated separately.
            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bodies[j][0] < target < bodies[j][1]:
                if target not in self._alias_entries:
                    self._alias_entries[target] = bodies[j][1]
                    added = True
                continue

            self._add_candidate(target, config.CONFIDENCE_TAIL_JUMP,
                                "tail_jump_target")
            added = True

        added = self._pass_cond_branch_orphans(bodies, starts) or added
        return added

    def _pass_data_ptr_targets(self, sections: List[SectionInfo]) -> bool:
        """
        A code pointer stored in a data section is a function whose address is
        only ever taken at run time.

        _pass_imm_ref_targets catches an address that appears as an immediate in
        code. It cannot catch one that only ever exists as a *value in a table*
        -- a vtable the compiler emitted into .rdata, a callback array, a
        dispatch table. RTTI recovery covers vtables belonging to polymorphic
        classes; nothing covers the rest.

        Half-Life 2 shows why that matters. Its boot reached module loading and
        then stopped on one unresolved indirect call after another --
        0x00427F80, 0x005AC780, 0x00581CC0 -- each a clean function reached
        through a table, each found only by running the title and reading the
        indirect-call feedback. There are 4,892 of them in this binary.

        Same filter as the immediate pass, and the gap does the same work: the
        target must land in executable bytes no function already covers, and
        must decode to a ret. A data word that happens to fall in a code
        section's range fails the decode; an offset into a real function fails
        the gap.
        """
        code_ranges = [(sec.virtual_addr, sec.virtual_addr + sec.virtual_size)
                       for sec in sections]
        code_names = {sec.name for sec in sections}

        def in_code_section(addr: int) -> bool:
            return any(lo <= addr < hi for lo, hi in code_ranges)

        bounds = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bounds]

        def inside_a_function(addr: int) -> bool:
            i = bisect.bisect_right(starts, addr) - 1
            return i >= 0 and addr < bounds[i][1]

        # Deliberately NOT filtering out targets inside a function. That guard
        # belongs to the immediate pass, which creates function *starts* and
        # would split whatever it landed in. This pass creates aliases, and
        # inside-a-function is precisely what an alias is for: an alternate
        # entry point sharing the enclosing body.
        #
        # It matters. MSVC merges runs of tiny C++ constructor thunks into one
        # body -- sub_005C10D6 covers 2,246 bytes of them -- so 45 of Half-Life
        # 2's static initialisers are addresses inside another function and
        # nowhere else. Excluding them left those constructors with no body at
        # all, and _initterm silently skipped every one.
        targets = set()
        for sec in self.image.sections:
            if sec.name in code_names:
                continue                    # scan data, not code
            data = self.image.get_section_data(sec)
            if not data:
                continue
            for off in range(0, len(data) - 3, 4):
                value = int.from_bytes(data[off:off + 4], "little")
                if in_code_section(value):
                    targets.add(value)

        # Alias entries, not candidates.
        #
        # Registering these as function starts measurably hurt: Half-Life 2
        # went from 5,305 constructors to 5,260 and from 1 clobbered
        # callee-saved register to 13, and the generated code shrank by 70%.
        # A start inside a gap does not truncate an already-measured body, but
        # it does stop its neighbour *extending* into that gap on the rebuild,
        # and plenty of bodies legitimately reach past their first measurement
        # via an out-of-line tail.
        #
        # An alias is the whole point: a callable entry that shares the
        # enclosing extent, built after every boundary is fixed, so it cannot
        # clamp anyone. Exactly what the tail-jump pass does for the same shape.
        section_end = {}
        for sec in sections:
            section_end[sec.name] = sec.virtual_addr + sec.virtual_size

        found = 0
        for target in sorted(targets):
            if target in self.functions or target in self._alias_entries:
                continue
            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bounds[j][0] < target < bounds[j][1]:
                # Inside a function, so the bytes are known to be code and the
                # only real question is whether the address is an instruction
                # boundary rather than the middle of one. Requiring a ret here
                # would be wrong: MSVC's constructor thunks are
                # `mov ecx, <this>; jmp <ctor>` and end in a tail jump, which
                # is exactly what the strict probe rejects. Share the enclosing
                # end, as the tail-jump pass does.
                if target not in self.engine.instructions:
                    continue
                end = bounds[j][1]
            else:
                # In a gap there is no enclosing body vouching for the bytes,
                # so require two things instead of one: the address is an
                # instruction boundary the sweep already found, and the stream
                # from it reaches a ret *or* a tail jump.
                #
                # Insisting on a ret was too strict. MSVC emits C++ constructor
                # thunks as `mov ecx, <this>; jmp <ctor>`, and a whole run of
                # them can sit between two detected functions -- 45 of Half-Life
                # 2's static initialisers are exactly that, in the gap after
                # sub_005C0FB0. Their `push .. call .. ret` siblings passed the
                # strict probe and they did not, which is a distinction with no
                # meaning: both are entry points named by the same table.
                #
                # Padding is what the boundary check buys: a run of int3 is not
                # an instruction the sweep records a start for, and a table does
                # not point into it anyway.
                if target not in self.engine.instructions:
                    continue
                first = self.engine.instructions[target]
                if first.mnemonic.lower() in ("int3", "nop"):
                    continue
                if not self.engine.probes_as_function_body(target,
                                                           max_insns=64):
                    continue
                i = bisect.bisect_right(starts, target)
                sec = self.image.get_section_at_va(target)
                end = starts[i] if i < len(starts) else section_end.get(
                    sec.name if sec else "", target + 4)
            if end <= target:
                continue
            self._alias_entries[target] = end
            found += 1

        if found:
            print(f"  {found} function address(es) found in data tables")
        return found > 0

    def _pass_cond_branch_orphans(self, bodies, starts) -> bool:
        """
        A conditional branch out of its function into unclaimed bytes.

        The pass above excludes jcc for a good reason -- a jcc target past the
        end of its own function usually means the body was measured short, and
        registering a function start there splits it instead of extending it.
        But the code still has to exist somewhere, and when it does not, the
        branch is lifted as a call to a stub that pops a return address and
        returns.

        That is not a small loss. MSVC parks an out-of-line block of realloc
        (sub_005B2C88) at 0x005B2D38 and reaches it with `jne`. Half-Life 2's
        realloc was truncated at 0x005B2D2A because a helper at 0x005B2D2F is
        separately called, so the block became orphaned and the branch went to
        a stub -- meaning the *common* path of realloc never ran its SEH
        epilogue. esp came back unrestored and ebx/esi/edi were never popped,
        so every CUtlMemory::Grow got a garbage buffer and every growable
        container in the game was quietly corrupt.

        Registering an alias rather than a candidate is what makes this safe:
        aliases are built after the bodies are measured, so they cannot clamp
        anyone's end, which is precisely the failure the jcc exclusion was
        protecting against. The target must also land in a gap -- inside
        another function is the alias case the pass above already handles --
        and must decode to a ret, so a mis-measured body's interior does not
        qualify on the strength of one branch.
        """
        added = False
        for insn in self.engine.instructions.values():
            if not insn.is_cond_jump:
                continue
            target = insn.jump_target
            if target is None or target in self._alias_entries:
                continue
            if target in self._candidates or target in self.functions:
                continue

            i = bisect.bisect_right(starts, insn.address) - 1
            if i < 0:
                continue
            body_start, body_end = bodies[i]
            if insn.address >= body_end:
                continue                    # not inside any known function
            if body_start <= target < body_end:
                continue                    # ordinary intra-function branch

            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bodies[j][0] <= target < bodies[j][1]:
                continue                    # inside a function: handled above

            section = self.image.get_section_at_va(target)
            if section is None or not section.executable:
                continue
            # A block that ends in an unconditional jmp instead of a ret is
            # the other half of this shape, and rejecting it left the branch
            # pointing at a stub just the same. Half-Life 2's _lock helper
            # (sub_005BE146) exits its scan loop into two such blocks at
            # 0x005BE244 and 0x005BE250; both were emitted as stubs that
            # returned without running the function's __finally, so _unlock
            # never ran. The CRT lock was taken 15 times and released none,
            # and the next thread to want it waited forever -- a level load
            # that deadlocked two thirds of the way in, with nothing in the
            # log to connect it to a missed function boundary.
            tail = self.engine.block_tail_jump(target)
            if not (self.engine.probes_as_returning_body(target, max_insns=256)
                    or self.engine.probes_as_vcall_thunk(target)
                    or tail is not None):
                continue

            # Run to the next known function start, or the section end.
            k = bisect.bisect_right(starts, target)
            end = starts[k] if k < len(starts) else (section.virtual_addr
                                                    + section.virtual_size)
            self._alias_entries[target] = end
            added = True

            # Where that jump lands has to be addressable too, or the block we
            # just recovered ends in a call to a stub and nothing is gained.
            # Landing inside a function is the ordinary case -- it is the rest
            # of the loop -- and the alias mechanism already exists for it.
            if tail is not None and tail not in self._alias_entries                     and tail not in self._candidates and tail not in self.functions:
                m = bisect.bisect_right(starts, tail) - 1
                if m >= 0 and bodies[m][0] < tail < bodies[m][1]:
                    self._alias_entries[tail] = bodies[m][1]

        if added:
            print("  conditional-branch orphans recovered as alias entries")
        return added

    def _build_alias_entries(self) -> None:
        """
        Emit a Function for each tail-jump target that lands inside another
        function, running from the target to that function's end.

        The overlap is deliberate: the translator produces a second body for the
        shared tail, which costs a little code size and makes the entry point
        callable. The alternative -- a stub that returns immediately -- silently
        skips the epilogue and leaks the caller's frame.
        """
        for addr, end in sorted(self._alias_entries.items()):
            if addr in self.functions:
                continue
            insns = self.engine.get_instructions_in_range(addr, end)
            if not insns:
                continue
            section = self.image.get_section_at_va(addr)
            sec_name = section.name if section else ""
            label = self.labels.get(addr)
            name = label.name if label else f"sub_{addr:08X}"
            if not label:
                self.labels.auto_name_function(
                    addr, sec_name, config.CONFIDENCE_TAIL_JUMP)
            self.functions[addr] = Function(
                start=addr,
                end=end,
                name=name,
                section=sec_name,
                confidence=config.CONFIDENCE_TAIL_JUMP,
                detection_method="tail_jump_alias",
                num_instructions=len(insns),
                has_prologue=False,
            )

    def _build_functions(self, sections: List[SectionInfo]) -> None:
        """
        Pass 5: Build Function objects from candidates.

        Determines function boundaries by finding the extent of each
        function (up to the next function start or unreachable point).
        """
        # Sort candidates by address
        sorted_starts = sorted(self._candidates.keys())
        if not sorted_starts:
            return

        # Build section boundary lookup
        sec_ranges = {}
        for sec in sections:
            sec_ranges[sec.name] = (sec.virtual_addr,
                                    sec.virtual_addr + sec.virtual_size)

        # Create functions
        for idx, start_addr in enumerate(sorted_starts):
            confidence, method = self._candidates[start_addr]

            # Determine section
            section = self.image.get_section_at_va(start_addr)
            sec_name = section.name if section else ""

            # Determine end address:
            # Walk instructions until we hit the next function start,
            # leave the section, or reach an unconditional terminator
            # with no fall-through.
            if idx + 1 < len(sorted_starts):
                next_func = sorted_starts[idx + 1]
            else:
                next_func = None

            # Section end boundary
            sec_end = None
            if section:
                sec_end = section.virtual_addr + section.virtual_size

            end_addr = self._find_function_end(start_addr, next_func, sec_end)

            # Count instructions
            insns = self.engine.get_instructions_in_range(start_addr, end_addr)
            num_insns = len(insns)

            if num_insns == 0:
                continue

            # Check for prologue
            first_insn = self.engine.get_instruction(start_addr)
            has_prologue = (first_insn is not None and
                            first_insn.mnemonic == "push" and
                            first_insn.op_str == "ebp")

            # Get or create name
            label = self.labels.get(start_addr)
            if label:
                name = label.name
            else:
                name = f"sub_{start_addr:08X}"
                self.labels.auto_name_function(
                    start_addr, sec_name, confidence)

            func = Function(
                start=start_addr,
                end=end_addr,
                name=name,
                section=sec_name,
                confidence=confidence,
                detection_method=method,
                num_instructions=num_insns,
                has_prologue=has_prologue,
            )
            self.functions[start_addr] = func

    def _find_function_end(self, start: int, next_func: Optional[int],
                           sec_end: Optional[int]) -> int:
        """
        Determine where a function ends.

        Walks forward from start, tracking the furthest reachable point
        through fall-through and internal jumps.
        """
        max_addr = start   # exclusive end of the code decoded so far
        max_target = start  # highest branch target that must be *inside* it
        addr = start

        # Upper bound
        upper = sec_end if sec_end else start + 0x100000
        if next_func and next_func < upper:
            upper = next_func

        while addr < upper:
            # An embedded switch table is data sitting on the fall-through
            # path. engine.resync_jump_tables() removed the instructions the
            # sweep hallucinated over it, so there is nothing to decode here --
            # step over the table and carry on with the real code after it.
            # Without this the function ends at its own switch and loses the
            # epilogue, which is how callers of memcpy lost esi/edi.
            tbl_end = self.engine.jump_tables.get(addr)
            if tbl_end is not None and tbl_end <= upper:
                if tbl_end > max_addr:
                    max_addr = tbl_end
                addr = tbl_end
                continue

            insn = self.engine.get_instruction(addr)
            if insn is None:
                break

            end = insn.end_address
            if end > max_addr:
                max_addr = end

            # Track internal forward jumps to extend function bounds.
            #
            # Unconditional jumps count too, not just conditional ones. A body
            # ending in "jmp <forward>" - the tail of an if/else, or a jump
            # over an interleaved block - otherwise hit the break below with
            # max_addr still short of the target, truncating the function
            # mid-body. Everything past the cut then looked like separate code,
            # and the function's own jump targets became calls to empty stubs.
            #
            # `upper` is already clamped to the next known function start, so a
            # target inside these bounds is internal rather than a tail call.
            # is_jump and is_cond_jump are mutually exclusive; is_branch is both.
            if insn.is_branch and insn.jump_target is not None:
                target = insn.jump_target
                if start <= target < upper and target > max_target:
                    # This jump goes forward within bounds, extend
                    max_target = target

            # A switch dispatch's case bodies are jump targets too, they are
            # just held in a table rather than encoded in the instruction.
            # Without them the terminator check below ends the function on the
            # dispatch itself, dropping every case and the shared epilogue --
            # MSVC's memcpy loses its "pop edi / pop esi", so every caller is
            # left with those registers clobbered.
            if insn.jump_table is not None:
                for target in self.engine.jump_table_entries(insn.jump_table):
                    if start <= target < upper and target > max_target:
                        max_target = target

            # A switch dispatch is an unconditional jump, so the terminator
            # check below would end the function on it -- but the table it
            # reads is parked immediately after, and the case bodies and the
            # epilogue follow that. Step over the table instead of stopping.
            if insn.is_jump and not insn.is_cond_jump:
                skipped = self._table_after(insn.end_address, upper)
                if skipped is not None:
                    if skipped > max_addr:
                        max_addr = skipped
                    addr = skipped
                    continue

            # MSVC ends noreturn bodies with INT3 and alignment NOPs. An
            # unpadded debug trap can resume into an epilogue, so retain that
            # distinction and the upstream forward-target coverage check.
            padded_trap = (insn.mnemonic == "int3"
                           and self._starts_nop_padding(insn.end_address,
                                                        upper))
            if (insn.is_ret or (insn.is_jump and not insn.is_cond_jump)
                    or padded_trap):
                # Stop only once we have decoded *past* every internal branch
                # target. A target is an address that must be inside the
                # function, so landing exactly on it is not coverage -- the
                # instruction there still has to be decoded. Using the target
                # as an exclusive end cut functions off at their own
                # out-of-line tail: MSVC routinely emits "jmp <backward>" and
                # then parks a conditional branch's target after it. Halo's
                # get_edge_vertex ended at the branch target, so the tail was
                # lifted as a separate function and the jump to it became a
                # tail call that returned without running the epilogue --
                # leaking the whole 28-byte frame on every call.
                if insn.end_address > max_target:
                    break
                # There might be more code after (jumped over)
                addr = insn.end_address
                continue

            addr = insn.end_address

        return max_addr

    def _starts_nop_padding(self, addr: int, upper: int) -> bool:
        """Return true for at least two consecutive bytes of decoded NOPs."""
        padding_bytes = 0
        while addr < upper and padding_bytes < 16:
            insn = self.engine.get_instruction(addr)
            if insn is None or not insn.is_nop:
                break
            padding_bytes += insn.size
            addr = insn.end_address
        return padding_bytes >= 2

    def _table_after(self, addr: int, upper: int) -> Optional[int]:
        """End of an embedded jump table starting at or just after `addr`.

        MSVC usually parks the table directly after the dispatching jump, but
        it may align first, so allow a few padding bytes.
        """
        for start in range(addr, addr + 16):
            end = self.engine.jump_tables.get(start)
            if end is not None and end <= upper:
                return end
        return None

    def _build_call_graph(self) -> None:
        """Populate calls_to and called_by for all functions."""
        func_starts = set(self.functions.keys())

        for func in self.functions.values():
            insns = self.engine.get_instructions_in_range(func.start, func.end)
            callees = set()
            for insn in insns:
                if insn.call_target is not None:
                    callees.add(insn.call_target)

            func.calls_to = sorted(callees)

            for callee_addr in callees:
                callee = self.functions.get(callee_addr)
                if callee is not None:
                    callee.called_by.append(func.start)

        # Sort called_by lists
        for func in self.functions.values():
            func.called_by = sorted(set(func.called_by))

    def get_function_at(self, addr: int) -> Optional[Function]:
        """Get the function containing an address."""
        # First check direct match
        if addr in self.functions:
            return self.functions[addr]
        # Search for containing function
        for func in self.functions.values():
            if func.start <= addr < func.end:
                return func
        return None

    def get_functions_in_section(self, section_name: str) -> List[Function]:
        """Get all functions in a section, sorted by address."""
        return sorted(
            [f for f in self.functions.values() if f.section == section_name],
            key=lambda f: f.start
        )

    def summary(self) -> dict:
        """Return summary statistics."""
        by_method: Dict[str, int] = {}
        by_section: Dict[str, int] = {}
        total_insns = 0
        with_prologue = 0

        for func in self.functions.values():
            by_method[func.detection_method] = by_method.get(
                func.detection_method, 0) + 1
            by_section[func.section] = by_section.get(func.section, 0) + 1
            total_insns += func.num_instructions
            if func.has_prologue:
                with_prologue += 1

        return {
            "total_functions": len(self.functions),
            "total_instructions_in_functions": total_insns,
            "with_prologue": with_prologue,
            "by_detection_method": by_method,
            "by_section": by_section,
        }
