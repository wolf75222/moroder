<div align="center">

# Moroder

**A YouTube Music player for the terminal**<br>
driven by the keyboard, the mouse or the trackpad, on macOS and Linux

[![Release](https://img.shields.io/github/v/release/wolf75222/moroder?color=e53935)](https://github.com/wolf75222/moroder/releases/latest)
![macOS](https://img.shields.io/badge/macOS-Apple%20silicon-555)
![Linux](https://img.shields.io/badge/Linux-x86__64-555)
![C++17](https://img.shields.io/badge/C%2B%2B-17-555)

<img src="img/app.gif" alt="Moroder: home, search, playback, lyrics, playlists" width="760">

<sub>A fork of <a href="https://github.com/orrnithogalum/moroder">orrnithogalum/moroder</a>, ported to macOS and reworked to feel natural.</sub>

<br>

**Fast** · starts in a few seconds, at once for the song you selected, gapless, every track at the same loudness<br>
**Your music** · home, library, search with suggestions, artists, albums, playlists you can edit, radio, a queue you can drag<br>
**On screen** · album covers in the terminal, lyrics that fill in word by word as they are sung<br>
**Natural** · click, right-click, scroll, swipe, back / forward, volume slider, sleep timer, Last.fm, media keys

</div>

## Install

macOS, with Homebrew (or grab the [release](https://github.com/wolf75222/moroder/releases/latest)):

```bash
brew install cmake pkg-config mpv yt-dlp
git clone https://github.com/wolf75222/moroder.git && cd moroder
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake --install build --prefix /opt/homebrew
```

Linux and other options: [Install](docs/install.md).

## Start

```bash
moroder setup    # sign in once, with your browser's YouTube Music session
moroder
```

<div align="center">

| Key | Does |
|:--|:--|
| <kbd>Enter</kbd> / double-click | play |
| <kbd>Space</kbd> | play / pause |
| <kbd>⇧</kbd>/<kbd>⌥</kbd> + <kbd>←</kbd> <kbd>→</kbd> · <kbd>&lt;</kbd> <kbd>&gt;</kbd> | previous / next track |
| <kbd>/</kbd> | search |
| <kbd>v</kbd> | Now Playing and lyrics |
| <kbd>Esc</kbd> · <kbd>]</kbd> | back · forward |
| right-click | everything else |
| hold <kbd>h</kbd> · <kbd>?</kbd> | all the keys |

<br>

[Install](docs/install.md) · [Usage](docs/usage.md) · [Configuration](docs/configuration.md) · [Integrations](docs/integrations.md) · [Internals](docs/internals.md) · [Troubleshooting](docs/troubleshooting.md)

<br>

<sub>
Fork of <a href="https://github.com/orrnithogalum/moroder"><b>orrnithogalum/moroder</b></a>, the original Moroder.<br>
YouTube Music client reimplemented from <a href="https://github.com/sigma67/ytmusicapi">ytmusicapi</a> · MPRIS adapted from <a href="https://github.com/chrg127/mpris-server">mpris-server</a>
</sub>

</div>
