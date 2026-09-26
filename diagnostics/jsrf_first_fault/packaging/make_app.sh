#!/bin/sh
# Assemble a double-clickable JSRF.app from a built jsrf_first_fault.
#
# WHY A SCRIPT AND NOT CMake's MACOSX_BUNDLE. The bundle's hard part is not the
# directory layout, it is the dylibs: this binary links three Homebrew
# libraries by absolute path, and one of them loads a fourth at RUNTIME that
# otool never mentions. MACOSX_BUNDLE does none of that, so the bundle it
# produces launches only on a machine that already has Homebrew and the same
# formulae installed -- which looks like it works, on the machine that built it.
#
# THE ONE THAT BITES: /opt/homebrew/opt/sdl2-compat is a shim over SDL3. Its
# LC_LOAD_DYLIB list contains no SDL3 at all; it dlopens it, searching
# "@loader_path/libSDL3.dylib", then "@executable_path/libSDL3.dylib", then the
# bare name. So SDL3 has to sit BESIDE libSDL2 in Frameworks/ or the app dies
# at SDL_Init with nothing in the log but a dlopen failure. Verified with
# `strings` on the shim, not assumed.
#
# Usage:
#   packaging/make_app.sh [<built binary>] [<output dir>]
# Defaults to build-macos/jsrf-first-fault/build-feav/jsrf_first_fault and
# build-macos/.
set -eu

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BIN=${1:-$ROOT/build-macos/jsrf-first-fault/build-feav/jsrf_first_fault}
OUTDIR=${2:-$ROOT/build-macos}
APP=$OUTDIR/JSRF.app
ID=net.xboxrecomp.jsrf

[ -x "$BIN" ] || { echo "no binary at $BIN -- build first" >&2; exit 1; }

echo "  binary:  $BIN"
echo "  bundle:  $APP"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"

# NOT "jsrf". CFBundleExecutable is "JSRF", and the default macOS filesystem is
# CASE-INSENSITIVE -- MacOS/JSRF and MacOS/jsrf are one file, so writing the
# launcher silently overwrote the engine and left a 1.3 KB shell script where
# the Mach-O had been. otool then said "is not an object file" and every
# install_name_tool call quietly did nothing. The two names must differ by more
# than case.
cp "$BIN" "$APP/Contents/MacOS/jsrf-engine"
chmod +x "$APP/Contents/MacOS/jsrf-engine"

# --- dylibs -----------------------------------------------------------------
# Walk the binary's own list, then each copied library's list, until nothing
# new appears. A fixed list would have missed libSDL3 even if it were in the
# LC_LOAD_DYLIB tables, and it is not.
copy_deps() {
    _target=$1
    otool -L "$_target" | tail -n +2 | awk '{print $1}' \
        | grep -v '^/usr/lib' | grep -v '^/System' | grep -v '^@' \
    | while read -r dep; do
        base=$(basename "$dep")
        [ -f "$APP/Contents/Frameworks/$base" ] && continue
        [ -f "$dep" ] || { echo "  MISSING $dep" >&2; continue; }
        echo "  + $base"
        cp "$dep" "$APP/Contents/Frameworks/$base"
        chmod u+w "$APP/Contents/Frameworks/$base"
        copy_deps "$APP/Contents/Frameworks/$base"
    done
}
echo "bundling libraries:"
copy_deps "$APP/Contents/MacOS/jsrf-engine"

