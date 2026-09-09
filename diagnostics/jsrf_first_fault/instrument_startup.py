"""Add/remove opt-in read-only probes in an existing generated checkout.

Does not regenerate guest code or change branches, registers, or guest memory.
"""
import argparse
import re
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--remove', action='store_true')
p.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2] / 'build-macos/jsrf-first-fault/gen')
a = p.parse_args()
# Generated file -> pc -> (probe function, argument expression).
#
# jsrf_startup_probe sites walk the root object and the update list.
# jsrf_interp_probe sites bracket one call of the colour interpolator
# sub_00024700 (entry 0x24700, single exit 0x24962) and record every call into
# its two setters, 0x24480 (current colour) and 0x24540 (target and step).
# jsrf_resource_probe follows the title's D3D allocation/release vector, the
# canonical resource Release helper, and the destructor's actual free paths.
# jsrf_usb_device_probe brackets the root-hub connect path's device-object
# allocation and observes whether the freshly allocated object is linked.
# Each site reads registers only; the probe reads guest memory read-only.
points = {
    'recomp_0000.c': {
        # Scene-tree lifetime around the post-BGM crash. sub_00012020 links a
        # freshly constructed node, sub_00011000 unlinks it, and the 0x11D24
        # traversal reads the child pointer that is later found corrupted.
        '00011000': ('jsrf_tree_probe', 'ecx, 0'),
        '00011070': ('jsrf_tree_probe', 'ecx, 0'),
        '00011077': ('jsrf_tree_probe', 'esi, 0'),
        '0001108A': ('jsrf_tree_probe', 'esi, MEM32(esi + 0x28)'),
        '00011096': ('jsrf_tree_probe', 'esi, MEM32(esi + 0x30)'),
        '0001109D': ('jsrf_tree_probe', 'MEM32(esp), esp'),
        '00011B90': ('jsrf_tree_probe', 'MEM32(esp + 4), ecx'),
        '00011D24': ('jsrf_tree_probe', 'edi, MEM32(edi + 0x28)'),
        '00011D63': ('jsrf_tree_probe', 'edi, 0'),
        '00011D30': ('jsrf_tree_probe', 'esi, 0'),
        '00011BB2': ('jsrf_tree_probe', 'esi, 0'),
        '00012020': ('jsrf_tree_probe', 'ecx, MEM32(esp + 4)'),
        '00012100': ('jsrf_tree_probe', 'ecx, MEM32(esp + 4)'),
        # The array-remove shift loop that was measured writing into the
        # kernel import thunk table. At the entry nothing is pushed, so the
        # return address is at esp and the index argument at esp+4.
        '000147A0': ('jsrf_list_remove_probe',
                     'ecx, MEM32(esp + 4), MEM32(esp)'),
        '00013A80': ('jsrf_startup_probe', 'ecx'),
        # These are the only small setters for the root object's transient
        # trigger fields.  Record who calls them and which object receives the
        # write; observation happens before the generated body changes RAM.
        '000126D0': ('jsrf_trigger_probe', 'ecx, MEM32(esp + 4)'),
        '000126F0': ('jsrf_trigger_probe', 'ecx, MEM32(esp + 4)'),
        '00012710': ('jsrf_trigger_probe', 'ecx, MEM32(esp + 4)'),
        '00012730': ('jsrf_trigger_probe', 'ecx, MEM32(esp + 4)'),
        '00012750': ('jsrf_trigger_probe', 'ecx, 0'),
        '00012760': ('jsrf_trigger_probe', 'ecx, 0'),
        '00012770': ('jsrf_trigger_probe', 'ecx, 0'),
        '00011083': ('jsrf_startup_probe', 'esi'),
        '00013220': ('jsrf_startup_probe', 'MEM32(esi + 0x87EC)'),
        '00025DD0': ('jsrf_startup_probe', 'MEM32(esp + 4)'),
        '00024700': ('jsrf_interp_probe', 'ecx'),
        '00024711': ('jsrf_interp_path_probe', 'esi'),
        '0002472C': ('jsrf_interp_path_probe', 'esi'),
        '00024731': ('jsrf_interp_path_probe', 'esi'),
        '00024740': ('jsrf_interp_path_probe', 'esi'),
        '00024757': ('jsrf_interp_path_probe', 'esi'),
        '00024760': ('jsrf_interp_path_probe', 'esi'),
        '0002476F': ('jsrf_interp_path_probe', 'esi'),
        '00024786': ('jsrf_interp_path_probe', 'esi'),
        '00024945': ('jsrf_interp_path_probe', 'esi'),
        '00024962': ('jsrf_interp_probe', 'esi'),
        '00024480': ('jsrf_interp_probe', 'ecx'),
        '00024540': ('jsrf_interp_probe', 'ecx'),
    },
    'recomp_0001.c': {
        # The title screen's state dispatcher. ecx is the object and its
        # +0x44 the index into the 21-entry jump table at 0x001FA008.
        '0004EF90': ('jsrf_title_state_probe', 'ecx, MEM32(ecx + 0x44)'),
    },
    'recomp_0009.c': {
        # 0x001A308E is a manual override now. Its original generated body was
        # an unbounded DSOUND completion spin, so there is no generated label
        # at which a read-only probe can be installed.
        '001914BD': ('jsrf_pb_patch_probe', 'esi'),
        '00191510': ('jsrf_pb_event_probe', 'esi'),
        '001915FD': ('jsrf_pb_reserve_probe', 'esi, eax, ecx, MEM32(esp + 0xC)'),
        # JSRF's vblank acknowledge spin. ebx is the NV2A register base, ecx
        # the value about to be written to PCRTC_INTR_0. Reads the summary and
        # the source as the guest sees them, before its own store.
        # D3D's notification dispatcher. At the entry nothing is pushed,
        # so the return address is at esp and the index at esp+4.
        '00193F70': ('jsrf_notify_probe',
                     'ecx, MEM32(esp + 4), MEM32(esp)'),
        '00193E40': ('jsrf_vblank_ack_probe',
                     'ebx, MEM32(ebx + 0x100), MEM32(ebx + 0x600100), ecx'),
        # The D3D pushbuffer free-space spin. edx holds the pointer the loop
        # dereferences for the GPU's GET position; edi is PUT and eax the space
        # being waited for. Read-only, and self-limiting inside the probe.
        '001914F0': ('jsrf_pushbuffer_wait_probe',
                     'edx, MEM32(edx), edi, eax'),
        # Main 0x1C40F8 allocation site: header storage, returned address,
        # and requested byte count (computed by sub_00193380 in edi).
        '0018E6E9': ('jsrf_resource_probe', 'esi, eax, edi'),
        # The one call through sibling slot 0x1C40FC, before and after.
        '00191A80': ('jsrf_resource_probe', 'ecx, 0, 0'),
        '00191A8B': ('jsrf_resource_probe', 'esi, eax, 0'),
        # Canonical resource Release: entry, last-reference path, actual
        # destructor call, and ordinary decrement path.
        '00192990': ('jsrf_resource_probe', 'MEM32(esp + 4), 0, 0'),
        '001929A4': ('jsrf_resource_probe', 'esi, eax, 0'),
        '001929C6': ('jsrf_resource_probe', 'esi, MEM32(esi), 0'),
        '001929D2': ('jsrf_resource_probe', 'esi, eax, 0'),
        # Release one binding/lock reference. This is the other way a
        # resource whose ordinary reference count reached zero is destroyed.
        '00192A10': ('jsrf_resource_probe', 'MEM32(esp + 4), 0, 0'),
        '00192A5D': ('jsrf_resource_probe', 'esi, MEM32(esi), 0'),
        '00192A67': ('jsrf_resource_probe', 'esi, eax, 0'),
        # Destructor entry and each path which actually calls the contiguous
        # free vector. The entry's stack return address identifies its caller.
        '00192830': ('jsrf_resource_probe', 'MEM32(esp + 4), MEM32(esp), 0'),
        '00192865': ('jsrf_resource_probe', 'edi, MEM32(edi + 4) | 0x80000000u, 0'),
        '0019288C': ('jsrf_resource_probe', 'edi, MEM32(edi + 4), 0'),
        '001928B1': ('jsrf_resource_probe', 'edi, MEM32(edi + 4) | 0x80000000u, 0'),
    },
    'recomp_0007.c': {
        # Texture-cache lifetime. 0x14F640 releases one indexed slot; the two
        # stores in the shared 0x14F720 body publish the newly created resource.
        # Observe the old slot before each write so replacements cannot hide.
        '0014F640': ('jsrf_texture_cache_probe',
                     'MEM32(esp + 4), 0, MEM32(MEM32(0x264F68) + MEM32(esp + 4) * 4)'),
        '0014F801': ('jsrf_texture_cache_probe',
                     'MEM32(esp + 0x3C), MEM32(esp + 0x40), MEM32(MEM32(0x264F68) + MEM32(esp + 0x3C) * 4)'),
        '0014FA19': ('jsrf_texture_cache_probe',
                     'MEM32(esp + 0x3C), edx, MEM32(MEM32(0x264F68) + MEM32(esp + 0x3C) * 4)'),
        # Cache owner teardown: if this never runs, retained slots are still
        # owned rather than lost through a resource-release branch.
        '00154B20': ('jsrf_texture_cache_probe',
                     '0xFFFFFFFFu, MEM32(0x264F70), MEM32(0x264F68)'),
    },
    'recomp_0002.c': {
        # Error-dialog constructor. The return address identifies which title
        # state requested the dialog; the first four arguments identify the
        # selected message/flags without modifying the path.
        '0006F450': ('jsrf_error_dialog_probe',
                     'MEM32(esp), MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 0xC)'),
    },
    'recomp_0006.c': {
        '00144F60': ('jsrf_adx_decode_probe', 'esp'),
        # Both exits of the cache index walk sub_00143540: 0x001435C4 returns
        # zero (miss) and 0x001435CC returns the matched entry in ebp. Five
        # registers are still pushed at each, so the query path is at esp+0x18.
        '001435C4': ('jsrf_cache_lookup_probe', 'MEM32(esp + 0x18), 0'),
        '001435CC': ('jsrf_cache_lookup_probe', 'MEM32(esp + 0x18), ebp'),
        # wxCiReqRd's entry. At the first instruction nothing is pushed yet,
        # so the arguments are at esp+4 onward: handle, buffer, sector count.
        '001403B0': ('jsrf_read_request_probe',
                     'MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 0xC)'),
        # Every CRI middleware diagnostic funnels through sub_0013C890, which
        # copies the message into 0x0027BC20 and forwards it to a hook at
        # 0x002616D8 that JSRF never installs -- so cvFsOpen's six failure
        # messages, cvFsAddDev's three and the rest reach a buffer and stop.
        # One probe here reads them all, a level above the WXCI reporter and
        # covering it too: the WXCI path arrives via 0x0013D840 -> 0x0013AF10.
        '0013C890': ('jsrf_wxci_error_probe',
                     'MEM32(esp + 4), 0, MEM32(esp)'),
        # The WXCI/XB disc driver's error reporter. It forwards to whatever
        # callback the title installed at 0x002615C4, and JSRF installs none --
        # so every one of its twelve diagnostics ("read error occurs",
        # "nsct < 0", "Timeout. (Waiting for transmission)", the three
        # parameter checks in wxCiReqRd) has been discarded for the whole
        # project. The driver has been describing its own failures all along.
        # Arguments are (arg, message) at [esp+4] and [esp+8].
        '00140190': ('jsrf_wxci_error_probe',
                     'MEM32(esp + 8), MEM32(esp + 4), MEM32(esp)'),
        # CRI's installed-handler dispatcher:
        #   mov eax,[0x2615E8] / test eax,eax / jz ret
        #   mov ecx,[0x2615EC] / push ecx / call eax
        # The handler it reaches is 0x0013F900, which is `eb fe` -- `jmp $`,
        # the halt stub CRI installs for a condition it does not expect to
        # survive. RECOMP_ICALL cannot resolve that address (it is not a
        # detected entry point), so the call is skipped and the run continues
        # past a point the middleware intended to stop at. It fires once, and
        # immediately before the first fault after the title screen advances.
        # The return address is the only thing that says which CRI path
        # decided to halt; the two globals are the handler and its argument.
        '00141B60': ('jsrf_cri_handler_probe',
                     'MEM32(0x2615E8), MEM32(0x2615EC), MEM32(esp)'),
        # The CRI ring-buffer class (vtable 0x0022DB38) acquire and commit.
        # 0x0013F9E0 hands out a block from one of the two views and 0x0013FBC0
        # gives it back; the ADX input buffer at 0x00277180 gets an acquire that
        # covers its whole capacity and never a matching commit. Both are
        # function entries, so the arguments are at esp+4 onward:
        # (this, view, size) and (this, view, block).
        '0013F9E0': ('jsrf_ringbuf_probe',
                     'MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 0xC)'),
        '0013FBC0': ('jsrf_ringbuf_probe',
                     'MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 0xC)'),
        # The ADXF read server's entry. Its one argument is the table entry
        # whose state bytes decide whether its caller keeps servicing it.
        '0013C070': ('jsrf_adxf_probe', 'MEM32(esp + 4)'),
        # Follow the WXCI request server around the status-2 completion path.
        # The title's request is the first 0x150-byte entry at 0x00273780.
        # Entry, return from issuing the native read, completion-flag poll,
        # and the block that publishes status 1 are enough to distinguish a
        # missing callback from a control-flow failure after that callback.
        '00140BA0': ('jsrf_wxci_request_probe', 'MEM32(esp + 4)'),
        '00140BDD': ('jsrf_wxci_request_probe', 'esi'),
        '00140C05': ('jsrf_wxci_request_probe', 'esi'),
        '00140C0F': ('jsrf_wxci_request_probe', 'esi'),
    },
    'recomp_0010.c': {
        # DSOUND completion points moved into this generated partition after
        # 0x001A308E became a manual function.
        '001A2FBE': ('jsrf_audio_completion_probe', 'esi, 0'),
        '001A25CA': ('jsrf_audio_completion_probe', 'edi, esi'),
        '001A3A8E': ('jsrf_audio_completion_probe', 'esi, 0'),
        # Root-hub connect path: entry, device-pool allocation result, and the
        # link helper reached only when that result is non-zero.
        '001BF72C': ('jsrf_usb_device_probe',
                     'ecx, 0, MEM32(esp + 4), MEM32(esp + 8)'),
        '001BF73B': ('jsrf_usb_device_probe',
                     'edi, eax, MEM32(esp + 0x10), MEM32(esp + 0x14)'),
        '001C06B3': ('jsrf_usb_device_probe',
                     'ecx, MEM32(esp + 4), 0, 0'),
    },
}
changed = 0
multi_labels = {
    ('recomp_0007.c', '0014F801'),
    ('recomp_0007.c', '0014FA19'),
}
for filename, file_points in points.items():
    f = a.gen / filename
    s = f.read_text()
    for pc, (func, args) in file_points.items():
        label = f'loc_{pc}: ;'
        call = f'\n    {func}(0x{pc}u, {args}); /* STARTUP_OBSERVATION */'
        expected = 2 if (filename, pc) in multi_labels else 1
        assert s.count(label) == expected, \
            f'expected {expected} copies of {label} in {f}, found {s.count(label)}'
        s = s.replace(label + call, label)
        if not a.remove:
            s = s.replace(label, label + call)
        changed += 1
    f.write_text(s)

