"""Add/remove opt-in read-only probes in an existing generated checkout.

Does not regenerate guest code or change branches, registers, or guest memory.
"""
import argparse
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
        '00013A80': ('jsrf_startup_probe', 'ecx'),
        '00011083': ('jsrf_startup_probe', 'esi'),
        '00013220': ('jsrf_startup_probe', 'MEM32(esi + 0x87EC)'),
        '00025DD0': ('jsrf_startup_probe', 'MEM32(esp + 4)'),
        '00024700': ('jsrf_interp_probe', 'ecx'),
        '00024962': ('jsrf_interp_probe', 'esi'),
        '00024480': ('jsrf_interp_probe', 'ecx'),
        '00024540': ('jsrf_interp_probe', 'ecx'),
    },
    'recomp_0009.c': {
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
    'recomp_0010.c': {
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
print(f'{"Removed" if a.remove else "Installed"} {changed} observation sites in {len(points)} generated files')
