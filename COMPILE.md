# Building AirScope

AirScope builds on both **Windows** and **Linux**. Windows is the primary
target (it also provides the embedded WebView2 Flight Map); Linux builds are
fully supported minus the WebView2 map, which falls back to a text notice.

- [Building on Windows](#building-on-windows)
- [Building on Linux](#building-on-linux)

# Building on Windows

AirScope is built with the **MSYS2 / MinGW-w64** toolchain (GCC 15.x) using
**CMake** and **Ninja**. The build is reproducible from a clean MSYS2 install.

> Important: GCC must be invoked from inside the **MINGW64** environment. If you
> run `g++.exe` directly from PowerShell/cmd without that environment set up,
> `cc1plus` dies silently with exit code 1 and no diagnostics. Always build from
> the **MSYS2 MINGW64** shell (or set `MSYSTEM=MINGW64` before launching bash).

## 1. Install MSYS2

Download and install from <https://www.msys2.org>, then open the
**"MSYS2 MINGW64"** shell (not the plain "MSYS2 MSYS" shell) from the Start menu.

Update the package database once:

```bash
pacman -Syu
# close and reopen the MINGW64 shell if it asks you to, then:
pacman -Su
```

## 2. Install build tools and dependencies

```bash
pacman -S --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-glib2 \
  mingw-w64-x86_64-glfw \
  mingw-w64-x86_64-rtl-sdr \
  mingw-w64-x86_64-libusb \
  mingw-w64-x86_64-zstd \
  mingw-w64-x86_64-libogg \
  mingw-w64-x86_64-libvorbis \
  mingw-w64-x86_64-sqlite3 \
  mingw-w64-x86_64-libxml2 \
  mingw-w64-x86_64-jansson
```

These provide: GCC/G++, CMake, Ninja, pkg-config, GLFW (windowing), librtlsdr +
libusb (RTL-SDR) and zstd (compression). OpenGL and zlib
ship with the toolchain. libogg + libvorbis provide OGG Vorbis voice recording.
SQLite3 provides the message archive database.  libxml2 is required
by the ACARS application decoder (CPDLC/ADS-C parsing).  GLib 2 provides
headers only, used by the vendored VDL2 decoder (dumpvdl2 core); no GLib
runtime is linked.

All other dependencies (Dear ImGui, ImPlot, libacars,
miniaudio, WebView2 SDK) are vendored in `third_party/` — the repo is fully
self-contained. **No `git submodule` commands are needed.**

### Optional: Airspy support

```bash
pacman -S --needed mingw-w64-x86_64-libairspy
```

Airspy headers are vendored in `third_party/airspy/`. The build automatically
enables Airspy (`HAS_AIRSPY=1`) when libairspy is found by CMake.

## 3. Configure and build

From the **MINGW64** shell, in the project root:

```bash
cmake -S . -B build -G Ninja
ninja -C build
```

The executable and the runtime DLLs it needs are placed in `build/`:

```
build/AirScope.exe
build/libgcc_s_seh-1.dll, libwinpthread-1.dll, libstdc++-6.dll,
      glfw3.dll, librtlsdr.dll, libusb-1.0.dll,
      libzstd.dll, zlib1.dll, libogg-0.dll, libvorbis-0.dll,
      libvorbisenc-2.dll, libsqlite3-0.dll,
      libxml2-16.dll, libiconv-2.dll, liblzma-5.dll,
      libjansson-4.dll, WebView2Loader.dll
```
(plus `build/libairspy.dll` when Airspy support is enabled)

The DLLs are copied next to the `.exe` automatically (POST_BUILD step), so it
runs standalone from a double-click or from PyCharm without MSYS2 on `PATH`.

Run it:

```bash
./build/AirScope.exe
```

## 4. Building from PowerShell (optional)

If you prefer to drive the build from PowerShell, you must enter the MINGW64
environment first. For example:

```powershell
$env:MSYSTEM = "MINGW64"
$env:CHERE_INVOKING = "1"
& C:\msys64\usr\bin\bash.exe -lc "cd /c/path/to/AirScope && ninja -C build"
```

The `-l` (login) shell with `MSYSTEM=MINGW64` sources `/etc/profile`, which puts
`/mingw64/bin` on `PATH` so `cc1plus` can find its runtime. `CHERE_INVOKING=1`
keeps the current directory.

## Notes / troubleshooting

- **First build is slow (~1 min on `implot_items.cpp`).** ImPlot's
  `implot_items.cpp` is extremely template-heavy; at `-O2` it takes ~5 minutes
  to compile. CMakeLists.txt overrides it to `-O1` (last `-O` flag wins) so it
  builds in about a minute. This is intentional, not a hang.
- **`cc1plus` exits 1 with no output.** You are not in the MINGW64 environment.
  Build from the MSYS2 MINGW64 shell (see the warning above).
- **Link fails with "Access is denied" on `AirScope.exe`.** A previous instance
  is still running and is locking the file. Close it, then rebuild.
- **Incremental builds.** Re-run `ninja -C build`. Only changed files recompile.
  After editing `CMakeLists.txt`, re-run `cmake -S . -B build` first.
- **Clean rebuild.** Delete the `build/` directory and re-run the configure +
  build steps. (Avoid `--clean-first`; it wipes the vendored objects and forces
  the slow `implot_items.cpp` recompile.)
```

# Building on Linux

AirScope builds natively on Linux with **GCC/Clang**, **CMake**, and
**Ninja**. The build uses the same `CMakeLists.txt` as Windows; the WebView2
Flight Map is Windows-only and is automatically skipped — the Flight Map panel
is hidden entirely on Linux. Everything else (decoding, voice,
spectrum/waterfall, web dashboard, SBS output) works.

## 1. Install build tools and dependencies

On **Debian / Ubuntu**:

```bash
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libglib2.0-dev \
  libglfw3-dev libgl1-mesa-dev \
  librtlsdr-dev libusb-1.0-0-dev \
  libzstd-dev zlib1g-dev \
  libogg-dev libvorbis-dev \
  libsqlite3-dev libxml2-dev libjansson-dev
```

On **Arch / Manjaro**:

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf \
  glib2 glfw rtl-sdr libusb \
  zstd zlib libogg libvorbis \
  sqlite libxml2 jansson
```

On **Fedora**:

```bash
sudo dnf install -y \
  gcc-c++ cmake ninja-build pkgconf-pkg-config \
  glib2-devel \
  glfw-devel mesa-libGL-devel \
  rtl-sdr-devel libusb1-devel \
  libzstd-devel zlib-devel \
  libogg-devel libvorbis-devel \
  sqlite-devel libxml2-devel jansson-devel
```

As on Windows, all remaining dependencies (Dear ImGui, ImPlot, libacars,
miniaudio) are vendored in `third_party/` — no submodules
needed.

### Optional: Airspy support

Install `libairspy-dev` (Debian/Ubuntu), `airspy` (Arch), or `airspy-devel`
(Fedora). The build enables Airspy (`HAS_AIRSPY=1`) automatically when libairspy
is found by CMake.

### Optional: SDRplay support

Install the SDRplay API 3.x (`libsdrplay_api` + the `sdrplay` service).
Headers are found in `/usr/include` and the library is linked automatically
(`HAS_SDRPLAY=1`) when `sdrplay_api.h` and `libsdrplay_api` are present.
The `sdrplay` service must be running to open an RSP device.

## 2. Configure and build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Run it from the project root (so it finds the bundled font under
`third_party/imgui/misc/fonts/`):

```bash
./build/AirScope
```

## Notes / troubleshooting

- **First build is slow (~1 min on `implot_items.cpp`).** Same template-heavy
  file as on Windows; CMakeLists.txt already caps it at `-O1`.
- **No Flight Map.** Expected — WebView2 is Windows-only, so the Flight Map tab
  is hidden on Linux. The rest of the app is unaffected.
- **RTL-SDR permissions.** Install the vendors' udev rules (e.g.
  `/etc/udev/rules.d/`) or run as a user in the `plugdev` group so the device is
  accessible without root.
