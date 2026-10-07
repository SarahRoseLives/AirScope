# AirScope

One desktop app for VHF airband monitoring and 1090 MHz ADS-B. It runs three
receivers at once, each filled by whichever SDR you plug in:

| Receiver | Role | Notes |
|---|---|---|
| 1 | **Voice** | Airband AM voice — the only one with a live spectrum/waterfall |
| 2 | **ACARS** | VHF ACARS (2400 bps MSK + ARINC-618) — headless |
| 3 | **ADS-B** | 1090 MHz Mode-S / ADS-B — headless |

Any role can use an **RTL-SDR**, **Airspy**, **SDRplay** (RSP1A/RSP2/RSPduo/
RSPdx), or a **WAV** file. Roles you don't need (2 and 3) can be set to
**Disabled**.

> **Official Windows builds are published at
> [sarahsforge.dev/products/airscope](https://sarahsforge.dev/products/airscope)
> — that is the only place to get them.**

Built in C++17 with Dear ImGui / ImPlot and OpenGL. AirScope started life as
InmarScope (an Inmarsat decoder); the satellite path has been removed and the
code retargeted to the airband and ADS-B.

## Features

- **ACARS** decoding with application layer output (CPDLC / ADS-C / MIAM) via
  libacars
- **AM voice** with squelch, live listening and WAV/OGG recording
- **Voice scanner** — watches the spectrum and automatically opens a decoder on
  each new voice call, following the active one and closing it when it ends
- **ADS-B** — Mode-S demodulation, 1/2-bit CRC repair, DF17/18 decoding and CPR
  position tracking
- **Aircraft table and live map**, merged from ACARS and ADS-B positions
- **Beast** and **SBS/BaseStation** output for virtual-radar clients
- **Persistent archive** (SQLite) and a built-in web dashboard

## Building

Needs CMake, Ninja and a C++17 compiler; see [COMPILE.md](COMPILE.md) for the
per-platform dependencies (librtlsdr, libacars, and optionally libairspy /
sdrplay).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/AirScope
```

## Using it

1. Pick a source for each receiver (or set 2/3 to *Disabled*).
2. Press **Start**.
3. On the Voice receiver, **Ctrl+click the spectrum** to add a channel, or turn
   on the **Voice Scanner** to find calls automatically.

Settings (tuning, gain, squelch, scanner, output, layout…) are saved to
`airscope.ini` and restored on restart.

## Credits & license

AirScope is **GPLv3** — see [LICENSE](LICENSE). The ADS-B decoder is ported
from [goadsb](https://github.com/SarahRoseLives/goadsb) (MIT, itself derived
from dump1090) and the ACARS decoder from acarsdec; both are GPL-compatible.