# The runtime dependency otool cannot see. Its absence is not a warning, it is
# a bundle that launches to a black screen, so fail loudly instead.
if [ -f "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib" ]; then
    if strings "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib" | grep -q 'libSDL3'; then
        sdl3=$(ls /opt/homebrew/opt/sdl3/lib/libSDL3.dylib \
                  /opt/homebrew/lib/libSDL3.dylib 2>/dev/null | head -1 || true)
        [ -n "$sdl3" ] || { echo "sdl2-compat needs libSDL3 and it was not found" >&2; exit 1; }
        echo "  + libSDL3.dylib (dlopened by sdl2-compat)"
        cp "$(readlink -f "$sdl3" 2>/dev/null || echo "$sdl3")" \
           "$APP/Contents/Frameworks/libSDL3.dylib"
        chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
        copy_deps "$APP/Contents/Frameworks/libSDL3.dylib"
        # sdl2-compat looks beside itself first, so this is already right --
        # but only if its own dependents resolve too.
        for extra in "$APP"/Contents/Frameworks/libSDL3.*.dylib; do
            [ -e "$extra" ] || continue
            [ "$extra" = "$APP/Contents/Frameworks/libSDL3.dylib" ] && continue
            install_name_tool -id "@rpath/$(basename "$extra")" "$extra" 2>/dev/null || true
        done
    fi
fi

