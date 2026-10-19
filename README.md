# Vocanal

A whole vocal chain in one plugin, built for psytrance vocals: clean-up, de-breath, compression, saturation, de-essing, air, a radio voice, a doubler, pitch and formant shifting, a ducked tempo delay and a reverb. Most modules are one knob: the plugin finds what to fix and the knob says how much. Drag the cards into any order; the default order is the one a mix engineer would patch.

*Work in progress.* Every module works; the editor is a placeholder until the real UI lands, and the factory presets come with it.

```
in ─┬─ CLEAN ─ BREATH ─ SHIFT ─ SMOOTH ─ COMP ─ WARMTH ─ DE-ESS ─ AIR ─ RADIO ─ DOUBLE ─ DELAY ─ REVERB ─┐ wet
    └──────────────────────────── delayed by the latency ───────────────────────────────────────────────┤ dry
                                                                                    mix ─ output ─ out
```

VST3 and Audio Unit for macOS (universal), VST3 for Windows. JUCE 8. An optional stereo sidechain, **Duck Key**, can duck the delay and the reverb from another track instead of the voice.

## The knobs

| Module | Knob | What it does |
|--------|------|--------------|
| **CLEAN** | **Clean** | Takes out what a voice doesn't need below its body: rumble under the lowest note, a boomy or muddy resonance that stays at one pitch whatever you sing, and the thump of a plosive. 0 = off. |
| | **Focus** | Auto: Vocanal listens for 6 s of voice, then holds what it learned (the corner and the two cuts show in the panel). Manual: the values stay where you set them. **Re-learn** listens again. |
| | **High-Pass** | Switches the rumble filter on or off (it sits at 0.7 × the voice's lowest notes). |
| **BREATH** | **Breath** | Turns breaths down, up to 24 dB, and nothing else: quiet singing, a soft /h/ and the ends of words stay. 0 = off. |
| | **Listen** | Plays only what Breath takes away. |
| **SHIFT** | **Mix** | Blends the shifted voice with the dry one (equal power: a harmony at 50 % is as loud as either alone). 0 = off. |
| | **Pitch** | −12 … +12 semitones. |
| | **Formant** | −12 … +12 semitones, on its own. At 0 the vowels stay where they were whatever the pitch; Formant = Pitch is the classic sped-up-tape voice. |
| **SMOOTH** | **Smooth** | Takes out harshness: resonant peaks and harsh bands that stick out of their neighbourhood, mostly between 2 and 8 kHz, as they happen. 0 = off. The loudness stays where it was. |
| | **Sharpness** | How narrow the cuts are: from about 3/4 octave down to 1/6 octave up high (wider below 1 kHz). |
| | **Listen** | Plays only what Smooth takes away. |
| **COMP** | **Comp** | One-knob vocal compression: a fast peak stage for the consonants and an optical-style leveller for the body. It learns the take's loudness first, so it does the same to a quiet and a loud recording, and the loudness stays where it was across the knob. 0 = off. **Re-learn** listens again. |
| **WARMTH** | **Warmth** | Tube/tape saturation: a warm 2nd harmonic first, the 3rd joining as you turn up. The colour follows the knob, not the input level, and the loudness stays put. 0 = off. |
| **DE-ESS** | **De-ess** | Finds the esses itself and cuts them, up to 16 dB, with 6 ms of lookahead so even their first moments are caught. Bright vowels and whistles are left alone. 0 = off. |
| | **Focus** | Auto: the band's centre is learned from your esses (3-12 kHz) and held. Manual: it stays where you set it. |
| | **Listen** | Plays only what De-ess takes away. |
| **AIR** | **Air** | Opens up the top: two gentle shelves (3.2 and 10 kHz) that lift quiet highs and leave loud ones (esses, harsh peaks) alone, only while the voice sounds. Up to +10 dB. 0 = off. |
| | **Tone** | Shares the lift: −100 all at 3.2 kHz, +100 all at 10 kHz. |
| **RADIO** | **Radio** | From the whole voice to a small-speaker radio or telephone: the band narrows to 300 Hz-3.4 kHz, a speaker resonance and a little drive come in. The loudness stays put. 0 = off. |
| | **Tone** | Moves the band: down is a darker, boxier AM radio (250 Hz-2.8 kHz), up a thinner phone (500 Hz-4.5 kHz). |
| **DOUBLE** | **Double** | Two artificial double-tracks a few cents and 16-27 ms off the voice, drifting like real takes. They go into the sides only, so the mono sum is exactly the dry voice. 0 = off. |
| | **Width** | How wide the doubles sit. |
| **DELAY** | **Mix** | The echoes' level; the dry voice stays at full. 0 = off. |
| | **Mode** | Tape (warm repeats that darken, wow and flutter), BBD (dark bucket-brigade echoes with hiss), Digital (clean up to 8 kHz), Clean (every repeat exactly like the first), Lo-Fi (a narrow, gritty band). |
| | **Style** | Mono, Ping-Pong (left, right, left...), Dual (the right side on a partner note: 1/8 against 1/8D, and so on). |
| | **Sync** / **Note** / **Time** | Synced to the host tempo (1/64 to 1/1, straight, dotted and triplet), or free in ms (up to 4 s). Time changes glide like tape. |
| | **Feedback** | 0-100 %; at 100 % the echoes keep going, held in check by the mode's saturation. |
| | **Duck** | Pulls the echoes (and the repeats) down while the voice, or the Duck Key, is loud, and lets them back up in the gaps. |
| | **Diffuse** | Smears each echo, from a slight blur to a wash about 100 ms wide. |
| | **Low Cut** / **High Cut** | Filters every repeat. |
| | **Age** | The mode's wear: wow and flutter, hiss, warble, a narrower band. 0 = none. |
| **REVERB** | **Mix** | The reverb's level; the dry voice stays at full. 0 = off. |
| | **Mode** | Hall, Bright Hall, Plate, Smooth Plate, Room, Chamber, Ambience. |
| | **Colour** | 70s (a darker, grainier early digital box), 80s, Now (clean and flat). A change lets the old tail ring out. |
| | **Decay** | 0.2-20 s, the mid-band RT60. |
| | **Size** | The space's size, decay kept. |
| | **Pre-delay** | 0-250 ms before the reverb starts. |
| | **Low Cut** / **High Cut** | Filters the reverb's output. |
| | **Mod** | How much the tail moves (chorus-like shimmer). |
| | **Duck** | Pulls the reverb down while the voice, or the Duck Key, is loud. |
| **Global** | **Mix** | Dry/wet of the whole chain. |
| | **Output** | −24 … +12 dB. |
| | **Duck Key** | Voice: DELAY and REVERB duck to the voice. Sidechain: to whatever feeds the sidechain input (a beat, a lead). |
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
./start.sh            # pick a vocal in the window
./start.sh vocal.wav  # your dry vocal
```

`start.sh` builds and opens the demo host: a dry voice through the plugin,
looped, nothing else playing. **VOCANAL** plays the plugin's output, **DRY**
the voice as it went in, lined up with it for an A/B; space plays and stops.
The plugin gets a looping transport at 140 BPM, so the delay's tempo sync
works as in a DAW.

The vocals to pick from come from a kit file: copy `demo-kit.example.txt` to
`demo-kit.txt` (ignored by git, sample packs aren't ours to share) and list
a folder of dry vocals (`vocals = ...`) or single files (`vocal = ...`);
`start.sh` picks it up (or set `VOC_DEMO_KIT`). The synthetic voice, plain
and with faults the smart modules should find, is always there. For a
configure option on first run: `VOC_CMAKE_ARGS=-D... ./start.sh`.

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
