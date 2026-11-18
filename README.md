# Smooth Plugins

15 free Audio Unit plug-ins for Logic Pro, made mostly for vocals: tuning, doubling, harmonies, voice changing, tape effects and more. Free to use in any music, including releases you sell.

**[Download the Smooth Plugins app](https://github.com/coquetteluke/smooth-plugins/releases/latest/download/Smooth-Plugins-App.zip)** · [All releases](https://github.com/coquetteluke/smooth-plugins/releases)

## What you need

- A Mac with Apple Silicon (M1 or newer). Intel Macs are not supported.
- macOS 13 Ventura or newer.
- Logic Pro. They are standard Audio Units, so other AU hosts may work, but they were only tested in Logic.

## Install

The Smooth Plugins app installs the plug-ins you pick and keeps them up to date:

![The Smooth Plugins app: every plug-in with a checkbox, its version and whether it has an update](docs/app.jpg)

1. Download [Smooth-Plugins-App.zip](https://github.com/coquetteluke/smooth-plugins/releases/latest/download/Smooth-Plugins-App.zip), unzip it and drag **Smooth Plugins** into your Applications folder.
2. Open it. macOS says it can't verify the app, because it isn't signed with a paid Apple developer account. Click **Done**.
3. Open System Settings > Privacy & Security, scroll down to the message about Smooth Plugins and click **Open Anyway**, then enter your password. (On macOS 13 or 14 you can instead right-click the app and choose Open.)
4. Tick the plug-ins you want and click **Install**.
5. Open Logic Pro. The plug-ins are under Audio Units > Luke Garito.

**Updates:** the app checks for updates once a day. When new versions come out, it opens in the background (it doesn't take you away from what you're doing) with the number of updates on its Dock icon. Click **Update** and restart Logic. You can turn the daily check off at the bottom of the app; untick it before deleting the app.

**Removing:** tick the plug-ins and click **Remove**. They go to the Trash.

If Logic lists a plug-in as failed, open Logic Pro > Settings > Plug-in Manager, select it and click Reset & Rescan Selection.

<details>
<summary>Other ways to install: the installer package or the zip</summary>

**Installer package.** It has a checkbox for each plug-in, but it does not check for updates.

![The installer's list of plug-ins, each with a checkbox](docs/installer.jpg)

1. Download [Smooth-Plugins-Installer.pkg](https://github.com/coquetteluke/smooth-plugins/releases/latest/download/Smooth-Plugins-Installer.pkg) and double-click it.
2. Click **Done** when macOS says it can't verify it, then click **Open Anyway** in System Settings > Privacy & Security.
3. Click Continue, choose **Install for all users** or **Install for me only**, untick any plug-ins you don't want, and click Install.

**Zip, by hand.**

1. Download `Smooth-Plugins.zip` from the [latest release](https://github.com/coquetteluke/smooth-plugins/releases/latest) and unzip it.
2. In Finder, choose Go > Go to Folder, paste `~/Library/Audio/Plug-Ins/Components` and press Return.
3. Drag the `.component` files you want into that folder.
4. Open Terminal and run this once. macOS blocks plug-ins copied out of a downloaded zip until you do:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/*.component
   ```

The app also updates plug-ins installed either of these ways.

</details>

## The plug-ins

| Plug-in | Kind | What it does |
|---|---|---|
| [Tune Pro](tune-pro) | MIDI-controlled effect | Advanced automatic vocal tuning with two engines, MIDI targets and a pitch window |
| [Tune](tune) | Effect | Low latency real time vocal pitch correction with formant preservation |
| [Glitter](glitter) | MIDI-controlled effect | Fast creative vocal tuner with effect modules |
| [Choir Loft](choir-loft) | MIDI-controlled effect | Up to eight formant preserving harmony voices from one lead vocal |
| [Duo](duo) | Effect | Automatic vocal doubling |
| [Alter Ego](alter-ego) | Effect | Real time voice changer with independent pitch and formants, characters, vibrato and whisper |
| [Throat](throat) | Effect | Reshapes the vocal tract of a voice without moving its pitch |
| [Velvet Rope](velvet-rope) | Effect | Finds the narrow peaks that stick out of a voice and turns them down only while they are there |
| [Punch](punch) | Effect | Vocal impact: transient shaper, compressors and output ceiling |
| [Key](key) | Effect | Live key, scale and tempo detection |
| [Cutup](cutup) | Instrument | Vocal sample slicer played from MIDI |
| [Tape Color](tape-color) | Effect | Oversampled tape saturation with wow, flutter, noise and age |
| [Tape Chorus](tape-chorus) | Effect | Tape style chorus with 1 to 8 voices, feedback, wow, flutter and saturation |
| [Tape Flanger](tape-flanger) | Effect | Through-zero tape style flanger |
| [Switchboard](switchboard) | Effect | Put it on Stereo Out to see and set the controls of these plug-ins on every track from one window |

Each folder has its own README with every control, the presets and the changelog.

**Switchboard and the link:** each plug-in writes a small file describing its controls to `~/Library/Application Support/SmoothPlugins/link/`, so Switchboard can find it. Nothing is sent over the internet.

## Build from source

You need the Xcode command line tools and CMake 3.22 or newer. Each plug-in builds on its own, and JUCE 8.0.10 is downloaded the first time you configure one:

```bash
cmake -S duo -B duo/build
```

```bash
cmake --build duo/build --target InstallAU -j 8
```

`InstallAU` builds the plug-in, signs it for your Mac and copies it into `~/Library/Audio/Plug-Ins/Components`. Swap `duo` for any other folder. To use a JUCE checkout you already have, add `-DJUCE_DIR=/path/to/JUCE` to the first command.

To build the Smooth Plugins app, run `updater/build-app.sh <output folder>`. To make every release file (app, installer, zips and the `manifest.json` the app reads) from a folder of built `.component` files, run `release/make-release-assets.sh <folder> <release tag> <output folder>`.

The tests' voice recordings are not in this repo, because they were made with the macOS text to speech voices, which Apple does not allow redistributing. Run `tests/make_corpus.sh` inside a plug-in's folder to make them on your own Mac. Tests that compare against saved renders from earlier versions will report those files missing.

## License

The code is licensed under the [GNU AGPL v3](LICENSE), the open source license of JUCE 8, which these plug-ins are built on. The fonts are under the SIL Open Font License 1.1; see [third-party/FONTS.md](third-party/FONTS.md).