# Instrument the branch itself for every dead `_flags` fallback. Function-level
# reachability is insufficient: many recovered functions contain cold data that
# was conservatively decoded after an unconditional tail jump. This records
# only a fallback conditional that actually executes, once per site and without
# changing its value or destination.
marker = '/* UNRESOLVED_FLAG_OBSERVATION */'
func_re = re.compile(r'^void sub_([0-9A-Fa-f]+)\(void\)')
cond_re = re.compile(r'/\* (\w+):')
unresolved_sites = 0
unresolved_files = 0
for f in sorted(a.gen.glob('recomp_*.c')):
    original = f.read_text().splitlines()
    lines = [line for line in original if marker not in line]
    output = []
    function = None
    flags_written = False
    file_changed = len(lines) != len(original)
    for lineno, line in enumerate(lines, 1):
        m = func_re.match(line)
        if m:
            function = int(m.group(1), 16)
            flags_written = False
        if '_flags = ' in line and '_flags = 0;' not in line:
            flags_written = True
        if 'if (_flags' in line and not flags_written:
            condition = cond_re.search(line)
            if not a.remove:
                output.append(
                    f'    jsrf_unresolved_flag_probe(0x{function:08X}u, '
                    f'{unresolved_sites}u); {marker} '
                    f'/* {f.name}:{lineno} '
                    f'{condition.group(1) if condition else "?"} */')
                file_changed = True
            unresolved_sites += 1
        output.append(line)
    if file_changed:
        f.write_text('\n'.join(output) + '\n')
        unresolved_files += 1

action = 'Removed' if a.remove else 'Installed'
print(f'{action} {changed} fixed observation sites in {len(points)} generated files')
print(f'{action} {unresolved_sites} unresolved-flag execution sites in '
      f'{unresolved_files} generated files')
