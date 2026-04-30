# OpenAL Proxy with Steam Audio HRTF

OpenAL32.dll proxy DLL that integrates Steam Audio binaural HRTF into Lineage 2 (UE2.5, 32-bit Windows).

## How it works

```
L2.exe → ALAudio.dll → OpenAL32.dll (our proxy) → wrap_oal.dll (Creative OpenAL)
                                    ↓
                              phonon.dll (Steam Audio)
```

The proxy intercepts `alSourcePlay` calls, converts MONO16 audio buffers to STEREO16 with Steam Audio's DirectEffect (distance attenuation) + BinauralEffect (HRTF spatialization), then replaces the source buffer before playback. This gives proper 3D positional audio through headphones without modifying the game engine.

## Features

- **HRTF binaural audio** — Steam Audio BinauralEffect for realistic 3D positioning
- **Distance attenuation** — DirectEffect with configurable 1/dist falloff
- **Air absorption** — frequency-dependent distance attenuation
- **Raycast occlusion** — Steam Audio IPLScene with OBJ geometry
- **STEREO16 support** — automatically downmixes stereo buffers to mono for HRTF processing
- **Live config reload** — edit `LineageAudio.ini` and press RightCtrl in-game
- **Hot toggle** — RightAlt to enable/disable HRTF processing
- **Window title HUD** — shows proxy status, volume, blend settings

## Installation

1. Place files in your game's `system/` directory:

| File | Purpose |
|------|---------|
| `OpenAL32.dll` | The proxy (replaces original) |
| `wrap_oal.dll` | Original Creative OpenAL backend (keep existing) |
| `phonon.dll` | Steam Audio 4.4.0 (32-bit) |
| `LineageAudio.ini` | Configuration file |
| `de_20_18_offset.obj` | Scene geometry for occlusion (optional) |

2. The proxy automatically finds `wrap_oal.dll` (or `DefOpenAL32.dll`) as the real OpenAL backend.

3. If `phonon.dll` is missing, the proxy operates in passthrough mode (no HRTF).

## Configuration — LineageAudio.ini

```ini
[HRTF]
Enabled=1
SpatialBlend=1.0      ; 0=dry mono, 1=full HRTF spatialization
VolumeBoost=1.5       ; output volume multiplier (1.0=normal)
GainMaster=1.0        ; AL_GAIN multiplier

[Direct]
Enabled=1             ; enable DirectEffect (distance + air absorption)
DistAtten=1.0         ; distance attenuation strength (0=disable)
AirAbsorb=0.0         ; air absorption amount (0=off, 1=full)
MinDistance=1.0       ; minimum distance for attenuation clamp
Occlusion=1.0         ; occlusion amount (0=off, 1=full)

[HUD]
Enabled=1             ; show proxy status in window title
```

## Hotkeys

| Key | Action |
|-----|--------|
| RightAlt | Toggle HRTF on/off |
| RightCtrl | Reload LineageAudio.ini config |

## Building

Requires MinGW 32-bit cross-compiler:

```bash
i686-w64-mingw32-g++ -O2 -Wall -std=c++17 -DWIN32_LEAN_AND_MEAN \
  -shared -static-libgcc -static-libstdc++ \
  -o OpenAL32.dll openal_proxy.cpp \
  -lkernel32 -luser32
```

## Steam Audio SDK

Requires `phonon.dll` — Steam Audio API v4.4.0, 32-bit (PE32). This is a custom Valve build available in various Source engine games. The proxy loads it dynamically at runtime via `LoadLibraryA`.

## Supported Clients

- Lineage 2 Interlude / Kamael / Freya (UE2.5, 32-bit)
- Wine + DXVK on Linux

## Notes

- No EAX patching required — the proxy works independently of ALAudio.dll's EAX path
- If `phonon.dll` is not found, audio passes through unchanged
- OBJ scene files are optional — without them, occlusion is disabled but HRTF still works
- The proxy is 32-bit only (PE32) — matches the game executable
