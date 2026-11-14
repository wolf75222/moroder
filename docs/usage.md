[README](../README.md) · [Install](install.md) · **Usage** · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)

# Usage

## Getting started

```bash
moroder setup     # sign in, once
moroder           # start the player
```

`setup` writes two files and never touches anything else:

```
~/.config/moroder/browser.json    your YouTube Music session   (cookies)
~/.config/moroder/moroder.conf    everything else
```

To run without an account:

```bash
moroder --anonymous     # or -a
```

### Commands

| Command | What it does |
|---|---|
| `moroder` | starts the player |
| `moroder setup` | signs in and fills in the config |
| `moroder keys` | shows what your terminal sends for each key (Shift / Option + arrows) |
| `moroder lastfm` | connects Last.fm scrobbling |
| `moroder --anonymous`, `-a` | starts without an account |
| `moroder --help`, `-h` | usage |

## Signing in

Moroder talks to YouTube Music the way your browser does, by reusing the
session your browser already holds. No password is involved and nothing is sent
anywhere except YouTube.

```bash
moroder setup
```

The command walks through five steps:

1. Checks that `mpv` and `yt-dlp` are installed
2. Opens `music.youtube.com` so you can confirm you are signed in
3. Looks for your browser profile, which is used for stream cookies
4. Takes the request headers you paste in and checks them against a live request
5. Writes `browser.json` and records `BROWSER` and the two paths in `moroder.conf`

Nothing is saved until step 4 passes. The new session is staged as
`browser.json.new` and only moved into place once YouTube Music has answered,
so a failed sign-in can never replace a working one. The previous session is
kept as `browser.json.bak`.

### Copying the headers

1. Open `music.youtube.com`, signed in
2. F12 → Network tab
3. Reload, filter requests for `/youtubei/`
4. Click any POST request
5. Copy everything under **Request Headers**, paste into the terminal, `Ctrl-D`

Both DevTools' "Copy All" (`name: value` lines) and "Copy as cURL"
(`-H 'name: value'`) are accepted.

> **Note**
> Earlier versions could also pull the cookie out of the browser store with
> yt-dlp. That path recovered the cookie alone - no `x-goog-visitor-id` and no
> real `user-agent` - so every start had to scrape a visitor id from the
> YouTube Music homepage, which made requests slower and easier to rate limit.
> Pasting is now the only method, and it captures all three.

### The browser profile

Separately from the session, setup looks for your browser's profile directory
and records it as `MPV_COOKIES_PATH`. That is what yt-dlp reads when opening a
stream, and it is what lets age-restricted and premium tracks play. It has
nothing to do with signing in, so if no profile is found, setup says so and
carries on.

```bash
moroder setup --browser librewolf
```

Supported: `firefox`, `librewolf`, `floorp`, `waterfox`, `zen`, `mercury`,
`icecat`, `tor`, `chrome`, `chromium`, `brave`, `edge`, `opera`, `vivaldi`,
`whale`.

yt-dlp only knows a fixed list of browser names, so the Firefox forks are
handed to it as `firefox:<profile path>` - the cookie store format is identical
and only the profile tells them apart. Setup finds that profile by reading
`profiles.ini` the way Firefox itself does, checking the `[Install…]` section
first (it names the profile actually in use, which often disagrees with the
`Default=1` flag left over from an older install). If there is no ini, it scans
for directories holding a `cookies.sqlite` and takes the busiest one.

If your profile lives somewhere unusual:

```bash
moroder setup --profile ~/.config/librewolf/librewolf/xxxxxxxx.default
```

### Setup options

| Flag | Meaning |
|---|---|
| `--browser <name>` | browser holding your session, saved as `BROWSER` |
| `--profile <dir>` | explicit browser profile directory, for stream cookies |
| `--no-colour` | plain output |
| `--help` | usage |

With no `--browser`, setup uses whatever `BROWSER` says in your config, so a
second run needs no flags.

### Running without an account

`moroder --anonymous` skips the session entirely. Search, radio and playback
still work; the library and the personalised home page have nothing to show.

Stream cookies are disabled in this mode too, because the browser's cookies
belong to an account the run is not using. Age-restricted and premium tracks
will not play anonymously.

## Using it

The interface is a sidebar, a search bar, a content area and a playback bar.

**Home** - your account's shelves, ordered by `HOME_ORDER`. Quick Picks renders
as a grid, everything else as a horizontal carousel. Signed out, this page says
so instead: search and playback still work, they are just slower.

**Library** - everything you have saved, with filter chips across the top.
Songs are excluded from the unfiltered view (there are too many) and appear
under their own chip.

**Search** - type and press <kbd>Enter</kbd>.

**Queue** - what is playing and what is next, with your queue and the radio
queue shown together.

### Looping

<kbd>r</kbd> cycles the loop icon in the playback bar through three states:

| State | Icon | What happens |
|---|---|---|
| Off | dimmed | the queue plays to the end, then radio and autoplay take over |
| Queue | white | the queue restarts at the top instead of moving on to the radio |
| Track | accent colour | the current track repeats until you skip or change the mode |