# --- rewrite every absolute path to @rpath ----------------------------------
echo "rewriting install names:"
for lib in "$APP"/Contents/Frameworks/*.dylib; do
    [ -e "$lib" ] || continue
    install_name_tool -id "@rpath/$(basename "$lib")" "$lib"
done
for target in "$APP/Contents/MacOS/jsrf-engine" "$APP"/Contents/Frameworks/*.dylib; do
    [ -e "$target" ] || continue
    otool -L "$target" | tail -n +2 | awk '{print $1}' \
        | grep -v '^/usr/lib' | grep -v '^/System' | grep -v '^@' \
    | while read -r dep; do
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$target" 2>/dev/null || true
    done
done
# STRIP THE BUILD MACHINE'S RPATHS FIRST. CMake bakes in the Cellar and
# /opt/homebrew/lib, and dyld searches LC_RPATH in order -- so with those left
# in place a bundle on THIS machine loads Homebrew's copies and the bundled
# ones are never touched. It works, and it proves nothing: the first machine
# without Homebrew is where you find out. Remove them so the bundle can only
# resolve against what it carries.
otool -l "$APP/Contents/MacOS/jsrf-engine" | awk '/LC_RPATH/{f=1} f&&/ path /{print $2; f=0}' \
| while read -r rp; do
    case "$rp" in
        @*) ;;
        *) install_name_tool -delete_rpath "$rp" "$APP/Contents/MacOS/jsrf-engine" 2>/dev/null || true ;;
    esac
done
install_name_tool -add_rpath "@executable_path/../Frameworks" \
    "$APP/Contents/MacOS/jsrf-engine" 2>/dev/null || true
# PROVE IT. Every failure above is silent: install_name_tool warns and exits 0,
# and a bundle with one absolute Homebrew path left in it runs perfectly on
# this machine and nowhere else. So check, and fail if anything is left.
left=$(otool -L "$APP/Contents/MacOS/jsrf-engine" | tail -n +2 | awk '{print $1}' \
       | grep -v '^/usr/lib' | grep -v '^/System' | grep -v '^@' || true)
if [ -n "$left" ]; then
    echo "  absolute paths still in the binary:" >&2
    echo "$left" | sed 's/^/    /' >&2
    exit 1
fi
if ! otool -l "$APP/Contents/MacOS/jsrf-engine" | grep -q LC_RPATH; then
    echo "  no LC_RPATH on the engine -- @rpath will not resolve" >&2
    exit 1
fi
echo "  done, no absolute paths left"

# --- launcher ---------------------------------------------------------------
# A double-clicked app inherits no shell environment, so the game dump and the
# emulated HDD cannot come from RECOMP_GAME_DIR the way every script here does
# it. They also cannot be bundled: they are the user's own copy of a retail
# disc and an emulated drive, and they do not belong inside an app. So the
# launcher reads a config file, and says exactly what to do when it is missing
# instead of exiting silently -- which, from a double-click, is indistinguishable
# from the app being broken.
cat > "$APP/Contents/MacOS/JSRF" <<'LAUNCH'
#!/bin/sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SUPPORT="$HOME/Library/Application Support/JSRF"
CONF="$SUPPORT/paths.conf"
LOG="$SUPPORT/last-run.log"
mkdir -p "$SUPPORT"

# ROTATE BEFORE TRUNCATING. This log is the only record of what the engine saw,
# and the launcher used to overwrite it on every start -- so the way to destroy
# a crash report was to do the natural thing after a crash, which is launch the
# game again. It cost the 08:11 crash on 19 Sep 2026: the wild-pointer
# instrument was armed and had run, and its output went under a relaunch two
# minutes later, leaving only the macOS .ips to work from.
#
# A log that recorded a guest fault is kept for good under its own timestamp.
# Everything else moves to last-run-previous.log, so even an ordinary session
# survives exactly one relaunch -- which is the case where someone quits, then
# realises they wanted the log.
if [ -f "$LOG" ]; then
    if grep -qa 'FIRST GUEST FAULT' "$LOG" 2>/dev/null; then
        mv "$LOG" "$SUPPORT/last-run-$(date -r "$LOG" +%Y-%m-%d_%H%M%S)-CRASH.log"
    else
        mv "$LOG" "$SUPPORT/last-run-previous.log"
    fi
fi

die() {
    osascript -e "display alert \"Jet Set Radio Future\" message \"$1\" as critical" \
        >/dev/null 2>&1 || true
    echo "$1" >&2
    exit 1
}

if [ -f "$CONF" ]; then . "$CONF"; fi
#@LIFT_DEFAULTS@

probe_readable() {
    # READ A BYTE, do not just stat. A GUI-launched app has its own TCC
    # identity -- not the Terminal's -- so a game dump in iCloud Drive, Desktop,
    # Documents or Downloads needs the user's consent before the first read.
    # `test -f` succeeds without it and the ENGINE then blocks forever inside
    # fopen() with nothing on screen. Measured: the same binary that runs from a
    # terminal sat at 0.0% CPU for two minutes in __open_nocancel when opened
    # from Finder. Triggering the read here means the prompt arrives while this
    # script can still say something useful about it.
    dd if="$1" bs=1 count=1 >/dev/null 2>&1
}

if [ -z "${JSRF_GAME_DIR:-}" ] || [ ! -f "${JSRF_GAME_DIR:-}/default.xbe" ]; then
    cat > "$CONF.example" <<'EX'
# Where your Jet Set Radio Future files live. Copy this to paths.conf and edit.
# JSRF_GAME_DIR must contain default.xbe.
JSRF_GAME_DIR="/path/to/Jet Set Radio Future (US)"
# A writable copy of an emulated Xbox HDD tree.
JSRF_HDD_ROOT="$HOME/Library/Application Support/JSRF/hdd"
#
# Anything else you export here reaches the runtime, so this is also where you
# try a switch without a rebuild: export it, relaunch, then read last-run.log.
# Two worth knowing about:
#
#   export RECOMP_APU_CYCLE_BREAK=1  # stop the voice-list walk revisiting a
#                                    # voice it has already rendered this
#                                    # subframe. Candidate fix for audio that
#                                    # glitches or plays at the wrong pitch in
#                                    # gameplay. OFF by default: the last APU
#                                    # guard shipped on a good argument made
#                                    # the crash worse, so this one waits for
#                                    # a run count rather than an argument.
#   export RECOMP_REPORT_MS=5000     # counter reports twice as often
#
# RECORDING WHAT YOU PLAYED, so a crash can be reproduced without you:
#
#   export RECOMP_PAD_RECORD=1
#
# Every session then writes one file to
#   ~/Library/Application Support/JSRF/padrec/jsrf-<date>_<time>-<pid>.padrec
# holding your controller input keyed to the game's own frame count. It costs
# well under a microsecond per pad poll and writes nothing while you are not
# touching the pad. Look for "[PAD-RECORD] recording to ..." near the top of
# last-run.log; if it says CANNOT WRITE instead, nothing was recorded and the
# line says why.
#
# PLAYING ONE BACK, hands off:
#
#   export RECOMP_PAD_SCRIPT="@$HOME/Library/Application Support/JSRF/padrec/<the file>"
#
# The replay refuses to run against a binary that is not the one the
# recording was made with, and says so rather than producing a run that
# looks like a reproduction and is not one.
EX
    die "No game files configured.\n\nEdit:\n$CONF\n\nAn example has been written to:\n$CONF.example\n\nJSRF_GAME_DIR must be a folder containing default.xbe."
fi

if ! probe_readable "$JSRF_GAME_DIR/default.xbe"; then
    die "macOS is not letting this app read your game files.\n\n$JSRF_GAME_DIR\n\nIf a permission prompt appeared, allow it and launch again. If you denied it, grant access in System Settings > Privacy & Security > Files and Folders (or Full Disk Access).\n\nFolders in iCloud Drive, Desktop, Documents and Downloads all need this. Keeping the game files somewhere else -- for example $SUPPORT/game -- avoids it entirely."
fi

: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$JSRF_HDD_ROOT"
if ! mkdir -p "$JSRF_HDD_ROOT" 2>/dev/null || [ ! -w "$JSRF_HDD_ROOT" ]; then
    die "Cannot write to the emulated HDD folder:\n\n$JSRF_HDD_ROOT\n\nEdit JSRF_HDD_ROOT in $CONF."
fi

# The binary reads RECOMP_* names; the scripts in this tree use JSRF_* ones.
# Translate here rather than teaching the binary a second set of names.
RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
exec "$HERE/jsrf-engine" >"$LOG" 2>&1
LAUNCH
# JSRF_APP_LIFT=1 bakes the D3D lift (G50-G52) on as the launcher's default.
# The defaults apply AFTER paths.conf, and only to names it left unset, so a
# player can still turn a class off there (RECOMP_D3D8_HOST_FF=off). Without
# JSRF_APP_LIFT the placeholder is removed and nothing changes.
if [ "${JSRF_APP_LIFT:-0}" = 1 ]; then
    LIFT='# D3D lift on by default (built with JSRF_APP_LIFT=1); override in paths.conf\
export RECOMP_D3D8_HOST_2D="${RECOMP_D3D8_HOST_2D:-draw}"\
export RECOMP_D3D8_HOST_FF="${RECOMP_D3D8_HOST_FF:-draw}"\
export RECOMP_D3D8_HOST_VS="${RECOMP_D3D8_HOST_VS:-draw}"\
export RECOMP_D3D8_HOST_FF_GPU="${RECOMP_D3D8_HOST_FF_GPU:-1}"\
export RECOMP_D3D8_HOST_POINTS="${RECOMP_D3D8_HOST_POINTS:-1}"\
export RECOMP_METAL_ASYNC_WRITEBACK="${RECOMP_METAL_ASYNC_WRITEBACK:-1}"'
    sed -i '' "s|^#@LIFT_DEFAULTS@\$|$LIFT|" "$APP/Contents/MacOS/JSRF"
    echo "  lift:    on by default (JSRF_APP_LIFT=1)"
else
    sed -i '' '/^#@LIFT_DEFAULTS@$/d' "$APP/Contents/MacOS/JSRF"
fi
chmod +x "$APP/Contents/MacOS/JSRF"

# --- Info.plist -------------------------------------------------------------
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>JSRF</string>
    <key>CFBundleDisplayName</key>       <string>Jet Set Radio Future</string>
    <key>CFBundleExecutable</key>        <string>JSRF</string>
    <key>CFBundleIdentifier</key>        <string>$ID</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key>           <string>0.1</string>
    <key>CFBundleIconFile</key>          <string>AppIcon</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <!-- Without this the window is upscaled by the compositor from a
         point-sized backing store, which undoes SDL_WINDOW_ALLOW_HIGHDPI. -->
    <key>NSHighResolutionCapable</key>   <true/>
    <key>LSApplicationCategoryType</key> <string>public.app-category.games</string>
    <!-- macOS shows these strings in the consent prompt. Without them the
         prompt is anonymous, and an anonymous prompt for a game people just
         built themselves is one they are likely to deny. -->
    <key>NSDocumentsFolderUsageDescription</key>
    <string>JSRF needs to read your Jet Set Radio Future disc image and save files.</string>
    <key>NSDesktopFolderUsageDescription</key>
    <string>JSRF needs to read your Jet Set Radio Future disc image if you keep it on the Desktop.</string>
    <key>NSDownloadsFolderUsageDescription</key>
    <string>JSRF needs to read your Jet Set Radio Future disc image if you keep it in Downloads.</string>
    <key>NSRemovableVolumesUsageDescription</key>
    <string>JSRF needs to read your Jet Set Radio Future disc image from an external drive.</string>
    <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>
</dict>
</plist>
PLIST

# --- icon -------------------------------------------------------------------
# Deliberately a plain generated mark, not the game's artwork: the bundle is
# built from the user's own dump but the icon would be redistributed with the
# app. Drop your own AppIcon.icns into Contents/Resources to replace it.
ICONSET=$(mktemp -d)/AppIcon.iconset
mkdir -p "$ICONSET"
python3 - "$ICONSET" <<'ICON'
import struct, sys, zlib, os
def png(path, n):
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    rows = bytearray()
    for y in range(n):
        rows.append(0)
        for x in range(n):
            u, v = x / (n - 1), y / (n - 1)
            edge = min(u, v, 1 - u, 1 - v)
            if edge < 0.06:
                rows += bytes((0, 0, 0, 0)); continue
            r = int(30 + 90 * v); g = int(40 + 60 * u); b = int(90 + 120 * (1 - v))
            d = abs((u - 0.5) + (v - 0.5)) + abs((u - 0.5) - (v - 0.5))
            if d < 0.42: r, g, b = 245, 228, 60
            rows += bytes((r, g, b, 255))
    data = png_sig = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    data += chunk(b"IEND", b"")
    open(path, "wb").write(data)
out = sys.argv[1]
for n in (16, 32, 64, 128, 256, 512, 1024):
    png(os.path.join(out, "icon_%dx%d.png" % (n, n)), n)
    if n > 16:
        png(os.path.join(out, "icon_%dx%d@2x.png" % (n // 2, n // 2)), n)
ICON
if iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null; then
    echo "icon:    generated"
else
    echo "icon:    iconutil failed, bundle will use the generic app icon" >&2
fi

# --- sign -------------------------------------------------------------------
# Ad-hoc. Enough for the app to run on the machine that built it and on any
# machine the user allows in Settings; not enough to distribute without the
# quarantine prompt, which needs a Developer ID and notarisation.
# Inside out: every dylib, then the engine, then the bundle. --deep alone does
# NOT reach Contents/MacOS/jsrf-engine, because CFBundleExecutable names the
# launcher script and codesign only follows the declared main executable --
# which left the one Mach-O that matters unsigned, and codesign --verify said
# so only when asked with --deep --strict.
sign_ok=1
for lib in "$APP"/Contents/Frameworks/*.dylib; do
    [ -e "$lib" ] || continue
    codesign --force --sign - --timestamp=none "$lib" 2>/dev/null || sign_ok=0
done
codesign --force --sign - --timestamp=none "$APP/Contents/MacOS/jsrf-engine" 2>/dev/null || sign_ok=0
codesign --force --sign - --timestamp=none "$APP" 2>/dev/null || sign_ok=0
if [ "$sign_ok" = 1 ] && codesign --verify --deep --strict "$APP" 2>/dev/null; then
    echo "signed:  ad-hoc, verifies"
else
    echo "signed:  FAILED -- Gatekeeper will refuse this bundle" >&2
    codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/    /' >&2
fi

echo
echo "Built $APP"
echo "First run needs $HOME/Library/Application Support/JSRF/paths.conf;"
echo "launching without it writes paths.conf.example and says so in a dialog."
