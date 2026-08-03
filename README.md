# Changelog - wzAudio

All notable changes to this project are documented in this file. This project is a custom XAudio2 + X3DAudio-based audio engine built on top of the Webzen (MU Online) client.

---

## - Final Breakthrough & Stabilization
### Added
- **Hybrid File-Stealing Wrapper:** Intercepted the static Webzen filename buffer (`0x0095C918`) within the DirectSound IAT Hook.
- **XAudio2 Stereo Bus Routing:** Muted the native mono mixing and rerouted all requested `.wav` files directly to the modern stereo pipeline.

### Fixed
- **Background Music (BGM) Restore:** Eliminated the memory integrity protection triggers caused by the previous inline hook; background music is now fully functional.
- **Client Stability:** Achieved 100% crash-proof operation with zero conflicts regarding Webzen's built-in memory protection mechanisms.

---

## [3.4.x] - Architecture Evolution
### Added
- **XAudio2 Voice Pool:** Implemented 64 dynamic channels. Audio clips no longer interrupt each other; they play on separate tracks as unique emitters with independent panning.
- **Forced Stereo Mastering Voice:** Forced the Windows Audio Engine to provide a clean, hardware-backed 2-channel stereo buffer.

### Removed
- **Inline Assembly Hook (0x0067A3AD):** The naked hook on Webzen's internal `PlayBuffer` function was discarded, as it triggered memory protection and disabled background music.

---

## [3.1.x] - Core Breakthroughs
### Added
- **VirtualQuery-Based IsReadable():** Replaced `IsBadReadPtr`, successfully eliminating all INT 29 crashes triggered by the Watson error handler.
- **Listener Positioning (idx=1):** Utilized the `idx=1` mob ID as a reference point for 3D positioning, since its coordinates consistently track the player's movement.
- **FFmpeg Mono Conversion:** Batch-converted all audio files to mono, as true X3DAudio spatial positioning strictly requires mono sound sources.

---

## - Initial State & Legacy Issues
### Identified
- **Memory Read Failure:** The player object is completely missing from the `OBJECT_LIST_BASE` structure, making the player's X/Y coordinates unstable and unreadable.
- **Critical Crashes:** Continuous `main.exe` crashes due to legacy memory pointer validation (`IsBadReadPtr`).
- **Mono Bottleneck:** The native game engine downmixes every sound effect into a single mono track before passing it to DirectSound, completely breaking 3D spatial audio.
