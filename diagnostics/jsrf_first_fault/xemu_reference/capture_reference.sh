#!/bin/zsh
# Reference capture with sample-exact audio.
# SDL3's disk driver takes the APU output straight to a raw S16LE 48k stereo
# file -- no host mixing, no resampling. TIMESCALE=1 is realtime; the 100 used
# earlier made it write ~100x too slowly.
# caffeinate keeps the display from locking mid-run.
set -u
#
# Third argument `audio` skips the video half. The audio is the sample-exact
# part -- it comes off the APU through SDL's disk driver -- while the video half
# needs Screen Recording permission, an avfoundation device index that is not
# stable across machines, and a full-screen xemu that takes the display. When
# the question is "what should this passage SOUND like", none of that earns its
# cost, and the run is far less intrusive without it.
OUT=${1:?outdir}
DUR=${2:-150}
MODE=${3:-av}
mkdir -p "$OUT"
# Absolute, once. Every path below was "$PWD/$OUT", which silently produces
# "$PWD//tmp/..." for an absolute outdir -- xemu then fails to open its trace
# file and exits before writing a single sample, and the only symptom is an
# empty capture.
OUT=${OUT:A}

export SDL_AUDIO_DRIVER=disk
export SDL_AUDIO_DISK_OUTPUT_FILE="$OUT/apu.raw"
export SDL_AUDIO_DISK_TIMESCALE=1

date +%s.%N > "$OUT/t0_xemu"
XEMU_ARGS=(-s -trace enable=mcpx_apu_method,file="$OUT/apu_method.trace")
[[ "$MODE" == audio ]] || XEMU_ARGS+=(-full-screen)
caffeinate -dimsu /Applications/xemu.app/Contents/MacOS/xemu "${XEMU_ARGS[@]}" \
  > "$OUT/xemu.log" 2>&1 &
echo $! > "$OUT/xemu.pid"

if [[ "$MODE" == audio ]]; then
    # No ffmpeg, no focus grab. Let it boot and play, then stop it. The disk
    # audio driver has been writing apu.raw the whole time: S16LE, 48 kHz,
    # stereo, one sample per sample, which is the whole point of this rig.
    sleep "$DUR"
    kill "$(cat "$OUT/xemu.pid")" 2>/dev/null
    sleep 2; kill -9 "$(cat "$OUT/xemu.pid")" 2>/dev/null
    date +%s.%N > "$OUT/t_end"
    ls -l "$OUT/apu.raw" 2>/dev/null || echo "NO AUDIO CAPTURED -- check xemu.log" >&2
    exit 0
fi

sleep 5
osascript -e 'tell application "System Events" to set frontmost of (first process whose name contains "xemu") to true' >/dev/null 2>&1 || true
date +%s.%N > "$OUT/t0_ffmpeg"
caffeinate -dimsu ffmpeg -hide_banner -loglevel warning \
  -f avfoundation -capture_cursor 0 -framerate 30 -i "3:" \
  -t "$DUR" -vf "scale=1280:-2" -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \
  -y "$OUT/screen.mp4" > "$OUT/ffmpeg.log" 2>&1
date +%s.%N > "$OUT/t_end"