Looping the queue does not throw the radio queue away. It goes on filling in
the background and is still listed under the queue - it just stops being where
the next track comes from, so turning the loop off picks it back up.

There is no repeat-one glyph in the icon set, so a track loop is the same icon
in `COLOR_ACCENT_PRIMARY` rather than a different one.

With a queue loop on, skipping wraps in both directions: forward from the last
track lands on the first, back from the first lands on the last.

### The search bar doubles as a library filter

On the library page, typing without pressing <kbd>Enter</kbd> narrows the grid
in place, matching titles and artist or author names. Press <kbd>Enter</kbd> and
it becomes a real search, switching to the search page. Nothing needs to be
invalidated on a keystroke: the query is folded into the grid's identity, so the
keystroke's own redraw rebuilds it.

### When something fails

Failed requests show an error message with a **Retry** button, on home, search
and library alike. Retry is only offered for errors worth retrying - an expired
session is not one of them, so it says what to do instead.

The library is fed by five independent requests (playlists, albums, songs,
artists, podcasts). 

## Keybinds

A single character (`"q"`, `"/"`), a named key, or `"ctrl+<letter>"`. Named
keys: `space`, `enter`, `tab`, `escape`, `backspace`, `delete`, `up`, `down`,
`left`, `right`, `home`, `end`, `pageup`, `pagedown`. Set a binding to `""` to
unbind it.

| Key | Default | Action |
|---|---|---|
| `KEY_QUIT` | `q` | exit |
| `KEY_QUEUE_VIEW` | `a` | show the queue, while something is playing |
| `KEY_TOGGLE_SIDEBAR` | `s` | hide or show the sidebar |
| `KEY_FOCUS_SEARCH` | `f` | jump into the search bar |
| `KEY_SKIP_BACKWARD` | `z` | previous track (restarts the track past its first 3 s) |
| `KEY_SKIP_FORWARD` | `x` | next track |
| `KEY_TOGGLE_PAUSE` | `space` | play / pause |
| `KEY_TOGGLE_LOOP` | `r` | cycle looping: off, queue, current track |
| `KEY_SEEK_BACKWARD` | `h` | jump back `SEEK_SECONDS` (default 10); hold it to show the keys sheet |
| `KEY_SEEK_FORWARD` | `l` | jump ahead `SEEK_SECONDS` |
| `KEY_VOLUME_DOWN` | `-` | volume down by `VOLUME_STEP` (default 5) |
| `KEY_VOLUME_UP` | `=` | volume up by `VOLUME_STEP` |
| `KEY_LIKE` | `+` | like / un-like the current track (signed in) |
| `KEY_HELP` | `?` | show every keybind |
| `KEY_MUTE` | `m` | mute / unmute |
| `KEY_SLEEP_TIMER` | `t` | sleep timer: 15, 30, 60 min, end of the track, off |

Always on: <kbd>Shift</kbd> or <kbd>Option</kbd> + <kbd>←</kbd> / <kbd>→</kbd>
(or <kbd>&lt;</kbd> / <kbd>&gt;</kbd>, which every terminal sends the same way)
skip to the previous / next track and + <kbd>↑</kbd> / <kbd>↓</kbd> change the
volume (plain arrows keep moving around); in Now Playing the plain arrows do
it. `moroder keys` shows whether your terminal sends Shift / Option + arrows.
Holding <kbd>h</kbd> shows the keys sheet for as long as it is held.

<kbd>/</kbd> also opens search, <kbd>Esc</kbd> leaves it (or goes back to Home
from another page), and shortcuts like <kbd>q</kbd> work everywhere except
while typing in the search bar. The mouse works like in a desktop music app: click selects (and opens
Home, Library, a playlist or a filter), double-click plays, right-click opens
a menu (play, play from here, play next, add to a playlist, start radio,
add to / remove from queue, open). The playback bar's controls, heart and loop
icon are clickable, a click on the progress line seeks, a click on the volume
opens a slider (click or drag to set it, click its icon to mute), and the
wheel over it changes it. The wheel (or a two-finger trackpad scroll) scrolls
the page line by line like a web page: the selection stays where it is while
it is in view, and the page only moves as much as it has to when the
selection does (a click never scrolls the row away from under the pointer).
A sideways swipe, or Shift+wheel, moves along a carousel. Hovering doesn't
move the selection.

