# AirScope

One desktop application for VHF airband monitoring: **ACARS**, **voice**, and
**1090 MHz ADS-B** decoded together, with aircraft locations on a live map.

AirScope is built on the former InmarScope codebase (an Inmarsat satellite
decoder). It is being retargeted to the VHF airband and ADS-B. Three fixed
receiver roles run concurrently, each filled by any supported SDR chosen by
the user:

- **Receiver 1 — Voice**: airband AM voice, with the only interactive
  spectrum/waterfall.
- **Receiver 2 — ACARS/DATA**: VHF ACARS (2400 bps MSK), runs headless.
- **Receiver 3 — ADS-B**: 1090 MHz Mode-S, decoded with algorithms ported from
  [goadsb](https://github.com/SarahRoseLives/goadsb); runs headless.

Each role can use an **Airspy**, **SDRplay** (RSP1A/RSP2/RSPduo/RSPdx),
**RTL-SDR**, or a **WAV** file.

> Status: **early development.** The inherited InmarScope codebase is being
> retargeted: the Inmarsat Aero/EGC/AMBE satellite path has been removed, and
> the source layer now runs several receivers concurrently (RTL-SDR, Airspy,
> SDRplay and WAV). The VHF ACARS, AM voice, and ADS-B decoders are being
> ported in phases.

## Planned features

- **VHF ACARS** — 2400 bps MSK demodulation and ARINC-618 framing, with
  application decoding (CPDLC / ADS-C / MIAM) via libacars
- **VHF voice** — AM demodulation, squelch, live listen and WAV/OGG recording
- **1090 ADS-B** — Mode-S demodulation, CRC correction, DF17/18 decoding, and
  CPR position tracking
- **Live spectrum & waterfall** for voice channel placement
- **Unified aircraft list & map** merging ACARS ADS-C and ADS-B positions
- **SBS/BaseStation + Beast output** for virtual radar clients
- **Persistent message archive** (SQLite) and web dashboard

## Building

AirScope supports Windows (MSYS2 MINGW64) and Linux (Debian/Ubuntu, Arch,
Fedora). See [COMPILE.md](COMPILE.md).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/AirScope
```

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
The ported `goadsb` ADS-B code is MIT-licensed and compatible with the GPL.
