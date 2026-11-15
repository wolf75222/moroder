[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · **Configuration** · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)

# Configuration

## Configuration

`~/.config/moroder/moroder.conf`, written on first run.

```ini
KEY = value        # text and paths in quotes, # starts a comment
```

Delete any line to fall back to its built-in default. Unknown keys are ignored,
so a config from an older build still parses.

### Account

| Key | Default | Meaning |
|---|---|---|
| `BROWSER` | `"firefox"` | which browser holds your session |
| `YTM_COOKIES_PATH` | `"~/.config/moroder"` | directory holding `browser.json` |
| `MPV_COOKIES_PATH` | `""` | browser profile directory, for stream cookies |
| `LASTFM_API_KEY` | `""` | optional, improves album lookups |

`MPV_COOKIES_PATH` is the directory holding `cookies.sqlite`, not a cookie
file. It is combined with `BROWSER` into a `--cookies-from-browser` spec for
yt-dlp. Required for the Firefox forks, which are indistinguishable from
Firefox by name alone; stock Firefox and the Chromium browsers find their
default profile without it. Without working stream cookies, age-restricted and
premium tracks will not play. [More on passing cookies to yt-dlp](https://github.com/yt-dlp/yt-dlp/wiki/FAQ#how-do-i-pass-cookies-to-yt-dlp)

`BROWSER` and `MPV_COOKIES_PATH` are both ignored in `--anonymous` mode.

Album names come from last.fm when a key is present and iTunes when it is not.
A free key: <https://www.last.fm/api/account/create>

### Playback

| Key | Default | Meaning |
|---|---|---|
| `FETCH_ALBUMS` | `true` | look up album details for tracks that arrive without them |
| `SEARCH_RESULT_LIMIT` | `40` | results per search |
| `RADIO_RESULT_LIMIT` | `50` | tracks per radio continuation |
| `NORMALIZE_VOLUME` | `true` | level tracks with YouTube's loudness figure (loud masters come down) |
| `ICONS` | `"auto"` | `"nerd"`: Nerd Font glyphs; `"plain"`: symbols any font has; `"auto"`: plain on Windows outside the Moroder profile (or a console window set to a Nerd Font), nerd elsewhere (`MORODER_ICONS` overrides) |
| `FADE_SECONDS` | `0` | fade out the end of a track and fade in the next one (0: off) |
| `LASTFM_API_SECRET` | `""` | with `LASTFM_API_KEY`, enables scrobbling (`moroder lastfm` once) |

`FETCH_ALBUMS` costs an extra request per track. Turn it off if queueing feels
slow.

### Appearance

| Key | Default | Meaning |
|---|---|---|
| `HOME_ORDER` | see below | home shelves in display order, case-insensitive |
| `EXTRA_BOTTOM_PADDING` | `false` | extra line under the player, for terminals that clip the last row |

```ini
HOME_ORDER = "from your library, quick picks, forgotten favorites, listen again, morning sunshine"
```

Anything not listed follows in the order YouTube Music sent it.

#### Colours

Seven colours are configurable, as `{r, g, b}` with each channel `0-255`. A
malformed or out of range value keeps the built-in colour, so a typo dims one
element rather than leaving the interface unreadable - the log names the key
that was rejected. Truecolour terminal required.

| Key | Default | Where it shows |
|---|---|---|
| `COLOR_ACCENT_PRIMARY` | `{255, 0, 0}` | progress bar, selected filter chips |
| `COLOR_TEXT_TOP_PRIMARY` | `{255, 255, 255}` | title of the item under the cursor |
| `COLOR_TEXT_TOP_SECONDARY` | `{170, 170, 170}` | every other title |
| `COLOR_TEXT_BOTTOM_PRIMARY` | `{170, 170, 170}` | second line, under the cursor |
| `COLOR_TEXT_BOTTOM_SECONDARY` | `{70, 70, 70}` | second line, everything else |
| `COLOR_SEPARATOR_PRIMARY` | `{100, 100, 100}` | separators, focused chip outlines |
| `COLOR_SEPARATOR_SECONDARY` | `{50, 50, 50}` | unfocused chip outlines |

The remaining colours - the success and error accents, and plain black and
white - are fixed.

### Cache

Counts, not bytes. Raise them if covers you have already seen get fetched
again, lower them on a memory-constrained machine.

| Key | Default | Meaning |
|---|---|---|
| `MAX_IMAGE_CACHE_SIZE` | `800` | full-size covers held in memory |
| `MAX_RESIZED_IMAGE_CACHE_SIZE` | `3200` | resized variants |
| `MAX_IMAGE_CHAR_CACHE_SIZE` | `800000` | rendered character-art frames |
| `MAX_CONCURRENT_IMAGE_LOADS` | `200` | cover downloads running at once |

### Discord

| Key | Default | Meaning |
|---|---|---|
| `ENABLE_DISCORD_RICH_PRESENCE` | `true` | show what you are listening to |
| `RICH_PRESENCE_STATUS_LABEL` | `"song"` | headline field: `song`, `artist` or `album` |

When disabled, no Discord client is created and no background thread runs.

---

[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · **Configuration** · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)
