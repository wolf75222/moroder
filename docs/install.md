[README](../README.md) · **Install** · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)

# Install

## One command

```bash
curl -fsSL https://raw.githubusercontent.com/wolf75222/moroder/main/install.sh | sh
```

- macOS (Apple silicon): installs `mpv`, `yt-dlp`, `jpeg-turbo` and `libpng`
  with Homebrew, then the latest release's build (its checksum is checked)
- Other Macs and Linux: builds the latest release from source (Linux: into
  `~/.local`)
- Linux: finds what the build lacks and offers to install it with your
  package manager (`pacman` on Arch, `dnf` on Fedora, `zypper` on openSUSE,
  `apt` on Debian/Ubuntu/Mint), showing the exact command first. On
  Debian/Ubuntu yt-dlp gets its own up-to-date Python environment, since the
  distribution's package lags behind YouTube
- Puts `moroder` on your `PATH` (zsh, bash, fish or `~/.profile`, if needed)
- Shows what it installs and where, with the independent steps running side
  by side (dependencies and download, or dependencies and source)

Options: `MORODER_VERSION=1.2.0` installs a given version,
`MORODER_FROM_SOURCE=1` always builds, `MORODER_PREFIX=~/opt` installs
elsewhere, `MORODER_NO_PATH=1` leaves your shell files alone,
`MORODER_SOURCE=~/moroder` builds a local checkout.

## Windows

```powershell
irm https://raw.githubusercontent.com/wolf75222/moroder/main/install.ps1 | iex
```

- Downloads the release's Windows build (checksum verified): `moroder.exe`
  with libmpv and the DLLs it needs, into `%LOCALAPPDATA%\Programs\moroder`
- Adds `yt-dlp.exe` next to it when yt-dlp isn't installed, and the folder
  to your user `PATH`
- Best in Windows Terminal (colours, mouse, Unicode)
- Settings and session live in `%USERPROFILE%\.config\moroder`, as on the
  other systems
- `moroder setup` reads Firefox, Zen and LibreWolf sessions directly. Chrome
  and Edge lock their cookies on Windows (app-bound encryption): pick
  **New window** in setup, sign in there once, and the browser hands the
  session over itself
- Media keys (play / pause, next, previous) work wherever the focus is;
  notifications are Windows toasts

Built with MSYS2 (UCRT64); `.github/workflows/windows.yml` shows the steps.

## Updates

At launch, at most once an hour, Moroder asks GitHub for the latest release
(nothing about you is sent). When a newer one is out it asks
`Update now? [Y/n]` before starting: yes installs it and restarts into it; no
is remembered for that version. `moroder update` installs the latest anytime
and also updates yt-dlp. `CHECK_FOR_UPDATES = false` in the config turns the
check off.

`moroder doctor` checks yt-dlp, mpv, sign-in, the network, the terminal and updates, all at once;
`moroder uninstall` removes Moroder (and, if you say so, your settings).

## Requirements

### Build

| Dependency | Purpose |
|---|---|
| C++17 compiler | GCC or Clang |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | terminal UI |
| `ftxui-grid-container` | grid layout for the library and Quick Picks |
| libmpv | playback |
| libcurl | HTTP |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON |
| [spdlog](https://github.com/gabime/spdlog) | logging |
| [sdbus-c++](https://github.com/Kistler-Group/sdbus-cpp) | MPRIS over D-Bus |
| Discord Social SDK (`discordpp`) | rich presence |

### Runtime

| Program | Purpose | Required |
|---|---|---|
| `mpv` | plays the audio | yes |
| `yt-dlp` | resolves YouTube streams, mpv calls it internally | yes |
| `xdg-open` | opens the browser during setup | optional |

`moroder setup` checks for these and tells you what is missing.

> **Note**
> Moroder targets Linux and macOS. On macOS, MPRIS (D-Bus) is replaced by the
> native equivalent: Now Playing in Control Center and the menu bar, plus the
> media keys. Discord rich presence works on both, given the Discord Social SDK
> in `lib/`.

## Installation

### Arch Linux

Moroder is available as a prebuilt package for Arch Linux via the AUR:

```bash
yay -S moroder-bin
```


If you want to build Moroder from source, see [Building](#building).

## Building

Building from source requires the [Discord Social SDK](https://docs.discord.com/developers/discord-social-sdk/getting-started/using-c++) to be placed at:

```text
./lib/discord_social_sdk/
```

The SDK is not included in this repository.

The expected directory structure is:

```text
./lib/
└── discord_social_sdk/
    ├── include/
    │   ├── cdiscord.h
    │   └── discordpp.h
    └── lib/
        ├── debug/
        └── release/
```

Without the Discord Social SDK in `./lib/`, **building from source is not possible**.

If you do not have the SDK, **prebuilt binaries are available from the GitHub Releases**.

Once the SDK is in place, build Moroder after cloning the repository with:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
```

The default config is embedded into the binary at build time: `moroder.conf` is compiled into `build/gen/defaults.hpp` as a byte array, which is what `config.hpp` writes out on first run. If you edit the default config, the header will be automatically regenerated.

### Arch Linux

Install the required build dependencies with:

```bash
sudo pacman -S --needed base-devel cmake mpv curl yt-dlp
```

### Debian Linux

Moroder depends on:

* `libmpv-dev` or `mpv`
* `libcurl-dev` (virtual package; see the Linux Mint instructions below)
* `cmake`
* `yt-dlp` **(latest version required)**
* `sdbus-c++`
* `libsystemd-dev`
* `pkg-config`

#### Linux Mint

On Linux Mint, install the required development libraries and build dependencies with:

```bash
sudo apt update
sudo apt install cmake mpv libmpv-dev libcurl4-openssl-dev libsystemd-dev pkg-config
```

Moroder also requires **the latest version of `yt-dlp`**. The version available in Linux Mint's repositories may be outdated, so install or upgrade `yt-dlp` using the official recommended installation method rather than relying on the Mint repository version.

You can verify your installed version with:

```bash
yt-dlp --version
```

> **Note:** `libcurl-dev` is a virtual package on Debian/Ubuntu-based distributions. On Linux Mint, `libcurl4-openssl-dev` provides the required `libcurl` development files.

After installing the dependencies, follow the standard CMake build instructions.

---

### macOS

```bash
brew install cmake pkg-config mpv yt-dlp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On macOS, MPRIS is replaced by Now Playing: the current track, its cover and
progress show in Control Center and the menu bar, and the media keys, AirPods
and Control Center buttons drive play/pause, next/previous, scrubbing and repeat.

Stream cookies: on macOS, yt-dlp is given the session saved by `moroder setup`
(exported to `~/.config/moroder/cookies.txt`) instead of reading a browser's
cookie store, which macOS protects. Any browser works for setup, Arc and
Safari included; Chromium's "Copy as cURL" can be piped straight in:
`pbpaste | moroder setup`.

Discord rich presence is built when the macOS SDK is in place, at
`lib/discord_social_sdk/lib/release/libdiscord_partner_sdk.dylib`, just like
the `.so` on Linux (`-DMORODER_ENABLE_DISCORD=OFF` to build without it).

---

[README](../README.md) · **Install** · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)
