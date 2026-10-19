# Vocanal

## Purpose
A whole vocal chain in one psytrance-minded plugin (VST3 / AU, JUCE 8): smart one-knob modules (CLEAN, BREATH, COMP, WARMTH, DE-ESS, AIR, RADIO, DOUBLE), pitch/formant SHIFT, a ducked tempo DELAY and a REVERB, in an order the user drags. SMOOTH (de-harsh) is a planned follow-up: its slot comes after SHIFT, but there is no code until Jonas sends its details. The plan, with the measurable targets, is `~/.claude/plans/i-want-to-build-cosmic-nautilus.md` (milestones M1–M8).

Buses: the voice is on the main input. The optional stereo sidechain "Duck Key" can duck DELAY and REVERB instead of the voice (Duck Key = Sidechain).

Invariants:
- With every knob at its default, the output is the input delayed by the latency, bit for bit.
- The latency is fixed per sample rate: the same in every order, at every quality, and at rest.
- A reorder or a new sound never clicks.

**Status:** M1 (foundation) is done. Every module is still a `Passthrough` placeholder. The chain, order, transitions, parameters, state, undo, sidechain, demo host and bench are in.

## Key Files
| File | Description |
|------|-------------|
| `CMakeLists.txt` | Targets: `vocanal_core` (static lib, no JUCE), the `Vocanal` plugin, `vocanal_snapshot`, `vocanal_demo`, `vocanal_tests`, `vocanal_bench` (core only). Options `VOCANAL_BUILD_PLUGIN`, `VOCANAL_BUILD_TESTS`, `VOCANAL_COPY_PLUGIN` (off in CI). Cache var `VOCANAL_VERSION`. UI sources are listed once in `VOCANAL_UI_SOURCES` |
| `src/core/fx/Params.h/.cpp` | **The parameter table** (X-macro `VOC_PARAMS`): id (the save format), host name, unit, range, default, flags (`kAuto`, `kPreset`, `kLearned`), choice lists. `Settings` = every plain value plus `render`. Knob laws `toNormalised`/`fromNormalised` (log for Hz/ms/s), `offEnd` (where a knob says "Off"). The processor, presets, the undo history and the tests all read it |
| `src/core/fx/Order.h/.cpp` | `ModuleId` (internal index, append only), `kModuleIds` (saved ids, never rename), `kDefaultOrder` with the reasoning, `Order` (pack into a uint64 with 4 bits per slot, `moved`, tolerant `parse`) |
| `src/core/fx/Module.h` | The module contract (see below), `Transport`, `Block` (duck key + transport), `Passthrough` |
| `src/core/fx/Chain.h/.cpp` | The engine: two instances per module, suffix-only reorder, sliced state copy, the 20 ms fade with B's inputs blended from A's, latency-aligned dry for Mix and bypass, Output, the duck key delayed per slot |
| `src/core/dsp/RingBuffer.h` | Pow2 float ring with a 64-bit counter; `delay()` is an exact integer delay; sliced copy (`beginCopy`/`copySlice`/`finishCopy`) that is exact while the source keeps writing |
| `src/core/dsp/Smoother.h` | Smoother / Fade (smoothstep) / SlewPole (kit) |
| `src/plugin/PluginProcessor.cpp` | Host params built from the table (custom `NormalisableRange` = the core's laws; texts with "Off"); transactions (`Change`); the order as state; snapshot undo history; sidechain read; transport (holds the last host BPM); `processBlockBypassed` through the chain's delayed bypass; state with `order` and `stateVersion` |
| `src/plugin/PluginEditor.cpp` | M1 placeholder sheet (Output, Mix). The real UI is M7 |
| `tests/StubModules.h` | Stand-ins with real latency (`Lookahead`), real tails (`Echo`), a shaper, a stateful filter and a key probe; `stubFactory()`, `delayFactory()` |
| `tests/SynthVoice.h` | Synthetic voice with ground truth: labelled segments (vowel, sibilant, fricative, plosive, breath) and an exact vowel envelope, optional boom, harsh ring and loud-ess faults at known Hz |
| `tools/Demo.cpp` | Demo host (`./start.sh [vocal.wav [beat.wav]]`): the voice through the plugin, the beat around it and on the Duck Key bus. MIX (beat delayed by the latency), VOICE, BEAT. Scenarios: synthetic voice, synthetic voice with faults, and every `vocal =` in the local `demo-kit.txt` |
| `tools/DemoKit.h` | Kit file parser: `tempo`, `pattern`, `kick`, `bass`, `vocal` (gitignored `demo-kit.txt`; never commit sample paths or audio) |
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
  - order parse and pack, parameter laws, the synth voice's ground truth.
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