Playlists open in their own view: header with Play and Add to queue, then
every track; playing a track continues with the rest of the playlist.
Right-click a track in one of your playlists to remove it; right-click one of
your playlists in the sidebar to rename or delete it (asked twice).
Search suggests as you type (your past searches first, then YouTube Music's):
<kbd>↑</kbd> / <kbd>↓</kbd> pick, <kbd>Tab</kbd> fills it in, <kbd>Enter</kbd> or a click searches.
In the queue view, drag a row onto another to move it. In Now Playing, click a
lyrics line to jump to it.

Pages have a history, like a browser: <kbd>Esc</kbd>, <kbd>Backspace</kbd> or
<kbd>[</kbd> go back to the previous page (Home when there is none),
<kbd>]</kbd> goes forward, and so do the <kbd>‹</kbd> <kbd>›</kbd> buttons left
of the search bar. On a trackpad, a two-finger swipe sideways does the same
(over a shelf of covers it scrolls the shelf instead), and on Now Playing's
cover it skips to the previous / next track, in terminals that pass sideways
scrolling on to apps. Warp doesn't (it keeps it for itself), so use the
buttons or the keys there; `moroder keys` shows what your terminal sends.

Menus, the volume slider, prompts and the search suggestions are drawn on
your terminal's own background (and transparency), not a fixed grey.

Long playlists show their first 100 tracks as soon as they arrive and the rest
follows; playing one before it has finished loading queues the rest when it
lands. A first launch (no cached home) shows the first shelves of Home within
a second, before the others.
**New playlist** in the sidebar asks for a name and creates a private
playlist; right-click a track, *Add to playlist…*, to add it to one of yours
(or to a new one).

### Pages and views

- **Playlists and albums** open in their own page (Play, Add to queue, every
  track); playing a track continues with the rest. A playlist is fetched as
  soon as it is selected, so opening it is instant.
- **Artists** open a page with their Top songs, Albums, Singles, Playlists and
  related artists. Right-click a song for *Go to artist* or *Start radio*.
- **Now Playing** (<kbd>v</kbd>, or click the title in the playback bar): large
  cover, progress and the lyrics sung along when synced: the current line
  fills word by word, sung lines fade to grey, instrumental breaks show three
  filling dots. Lyrics come from [LRCLIB](https://lrclib.net): the track's title,
  artist and duration are sent there; `ENABLE_LYRICS = false` turns it off.
- **Recently played** is the first home shelf.

### Trying things out

- **Preview** (<kbd>w</kbd>, middle click, or right-click): 15 seconds of a track
  from its middle, without touching the queue. What was playing pauses and
  picks up where it was; <kbd>Esc</kbd> or <kbd>Space</kbd> ends it early.
- **Play next** (<kbd>N</kbd> or right-click): slot a track right after the
  current one.
- Right-click a track for **Copy link** and **Open in browser** too.

### Queue

- <kbd>S</kbd> (or the shuffle icon) shuffles what comes after the current track.
- In the queue view, <kbd>K</kbd> / <kbd>J</kbd> move a track up / down (also in
  the right-click menu, along with *Clear what comes next* and *Save queue as
  playlist…*).
- The next two tracks are resolved while the current one plays, and a track
  is resolved as soon as it is selected, so playing, skipping or moving on
  starts at once. A cold start (nothing resolved yet) takes ~3-5 s instead of
  the ~9 s of mpv's YouTube hook: moroder keeps a yt-dlp process running,
  hands mpv the direct stream URL and starts it as soon as YouTube accepts it.
- Covers are shown square: video thumbnails lose their black bars and are
  cropped to the centre. Each cover is drawn once per size and reused.
- Selecting a playlist resolves its first track too, so Play starts at once.
- Resolved stream links are kept (they last 6 h, saved in
  `~/.cache/moroder/streams.json` at exit): the resumed track and recent ones
  start immediately on the next launch. A link that no longer works is
  replaced by a fresh one.
- Tracks follow each other without a gap (the next one is opened before the
  current one ends) and are levelled to the same loudness.

### Last.fm

Scrobbling is optional. Create an API account at
[last.fm/api](https://www.last.fm/api/account/create), put its key and secret in
the config (`LASTFM_API_KEY`, `LASTFM_API_SECRET`), then run `moroder lastfm`
once and allow it in the page that opens. The session key is kept in
`~/.config/moroder/lastfm.json` (readable by you only). A track is shown as
now playing when it starts, and scrobbled once half of it (or four minutes)
has played.

### Picking up where you left off

- The current track, its position and the queue are saved; the next launch
  brings them back paused at the same spot.
- Searches are remembered: <kbd>Up</kbd> / <kbd>Down</kbd> in the search bar.
- The last home page, your playlists and the covers are cached in
  `~/.cache/moroder`, so the app shows them at once while fresh copies load. The playback bar shows elapsed / total time and the volume.

The volume is remembered between launches. Each new track raises a desktop notification (`notify-send` on Linux); on macOS it comes from a small helper app installed with Moroder (`share/moroder/Moroder.app`), so it carries Moroder's icon and the track's cover. Set `ENABLE_NOTIFICATIONS = false` to turn it off.

Liking needs fresh session cookies: Google rotates them, and YouTube Music
refuses writes with stale ones even while browsing still works. If a like is
refused, the bar says so; copy a fresh cURL and run `pbpaste | moroder setup`.

These act on whatever the cursor is over:

| Key | Default | Action |
|---|---|---|
| `KEY_PLAY_NOW` | `enter` | play now, replacing the queue and starting a radio |
| `KEY_ADD_TO_QUEUE` | `d` | append to the queue, leaving playback alone |
| `KEY_REMOVE_FROM_QUEUE` | `c` | remove from the queue, queue view only |

For keybind remapping, a [list](https://arthursonzogni.github.io/FTXUI/event_8cpp_source.html) of possible keybinds is available in the ftxui docs.

---

[README](../README.md) · [Install](install.md) · **Usage** · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)
