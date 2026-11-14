<div align="center">

# Moroder

**YouTube Music in your terminal.**<br>
Keyboard, mouse or trackpad. macOS and Linux.

<a href="https://github.com/wolf75222/moroder/releases/latest"><img alt="Release" src="https://img.shields.io/github/v/release/wolf75222/moroder?style=for-the-badge&logo=github&logoColor=white&label=release&labelColor=161b22&color=e53935"></a>
<img alt="macOS" src="https://img.shields.io/badge/macOS-161b22?style=for-the-badge&logo=apple&logoColor=white">
<img alt="Linux" src="https://img.shields.io/badge/Linux-161b22?style=for-the-badge&logo=linux&logoColor=white">
<img alt="YouTube Music" src="https://img.shields.io/badge/YouTube%20Music-161b22?style=for-the-badge&logo=youtubemusic&logoColor=FF0000">

<img src="docs/assets/app.gif" alt="Moroder playing Get Lucky with synced lyrics" width="760">

Fast, gapless playback · synced lyrics · album art<br>
Playlists, radio, search, queue · clicks, swipes, menus

</div>

## Install

**1.** Install (macOS or Linux):

```bash
curl -fsSL https://raw.githubusercontent.com/wolf75222/moroder/main/install.sh | sh
```

**2.** Sign in with your browser, once ([guide](docs/sign-in.md)):

```bash
moroder setup --browser chrome    # or arc, brave, edge, firefox, safari...
```

Or skip it and play without an account (no library):

```bash
moroder -a
```

**3.** Play:

```bash
moroder
```

New releases are offered at launch (`moroder update` anytime, `moroder doctor` if something is off).

<div align="center">

## Keys

| Key | Does |
|:--|:--|
| <kbd>Enter</kbd> · double-click | play |
| <kbd>Space</kbd> | play / pause |
| <kbd>&lt;</kbd> <kbd>&gt;</kbd> · <kbd>⇧</kbd> + <kbd>←</kbd> <kbd>→</kbd> | previous / next |
| <kbd>/</kbd> | search |
| <kbd>v</kbd> | lyrics |
| <kbd>Esc</kbd> · <kbd>]</kbd> | back · forward |
| right-click | everything else |
| hold <kbd>h</kbd> | all the keys |

<br>

[Install](docs/install.md) · [Sign in](docs/sign-in.md) · [Usage](docs/usage.md) · [Configuration](docs/configuration.md) · [Integrations](docs/integrations.md) · [Internals](docs/internals.md) · [Troubleshooting](docs/troubleshooting.md)

<br>

<sub>Fork of <a href="https://github.com/orrnithogalum/moroder">orrnithogalum/moroder</a>, ported to macOS and enhanced</sub>

</div>
