#!/bin/zsh
# Reference capture with sample-exact audio.
# SDL3's disk driver takes the APU output straight to a raw S16LE 48k stereo
# file -- no host mixing, no resampling. TIMESCALE=1 is realtime; the 100 used
# earlier made it write ~100x too slowly.
# caffeinate keeps the display from locking mid-run.
set -u
OUT=${1:?outdir}
DUR=${2:-150}
mkdir -p "$OUT"

export SDL_AUDIO_DRIVER=disk
export SDL_AUDIO_DISK_OUTPUT_FILE="$PWD/$OUT/apu.raw"
export SDL_AUDIO_DISK_TIMESCALE=1

date +%s.%N > "$OUT/t0_xemu"
caffeinate -dimsu /Applications/xemu.app/Contents/MacOS/xemu \
  -s -full-screen \
  -trace enable=mcpx_apu_method,file="$PWD/$OUT/apu_method.trace" \
  > "$OUT/xemu.log" 2>&1 &
echo $! > "$OUT/xemu.pid"

sleep 5
osascript -e 'tell application "System Events" to set frontmost of (first process whose name contains "xemu") to true' >/dev/null 2>&1 || true
date +%s.%N > "$OUT/t0_ffmpeg"
caffeinate -dimsu ffmpeg -hide_banner -loglevel warning \
  -f avfoundation -capture_cursor 0 -framerate 30 -i "3:" \
  -t "$DUR" -vf "scale=1280:-2" -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \
  -y "$OUT/screen.mp4" > "$OUT/ffmpeg.log" 2>&1
date +%s.%N > "$OUT/t_end"
