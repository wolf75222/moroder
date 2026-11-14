[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · **Internals** · [Troubleshooting](troubleshooting.md)

# Internals

## Architecture

```
src/
├── main.cpp                  entry point, argv dispatch, the render loop
├── setup/setup.cpp           the `moroder setup` command
├── services/
│   ├── player.cpp            orchestration, queue, state
│   ├── music.cpp             YouTube Music, models out of raw JSON
│   ├── mpv.cpp               playback via libmpv
│   ├── mpris.cpp             D-Bus / MPRIS
│   └── social.cpp            Discord rich presence
├── ytm/
│   ├── ytmusic.cpp           InnerTube client: auth, endpoints, continuations
│   ├── parsers.cpp           renderer JSON to flat structures
│   ├── nav.cpp               safe navigation into deeply nested JSON
│   ├── http.cpp              libcurl wrapper
│   ├── metadata.cpp          last.fm / iTunes album lookups
│   └── sha1.cpp              SAPISIDHASH
└── ui/components/            sidebar, search bar, content, grid, carousel,
                              chips, playback bar, error box, image view
include/
├── config/config.hpp         config parsing, writing and defaults
└── utils/utils.hpp           path, string and directory helpers
```

### How a request flows

```
UI ──▶ Player ──▶ command queue ──▶ worker thread
                                        │
                                        ▼
                                     Music ──▶ ytm::YTMusic ──▶ ytm::Http
                                        │
                                        ▼
                                  PlayerState (mutex)
                                        │
                                        ▼
                              screen.PostEvent ──▶ redraw
```

`Player` is the only thing the UI talks to. Every request becomes a command on
a queue, a single worker thread drains it, and results land in `PlayerState`
behind a mutex. The UI copies that state once per render and reads nothing else,
so no UI code ever blocks on the network.

`PlayerState` carries the user queue, the radio queue, the current track, home
and library results, and two maps that drive the interface: `flags` (`Ongoing`,
`Done`, `Error`, `True`, `False`) and `errors`, which is what turns into an
error box with a retry button.

### Threads

| Thread | Job |
|---|---|
| main | ftxui event loop and rendering |
| player worker | drains the command queue, one request at a time |
| position tick | wakes each second to update the progress bar |
| mpv event loop | translates mpv events into stream start/end/error callbacks |
| sdbus | MPRIS method calls |
| discord | runs the Social SDK callbacks |

### Queue and radio

Queueing uses mpv's own playlist with yt-dlp so the next track isn't buffered before the
current one ends, but the queue position advances whether or not you
skipped manually. When the user queue runs out, tracks come from the radio
queue; when that runs low and `autoplay` is on, a continuation is fetched.

Looping sits in front of that. A queue loop wraps the position back to zero
instead of reaching for the radio queue, and because mpv has nothing left to
autoplay into at the end of its playlist, the wrap is an explicit
`playlist-play-index`. A track loop is mpv's own `loop-file`, so a repeat never
unloads the file: no end-of-file event, no gap between plays, and the queue
position stays put.

## Talking to YouTube Music

`ytm::YTMusic` is a C++ implementation of the parts of `ytmusicapi` python library this client
needs. It speaks to InnerTube directly.

- **Auth** - `browser.json` is a flat object of request headers. The cookie
  must carry `__Secure-3PAPISID` (or `SAPISID`), from which each request's
  `Authorization: SAPISIDHASH` is derived.
- **Visitor id** - taken from the pasted headers. If they did not carry one, it
  is scraped from the YouTube Music homepage on first use instead.
- **Endpoints** - `search`, `getHome`, `getAlbum`, `getPlaylist`,
  `getLibraryPlaylists` / `Albums` / `Songs` / `Artists` / `Podcasts`,
  `getWatchPlaylist` and its continuation.
- **Errors** - every failure funnels through one place and is classified as
  `Network`, `Auth`, `RateLimit`, `Server`, `Request`, `Parse`, `NotFound` or
  `Cancelled`. Only some are retryable, which is what decides whether the UI
  offers a Retry button.
- **Parsing** - renderer JSON is navigated through a path-based helper that
  returns null rather than throwing on a missing key. A malformed item is
  skipped and logged, never fatal to the rest of the page.

Album metadata that YouTube Music does not return is looked up from last.fm,
falling back to iTunes.

## Logging

Logs go to `~/.local/state/moroder/logs/moroder.log`, truncated at each start. mpv writes
its own log alongside it.

Default level is `info`. `HTTP:` request lines and `YTM:` per-endpoint lines are
at `debug` - raise the level in `main.cpp` to see them:

```cpp
logger->set_level(spdlog::level::debug);
```

URLs are redacted before being written: the InnerTube key and your last.fm key
both travel in the query string, so both are stripped. The session cookie is
never logged, only a short prefix and its length.


## Credits

The YouTube Music client is a C++ reimplementation of the endpoints and parsers
in [ytmusicapi](https://github.com/sigma67/ytmusicapi). The MPRIS
implementation is adapted from [mpris-server](https://github.com/chrg127/mpris-server).

---

[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · **Internals** · [Troubleshooting](troubleshooting.md)
