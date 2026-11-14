[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · **Integrations** · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)

# Integrations

## Integrations

### MPRIS

Moroder registers `org.mpris.MediaPlayer2.moroder` on the session bus and
implements the full `MediaPlayer2` and `MediaPlayer2.Player` interfaces:
`Play`, `Pause`, `PlayPause`, `Stop`, `Next`, `Previous`, `Seek`,
`SetPosition`, `Quit`, plus `Metadata`, `Position`, `PlaybackStatus`,
`LoopStatus`, `Shuffle` and the `Can*` properties.

```bash
playerctl -p moroder play-pause
playerctl -p moroder metadata
```

Media keys and desktop widgets work without configuration. Every incoming call
is logged, including refusals, so "the button did nothing" is diagnosable.

`LoopStatus` is readable and writable, and is the same setting the <kbd>r</kbd>
key cycles - a change made either way shows up in the other:

```bash
playerctl -p moroder loop Track
```

MPRIS has no concept of a radio queue, so its `Playlist` is Moroder's queue
loop and its `Track` is the track loop. `Shuffle` is still accepted and
reported, but does nothing yet.

### Discord

Rich presence shows the track as a *Listening* activity with the cover as the
large image and the album as its tooltip. Timestamps track real playback
position, so the progress bar in Discord follows seeks and pauses.

`RICH_PRESENCE_STATUS_LABEL` picks which field is the headline, right under your username.

---

[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · **Integrations** · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)
