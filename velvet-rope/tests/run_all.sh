#!/bin/zsh
# Runs every test and saves the output to tests/results.txt. Run from the repo root, after InstallAU.
{
  echo "Smooth Resonance test results, $(date '+%Y-%m-%d')"
  echo "\n===== ProcessorTest: the real processor, every control, generic checks"
  build/ProcessorTest_artefacts/Release/ProcessorTest
  echo "\n===== NullTest: old sessions, presets and defaults against 0.4.1"
  build/NullTest_artefacts/Release/NullTest
  echo "\n===== FeatureTest: 8 dots, Quality, Max Cut, Band Listen, multichannel"
  build/FeatureTest_artefacts/Release/FeatureTest
  echo "\n===== HeldNotesTest: the Held Notes control, off and on"
  build/HeldNotesTest_artefacts/Release/HeldNotesTest
  echo "\n===== DspTest: synthetic signals through the DSP"
  build/DspTest_artefacts/Release/DspTest
  echo "\n===== SpeechTest: real speech from macOS say, with resonant boosts at known frequencies"
  build/SpeechTest_artefacts/Release/SpeechTest
  echo "\n===== HostCheck in this repo: the installed AU through AudioToolbox, measured"
  build/HostCheck
  echo "\n===== LayoutCheck: the installed AU once per channel layout, and Quality through the real AU"
  build/LayoutCheck
  echo "\n===== Shared HostCheck: aufx SmRs Luke"
  ~/smooth-plugins/_shared/bin/HostCheck aufx SmRs Luke
  echo "shared HostCheck exit code $?"
  echo "\n===== auval -v aufx SmRs Luke, last lines"
  auval -v aufx SmRs Luke 2>&1 | tail -4
} 2>&1 | tee tests/results.txt
