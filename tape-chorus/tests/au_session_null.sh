#!/bin/bash
# Saved sessions through the real AU, from 0.5.0. A host keeps a plug-in's saved state (kAudioUnitProperty_ClassInfo) in its session and
# gives it back when the session opens. This makes those states with an older build, then opens each in the new build and compares the
# renders sample for sample. Run from the repo root, after building build/AuRender (see tests/AuRender.cpp).
#   tests/au_session_null.sh save <old .component> <folder>    renders each case with the old build, keeps its state and its render
#   tests/au_session_null.sh check <folder> [new .component]    opens each state in the new build (the installed one if none) and compares
# Every case is rendered with settle=1 (the plug-in starts again once the state is in), as a session that opens before playback.
# The cases: the default, every factory preset the old build has, one session with every control moved at three rates, in stereo and
# in mono, and Bypass. With an 0.4.0 build also its three Clean presets and two sessions with Clean on. Cases named *fb90* hold Feedback
# 90, which 0.5.0 turns down on purpose, and clean_sat100 holds Clean on at Saturation 100, where 0.5.0's Clean drive bends over: those
# are expected to differ, and are reported as such. Everything else must be identical.
mode="$1"; R=build/AuRender
L=tests/speech/samantha.wav; D=tests/speech/daniel.wav
ALL=("Voices=1" "Rate=0.3" "Depth=0.4" "Delay Time=0.2" "Feedback=0.7" "Wow=0.5" "Wow Rate=0.4" "Flutter=0.6" "Flutter Rate=0.4" "Random=0.5" "Random Rate=0.5" "Unsync=1" "Saturation=0.6" "Degrade=0.5" "Degrade Rate=0.5" "Noise=0.4" "Noise Gate=0" "Low Cut=0.3" "High Cut=0.6" "Tone=0.4" "Spread=0.9" "Mix=0.6" "Input Gain=0.55" "Output Gain=0.45")
form_args () { case "$1" in all_s44b64*) echo "rate=44100 block=64";; all_s96b512*) echo "rate=96000 block=512";; all_m48b256*|default_m48b256) echo "channels=1 block=256";; *) echo "";; esac; }
if [ "$mode" = save ]; then
  old="$2"; out="$3"; mkdir -p "$out"
  v=$(defaults read "$old/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null)
  presets="0 1 2 3 4 5 6 7 8 9 10 11 12"; [ "$v" = 0.4.0 ] && presets="$presets 13 14 15"
  # The old build makes the state, then renders it the way it renders a session it reopens: from that state. (A render made straight from
  # the preset or the settings can differ in its last bits from one made from the saved state, because the state holds each value as text.)
  run () {
    name="$1"; shift
    "$R" "$L" "$D" "$out/$name.tmp.raw" "$@" settle=1 "component=$old" "savestate=$out/$name.state" || { echo "FAILED $name"; exit 1; }
    rm -f "$out/$name.tmp.raw"
    args=($(form_args "$name"))
    "$R" "$L" "$D" "$out/$name.old.raw" "${args[@]}" "loadstate=$out/$name.state" settle=1 "component=$old" || { echo "FAILED $name"; exit 1; }
  }
  run default
  run default_m48b256 channels=1 block=256
  for p in $presets; do run "preset$p" "preset=$p"; done
  run all_s48b128 "${ALL[@]}"
  run all_s44b64 rate=44100 block=64 "${ALL[@]}"
  run all_s96b512 rate=96000 block=512 "${ALL[@]}"
  run all_m48b256 channels=1 block=256 "${ALL[@]}"
  run bypass "Bypass=1"
  run all_fb90 "${ALL[@]}" "Feedback=1"
  if [ "$v" = 0.4.0 ]; then
    run default_clean "Clean=1"
    run all_clean_s48b128 "${ALL[@]}" "Clean=1"
    run clean_sat100 "Clean=1" "Saturation=1"
  fi
  echo "$v" > "$out/version.txt"
  echo "saved $(ls "$out"/*.state | wc -l | tr -d ' ') states from $v in $out"
  exit 0
fi
if [ "$mode" = check ]; then
  out="$2"; new="$3"; bad=0; same=0; differ=0
  for s in "$out"/*.state; do
    name=$(basename "$s" .state)
    args=($(form_args "$name"))
    extra=(); [ -n "$new" ] && extra+=("component=$new")
    "$R" "$L" "$D" "$out/$name.new.raw" "${args[@]}" "loadstate=$s" settle=1 "${extra[@]}" || { echo "FAILED $name"; bad=1; continue; }
    if cmp -s "$out/$name.old.raw" "$out/$name.new.raw"; then echo "identical  $name"; same=$((same + 1))
    elif [[ "$name" == *fb90* ]]; then echo "differs as it should (Feedback 90 comes down in 0.5.0)  $name"; differ=$((differ + 1))
    elif [[ "$name" == *sat100* ]]; then echo "differs as it should (Clean's drive bends over above Saturation 60 in 0.5.0)  $name"; differ=$((differ + 1))
    else echo "DIFFERENT  $name"; bad=1; fi
    rm -f "$out/$name.new.raw"
  done
  echo "from $(cat "$out/version.txt"): $same identical, $differ differ on purpose"
  exit $bad
fi
echo "usage: tests/au_session_null.sh save <old .component> <folder> | check <folder> [new .component]"; exit 2
