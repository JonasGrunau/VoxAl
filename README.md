# Vocanal

A whole vocal chain in one plugin, built for psytrance vocals: clean-up, de-breath, compression, saturation, de-essing, air, a radio voice, a doubler, pitch and formant shifting, a ducked tempo delay and a reverb. Most modules are one knob: the plugin finds what to fix and the knob says how much. Drag the cards into any order; the default order is the one a mix engineer would patch.

*Work in progress.* SHIFT and SMOOTH work; the other modules pass the voice through untouched until their milestone lands, and the editor is a placeholder until the real UI arrives.

```
in ─┬─ CLEAN ─ BREATH ─ SHIFT ─ SMOOTH ─ COMP ─ WARMTH ─ DE-ESS ─ AIR ─ RADIO ─ DOUBLE ─ DELAY ─ REVERB ─┐ wet
    └──────────────────────────── delayed by the latency ───────────────────────────────────────────────┤ dry
                                                                                    mix ─ output ─ out
```

VST3 and Audio Unit for macOS (universal), VST3 for Windows. JUCE 8. An optional stereo sidechain, **Duck Key**, can duck the delay and the reverb from another track instead of the voice.

## The knobs

| Module | Knob | What it does |
|--------|------|--------------|
| **SHIFT** | **Mix** | Blends the shifted voice with the dry one (equal power: a harmony at 50 % is as loud as either alone). 0 = off. |
| | **Pitch** | −12 … +12 semitones. |
| | **Formant** | −12 … +12 semitones, on its own. At 0 the vowels stay where they were whatever the pitch; Formant = Pitch is the classic sped-up-tape voice. |
| **SMOOTH** | **Smooth** | Takes out harshness: resonant peaks and harsh bands that stick out of their neighbourhood, mostly between 2 and 8 kHz, as they happen. 0 = off. The loudness stays where it was. |
| | **Sharpness** | How narrow the cuts are: from about 3/4 octave down to 1/6 octave up high (wider below 1 kHz). |
| | **Listen** | Plays only what Smooth takes away. |
| **Global** | **Mix** | Dry/wet of the whole chain. |
| | **Output** | −24 … +12 dB. |
| | **Quality** / **Render Quality** | Normal or High. Bounces use Render Quality (High by default); the latency is the same in both. |

With every knob at its default the voice passes bit for bit, delayed by the plugin's latency (fixed per sample rate, the same at every quality and in every order). Moving a card, loading a preset or undoing never clicks: the chain crossfades over 20 ms, with every module's state copied first, so echoes and tails carry on.

## How it sounds the way it does

**SHIFT** (`src/core/fx/PitchShifter.cpp`, `Shift.cpp`): a peak-locked phase vocoder. Every spectral peak moves with the bins around it to exactly ratio × its true frequency, its phase turned to match, so partials stay pure; shifting down, each output bin keeps the louder of the regions crowding onto it instead of summing them, so an octave down doesn't beat. The formants come from a true spectral envelope (an iterated cepstrum whose order follows the voice's F0) and each moved region is scaled from the envelope at its old frequency to the envelope at its new one, read through the Formant knob. Above 50 kHz it runs at half or a quarter of the rate behind IIR halfbands, so it costs the same at every rate (about 6 % of one core) and sounds the same. Asymmetric low-delay windows keep it 18 ms late. The shifted voice is held at the dry voice's K-weighted loudness. Tests: the interval within 0.3 % up and down an octave, one clean sine (residual under −30 dB), the moved harmonics within 2 dB RMS of the envelope they should follow, loudness within 1.5 dB across Pitch and Formant, bit-exact rest, an honest latency.

**SMOOTH** (`src/core/fx/Smooth.cpp`): a lookahead spectral compressor driving a minimum-phase EQ. Every 5.3 ms a 2048-point analysis centred 17 ms ahead reads the voice's peak envelope on a 1/48-octave grid and compares each frequency with its neighbourhood; whatever stands more than 3.5 dB out is cut on a soft curve, most where harshness lives (2–5 kHz) and hardly in the body, with the highs reacting fastest. A resonance that appears is cut harder than one that stays. The cut curve becomes a minimum-phase filter (folded cepstrum: no pre-ringing) that runs on the delayed voice by overlap-add, so it moves without steps; a slow K-weighted makeup gives back the level. Latency 27 ms. Tests: the notch phase is minimum-phase within 0.004 rad; white and pink noise change under 0.05 dB, a broad bump under 0.4 dB; a ringing peak 20 dB over noise loses 9 dB at 70 %; a harsh ring in a voice loses 4.3 dB while the rest of the vowels move 0.5 dB RMS and the loudness 0.2 LU. About 1 % of a core at 48 kHz.

