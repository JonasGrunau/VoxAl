# Vocanal

## Purpose
A whole vocal chain in one psytrance-minded plugin (VST3 / AU, JUCE 8): smart one-knob modules (CLEAN, BREATH, SMOOTH, COMP, WARMTH, DE-ESS, AIR, RADIO, DOUBLE), pitch/formant SHIFT, a ducked tempo DELAY and a REVERB, in an order the user drags. SMOOTH is the one-knob harshness remover (soothe2-like behaviour, from Jonas's black-box notes `docs/soothe2-how-it-works.md`, which stay out of git). The plan, with the measurable targets, is `~/.claude/plans/i-want-to-build-cosmic-nautilus.md` (milestones M1–M8).

Buses: the voice is on the main input. The optional stereo sidechain "Duck Key" can duck DELAY and REVERB instead of the voice (Duck Key = Sidechain).

Invariants:
- With every knob at its default, the output is the input delayed by the latency, bit for bit.
- The latency is fixed per sample rate: the same in every order, at every quality, and at rest.
- A reorder or a new sound never clicks.

**Status:** M1–M6 done: all 12 modules are real. The editor is still the M1 placeholder (M7). The measured DELAY/REVERB targets are in memory (`vocanal-measurement-targets`). The chain, order, transitions, parameters, state, undo, sidechain, demo host, render tool and bench are in.

## Key Files
| File | Description |
|------|-------------|
| `CMakeLists.txt` | The core builds with `-fcx-limited-range` (complex products without the inf/NaN rescue: 6x faster FFTs). Targets: `vocanal_core` (static lib, no JUCE), the `Vocanal` plugin, `vocanal_snapshot`, `vocanal_demo`, `vocanal_tests`, `vocanal_bench` (core only). Options `VOCANAL_BUILD_PLUGIN`, `VOCANAL_BUILD_TESTS`, `VOCANAL_COPY_PLUGIN` (off in CI). Cache var `VOCANAL_VERSION`. UI sources are listed once in `VOCANAL_UI_SOURCES` |
| `src/core/fx/Params.h/.cpp` | **The parameter table** (X-macro `VOC_PARAMS`): id (the save format), host name, unit, range, default, flags (`kAuto`, `kPreset`, `kLearned`), choice lists. `Settings` = every plain value plus `render`. Knob laws `toNormalised`/`fromNormalised` (log for Hz/ms/s), `offEnd` (where a knob says "Off"). The processor, presets, the undo history and the tests all read it |
| `src/core/fx/Order.h/.cpp` | `ModuleId` (internal index, append only), `kModuleIds` (saved ids, never rename), `kDefaultOrder` with the reasoning, `Order` (pack into a uint64 with 4 bits per slot, `moved`, tolerant `parse`) |
| `src/core/fx/Module.h` | The module contract (see below), `Transport`, `Block` (duck key + transport), `Passthrough` |
| `src/core/fx/Chain.h/.cpp` | The engine: two instances per module, suffix-only reorder, sliced state copy, the 20 ms fade with B's inputs blended from A's, latency-aligned dry for Mix and bypass, Output, the duck key delayed per slot |
| `src/core/dsp/RingBuffer.h` | Pow2 float ring with a 64-bit counter; `delay()` is an exact integer delay; sliced copy (`beginCopy`/`copySlice`/`finishCopy`) that is exact while the source keeps writing |
| `src/core/dsp/Smoother.h` | Smoother / Fade (smoothstep) / SlewPole (kit) |
| `src/core/dsp/Fft.h/.cpp`, `Halfband.h/.cpp` | From build-app: radix-2 FFT (double), polyphase IIR halfband (HIIR design) |
| `src/core/dsp/Loudness.h` | BS.1770 K-weighting at any rate; `LoudnessMatch` (slow gain that holds a follower at a reference's K-weighted power) |
| `src/core/fx/PitchShifter.h/.cpp` | SHIFT's engine at an internal rate <= 50 kHz: peak-locked PV (build-app's, reworked), louder-region-wins bins when shifting down, true-envelope formants (order fs / 2 F0), density compensation, a mono fast path, each frame spread over its hop in 5 stages (lag = 2 tails + a hop = 864 samples) |
| `src/core/fx/Smooth.h/.cpp` | SMOOTH: centred 2048-point linked power analysis, a peak envelope (loudest bin within 120 Hz) on a 1/48-octave grid, narrow (Sharp) vs 1.5-octave dB-averaged neighbourhood with a slow adaptive reference, 3.5 dB threshold, presence weighting, HF-fast attack; minimum-phase filter from the folded cepstrum (4x grid at render) applied by overlap-add; boost-only loudness makeup; 6 stages per hop; latency N/2 + hop = 1280 at 48 kHz; `displayCutDb()` for the UI |
| `src/core/fx/Clean.h/.cpp` | CLEAN: 18 dB/oct HPF at 0.7 x the low F0 percentile (YIN on a decimated copy), boom/mud bells fitted to the residual against 60 vowel templates (fixed-Hz resonances only, pitch spread required), plosive dip with 5 ms lookahead; learns HPF/boom/mud Hz + cut dB |
| `src/core/fx/Breath.h/.cpp` | BREATH: two-tier downward expander on breaths (quiet, broadband, unvoiced, slow swell), opens in 0.8 ms on onsets, 5 ms lookahead, 24 dB x knob, Listen |
| `src/core/fx/Comp.h/.cpp` | COMP: 2 ms lookahead peak stage (4:1) into an opto-style leveller (3:1, two-stage release), K-weighted/100 Hz sidechain; thresholds relative to the learned loudness `comp_ref`; makeup fixed per (knob, learned spread `comp_spread`); `gainReductionDb()` |
| `src/core/fx/DeEss.h/.cpp` | DE-ESS: 6 ms lookahead; band power vs the voiced body level, gated by band share, spectral flatness (512/64 FFT) and HF periodicity (no prev-buffer energy ratio / ZCR: patent clear); TPT bell at the learned centre `deess_hz`; Listen |
| `src/core/fx/Warmth.h/.cpp` | WARMTH: tanh - 0.3 tanh^2 curve on the residual only (ADAA), odd part through tape emphasis, even through a high cut, 4x linear-phase FIR, drive vs a slow RMS (colour, not level), static makeup |
| `src/core/fx/Air.h/.cpp` | AIR: two dynamic analog-fitted high shelves (3.2 / 10 kHz), lift capped by each band's level vs the voice (esses get none), voice gate with hold; Tone splits the lift; latency 0 |
| `src/core/fx/Radio.h/.cpp` | RADIO: 24 dB/oct band morph to 300 Hz-3.4 kHz (Tone 250-2.8k ... 500-4.5k), speaker bell, ADAA tanh at a 176-192 kHz inner rate, static makeup per knob from a standard speech spectrum |
| `src/core/fx/Doubler.h/.cpp` | DOUBLE: two seeded drifting delay taps (16/27 ms, +-14 cents max) into the side only (mono sum = dry), centre keeper and loudness guard on the side, Hermite / Lagrange-6 |
| `src/core/fx/Delay.h/.cpp`, `Sync.h` | DELAY: two lines, latency 0; modes Tape / BBD / Digital / Clean / Lo-Fi as stage choices (input, per pass, feedback-only), write nonlinearity (ADAA algebraic sigmoids, no oversampling) and read modulation, fitted to the M4 measurements; styles Mono / Ping-Pong / Dual (partner note); sync with 0.01 BPM hysteresis, tape-like time glide, read compensated for the loop's group delay; duck on output and feedback; Diffuse = 4 allpasses per line at fixed length. `Sync.h`: note lengths in beats |
| `src/core/fx/Reverb.h/.cpp` | REVERB: two engines (playing + spill on a mode/colour change), each a 16-line Hadamard FDN (Hall, Bright Hall, Ambience with early reflections) or a Dattorro-style ring (Plate, Smooth Plate, Room, Chamber); Jot decay filters per line from the target RT60 per band; colours 70s (half rate behind halfbands, stepped modulation) / 80s / Now; Mix on a square law; latency 0 |
| `src/core/fx/Learn.h` | Learn and hold: which params each learner owns (`kLearnSpecs`), `LearnReport`; the processor latches ready reports on a 10 Hz timer |
| `src/core/fx/Shift.h/.cpp` | The SHIFT module: dry ring, halfband resampling to the engine rate, equal-power Mix, engage fade (only once the engine is ready), loudness match. Rest = power off, Mix 0, or Pitch and Formant both 0 |
| `src/plugin/PluginProcessor.cpp` | Host params built from the table (custom `NormalisableRange` = the core's laws; texts with "Off"); transactions (`Change`); the order as state; snapshot undo history; sidechain read; transport (holds the last host BPM); `processBlockBypassed` through the chain's delayed bypass; state with `order` and `stateVersion` |
| `src/plugin/PluginEditor.cpp` | M1 placeholder sheet (Output, Mix). The real UI is M7 |
| `tests/StubModules.h` | Stand-ins with real latency (`Lookahead`), real tails (`Echo`), a shaper, a stateful filter and a key probe; `stubFactory()`, `delayFactory()` |
| `tests/SynthVoice.h` | Synthetic voice with ground truth: labelled segments (vowel, sibilant, fricative, plosive, breath) and an exact vowel envelope, optional boom, harsh ring and loud-ess faults at known Hz |
| `tools/Demo.cpp` | Demo host (`./start.sh [vocal.wav]`): a dry voice through the plugin, looped on whole bars with room for the tails, no beat. VOCANAL / DRY (the input delayed by the latency) for an A/B. Sources: every kit vocal, the synthetic voice, the synthetic voice with faults, your file. The Duck Key bus gets silence |
| `tools/DemoKit.h` | Kit file parser: `tempo`, `vocal = file`, `vocals = folder` (gitignored `demo-kit.txt`; never commit sample paths or audio) |
| `tools/Render.cpp` | `vocanal_render in.wav out.wav [id=value ...] [order=...] [render=0|1]`: offline render through the chain, latency removed (before/after WAVs; mp3 first through `afconvert`/`ffmpeg`) |
| `tools/Bench.cpp` | `vocanal_bench [seconds]`: average load and worst block per rate and block size, with and without reorders; exit 1 over budget (48 kHz, 32/64 blocks: average ≤ 25 %, worst ≤ 50 % of the period) |
| `tools/Snapshot.cpp` | `vocanal_snapshot <dir> [state ...]`: renders the editor offscreen to PNGs; `bench` times the paints |

## For AI Agents

### Working In This Directory
- Core loop (seconds): `cmake -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release -DVOCANAL_BUILD_PLUGIN=OFF -DFETCHCONTENT_SOURCE_DIR_CATCH2=$HOME/Projects/psysynth/build-core/_deps/catch2-src && cmake --build build-core && ./build-core/vocanal_tests`
- Full plugin: add `-DFETCHCONTENT_SOURCE_DIR_JUCE=$HOME/Projects/open_music_analyzer/plugin/third_party/JUCE` (FetchContent can't download in the sandbox).
- Never include JUCE from `src/core`. New `.cpp` files go into `CMakeLists.txt` by hand.

### The module contract (`src/core/fx/Module.h`)
- `prepare()` allocates everything for every quality; `process()` never allocates, locks or blocks. `[rt]` counts allocations through reorders.
- **Latency is fixed per rate**, the same at every quality and at rest. At rest a module with latency L is a pure `RingBuffer::delay` of L; a module without latency returns before touching a sample.
- Each detector analyses its own input (it sits anywhere in the order). Slow estimates are learned, then latched into `kLearned` parameters on the message thread, so they are undoable and identical in bounces.
- State copy: big buffers are `RingBuffer`s listed by `rings()` (same sizes and order in both instances). Everything else goes through `copySmallState()`. No pointer into a module's own storage may live in copied state.

### Rules the code keeps
- **Parameter ids, choice lists and module ids are the save format.** Append only; bump the version when a meaning changes. Before v0.1.0 ships they may still change.
- **The order is a ValueTree property**, `order = "clean,breath,..."`, not a host parameter: 11! orders don't fit a float-backed int, and a list loads tolerantly when a module is added. The packed atomic is the truth. Unknown ids are skipped; a missing module goes after its nearest default predecessor.
- **Undo is psysynth-style snapshot history**, pushed on gesture end, card drop, preset load and session load. **Never give the APVTS an UndoManager**: it would record host automation.
- **Presets, undo and session loads are transactions** (`Change`, `openChanges` before `patchEpoch`). While a transaction is open the audio thread holds the last whole sound (at most 1 s), then crossfades: A keeps the old knobs, B fades in on the new ones.
- **Host bypass** goes through the chain's latency-aligned, faded bypass (`setHostBypass`), never JUCE's undelayed default.
- **The duck key is delayed by the latency upstream of the reading module's slot**, so the key lines up with the voice that module hears.

### Testing Requirements
- After core changes, `./build-core/vocanal_tests` must pass. It covers:
  - bit-exact rest at 44.1/48/96 kHz;
  - the latency in every one of the 110 single moves;
  - no click across every move, with a control test proving the detector catches a torn echo;
  - queued and latest-wins requests, prepare mid-fade, new-sound crossfades;
  - Mix 0, bypass and host bypass bit-exact (delayed);
  - duck key alignment, block-size invariance (1/37/64/4096), stress up to 192 kHz;
  - zero allocations;
  - order parse and pack, parameter laws, the synth voice's ground truth;
  - `[dsp]` the copied primitives; `[shift]` rest, interval, formants, loudness, latency honesty, block invariance, state copy, clicks; `[smooth]` rest, min-phase phase (< 0.05 rad), noise and broad tone left alone, sine and ring cuts, Sharp widths, lookahead/attack order, Listen, copies, clicks.
- Every module has its own `[clean]`/`[breath]`/`[comp]`/`[deess]`/`[warmth]`/`[air]`/`[radio]`/`[double]`/`[delay]`/`[reverb]` cases: rest bit-exact, the plan's targets, negative controls, block invariance, state copy, clicks.
- CPU note: during a reorder fade both copies of the suffix run on the same stage schedule, so at 192 kHz with 32-sample blocks a block can overrun (~150 %); 48 kHz stays under 25 %.
- After DSP changes, run `./build-core/vocanal_bench` (exit 0 = within budget).
- After UI changes, run the snapshot loop and read the PNGs.
- Before a release: `auval -v aufx Vcnl Jgru` and pluginval at strictness 5 on both formats. The AU warning "Disabling non-main buses failed" is harmless (the sidechain can't be switched off).

### Common Patterns
- Namespaces: `voc` (core), `voc::dsp` (primitives), `theme` (UI tokens). Plugin/UI classes are global.
- JUCE-style formatting: space before call parens `foo (x)`, 4-space indent, braces on their own line.
- **Look:** night sheet `#131211`, ink `#ECE7DB`, accent **ultraviolet `#C4A4FE`**. It means "what Vocanal is doing to the voice right now" (removed bands, gain reduction, acting knob rings); a module at rest steps back to the inks. IBM Plex Sans Condensed + Plex Mono, flat, 2 px radius.

### Clean room
- Valhalla Delay / VintageVerb (official installs): black-box measurement only.
- Fresh Air / soothe2: the installed copies don't come from official packages. Don't measure them; Fresh Air is free from Slate, so measure only an official copy.
- No decompiled DSP, vendor tables or vendor preset values. No trademarks in the UI or presets. DE-ESS must not use Dolby's patented trigger (US11322170).

## Dependencies
- JUCE 8.0.15 (AGPLv3), Catch2 v3.7.1, C++20, CMake >= 3.24, macOS deployment target 11.0
- IBM Plex Sans Condensed + Plex Mono (OFL), bundled in `resources/fonts`

## Manual Notes

Notes under this heading are written by humans and preserved on regeneration.
