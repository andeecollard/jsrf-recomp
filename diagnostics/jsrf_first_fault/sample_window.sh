#!/bin/sh
# Record the game window at full frame rate and extract consecutive frames.
#
# WHY: every capture path in this tree samples too slowly to see this class of
# defect. RECOMP_FB_DUMP_FLIP writes at most 24 frames and counts presents from
# process start, so a stride large enough to reach gameplay puts hundreds of
# frames between captures; screencapture manages two or three a second. The
# artefacts that have mattered here -- a tile mosaic, black bars that vanish
# "very quickly", pixel noise -- are all frame-to-frame, and none of them is
# visible in frames taken 350 presents apart.
#
# A person watching the screen found in seconds what four synthetic metrics
# could not, because they could see consecutive frames. This is that, made
# repeatable: avfoundation records the screen at the display's own rate and
# ffmpeg writes every frame out, so the difference between frame N and N+1 is
# actually measurable.
#
#   sample_window.sh <outdir> [seconds] [delay-before-recording]
#
# The delay exists because the artefact lives at gameplay and the title takes
# around 200 s of the scripted pad to get there; start the run, then start this.
set -u
OUT="${1:?usage: sample_window.sh <outdir> [seconds] [delay]}"
SECS="${2:-6}"
DELAY="${3:-0}"
DEV="${JSRF_SCREEN_DEV:-3}"     # ffmpeg -f avfoundation -list_devices true -i ""

mkdir -p "$OUT" || exit 1
[ "$DELAY" -gt 0 ] && sleep "$DELAY"

# -r before -i asks the device for that rate; the display decides what it gives.
ffmpeg -loglevel error -f avfoundation -r 30 -i "$DEV" -t "$SECS" \
       -pix_fmt yuv420p "$OUT/screen.mov" </dev/null || exit 1
ffmpeg -loglevel error -i "$OUT/screen.mov" "$OUT/f%04d.png" </dev/null || exit 1

n=$(ls "$OUT" | grep -c '\.png$')
echo "$OUT: ${SECS}s recorded, $n frames extracted"
# Consecutive frames are what this is for, so say how many are actually
# distinct -- a recording faster than the game repeats frames, and a metric
# that counts duplicates as evidence would be measuring the recorder.
echo "hint: fb_score.py --temporal $OUT   (dedupes before comparing)"