## Building

Requirements: macOS 11+, CMake 3.24+, Ninja, Xcode command line tools.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This installs the VST3 and AU into `~/Library/Audio/Plug-Ins`. JUCE 8 is
downloaded automatically; pass `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`
to use a local checkout.

### Core only

The DSP doesn't depend on JUCE and builds in seconds:

```sh
cmake -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release -DVOCANAL_BUILD_PLUGIN=OFF
cmake --build build-core
./build-core/vocanal_tests
```

### UI snapshots

`build/vocanal_snapshot_artefacts/Release/vocanal_snapshot <dir>`
renders the editor to PNGs with the built-in loop playing through it, no
DAW needed. `... <dir> bench` times the paints instead.

### Offline renders and the CPU bench

```sh
./build-core/vocanal_render in.wav out.wav shift_mix=100 shift_pitch=-12
./build-core/vocanal_bench
```

`vocanal_render` runs a WAV through the chain with any parameter (ids as in
`src/core/fx/Params.h`), at Render Quality, latency removed. `vocanal_bench`
prints the load per sample rate and block size.

### Trying it without a DAW

```sh
./start.sh                       # the synthetic voice over the built-in loop
./start.sh vocal.wav [beat.wav]  # your vocal (and beat)
```

`start.sh` builds and opens the demo host: the voice through the plugin, the
beat around it and on the Duck Key bus. **MIX**, **VOICE** and **BEAT** pick
what you hear; space plays and stops. The plugin gets a matching looping
transport, so the delay's tempo sync works as in a DAW.

Real samples come from a kit file: copy `demo-kit.example.txt` to
`demo-kit.txt` (ignored by git, sample packs aren't ours to share) and list
dry vocals (`vocal = ...`) and, for the beat, a kick, the bass shots, their
tempo and the pattern; `start.sh` picks it up (or set `VOC_DEMO_KIT`).
Without a kit the voice and the loop are synthesized. For a configure option
on first run: `VOC_CMAKE_ARGS=-D... ./start.sh`.

### Releases

Push a version tag and GitHub Actions builds, tests and validates the plugins
(pluginval at strictness 5, auval) and attaches the installers to a new
release:

```sh
git tag v1.2.0 && git push origin v1.2.0
```

You get a macOS `.pkg` (universal, VST3 + AU), a Windows setup `.exe` (VST3)
and a zip of the bare plug-ins for each. A suffix such as `v1.2.0-beta.1` makes
it a pre-release. The installer scripts are in `packaging/`.

The macOS build is signed with a Developer ID and notarized when these
repository secrets are set (Settings > Secrets and variables > Actions):

| Secret | Value |
|--------|-------|
| `MACOS_CERTIFICATES_P12` | base64 of a `.p12` holding both the *Developer ID Application* and the *Developer ID Installer* certificate with their keys |
| `MACOS_CERTIFICATES_PASSWORD` | the `.p12` password |
| `NOTARY_APPLE_ID` | Apple ID of the developer account |
| `NOTARY_PASSWORD` | an app-specific password for that Apple ID |

Without them the macOS build is unsigned and macOS blocks the `.pkg` the first
time (System Settings > Privacy & Security > Open Anyway). The Windows
installer is not signed, so SmartScreen asks once (More info > Run anyway).

## Project layout

```
src/core/dsp       primitives (rings, smoothers, FFT, halfbands, loudness)
src/core/fx        the parameter table, the order, the chain, the modules
src/plugin         JUCE processor and editor
src/ui             theme, controls, views
tests/             Catch2 tests for the core, test signals, the synthetic voice
tools/             snapshot tool, demo host, offline render, CPU bench
packaging/         macOS pkg and Windows installer scripts
```

## License

The plugin links JUCE 8 under the AGPLv3. IBM Plex fonts under the SIL Open
Font License (`resources/fonts/IBMPlex-OFL.txt`).
