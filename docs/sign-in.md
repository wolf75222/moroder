[README](../README.md) · [Install](install.md) · **Sign in** · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)

# Sign in

```bash
moroder setup
```

Moroder uses the YouTube Music session your browser already has. No password
is typed into it and nothing is sent anywhere except YouTube.

## What setup does

1. Finds which of your browsers are signed in to YouTube (Chrome, Arc,
   Brave, Edge, Vivaldi, Chromium, Firefox, Zen, LibreWolf, and Opera /
   Floorp on Linux), every profile included, without asking for anything
2. Lists them, the one you used last first: pick with the arrow keys and Enter
3. Reads that one only, asks YouTube Music whose account it is, and shows its
   name, @handle and playlists. A browser with several Google accounts lists
   them all, to pick from the same way
4. Checks it and saves it. `moroder doctor` shows which account is in use

None signed in? Two ways:

- **New window**: Chrome, Edge or Brave opens on a profile of its own; sign in
  to YouTube Music there and setup takes the session from the browser itself.
  Works for any account, and where cookies can't be read from disk (Chrome
  and Edge on Windows)
- **Sign in first**: music.youtube.com opens in your usual browser, and setup
  looks again once you're signed in

On macOS, Chrome, Arc, Brave, Edge, Vivaldi and Chromium keep their cookies
encrypted: macOS asks to let moroder use the browser's "Safe Storage", choose
**Always Allow**. With those, the session also stays fresh on its own: as long
as you're signed in in the browser, Moroder is too.

Only one browser? `moroder setup --browser firefox` (or `arc`, `brave`...).

## None of them signed in?

Sign in at [music.youtube.com](https://music.youtube.com), then run
`moroder setup` again. Or, when setup can't read a browser (Safari, for one),
it walks you through copying one request from it:

1. Open [music.youtube.com](https://music.youtube.com), signed in
2. Open DevTools (<kbd>⌘</kbd> <kbd>⌥</kbd> <kbd>I</kbd> on macOS, <kbd>F12</kbd> on Linux), then the **Network** tab
3. Reload the page and filter the requests for `/youtubei/`
4. Click any `POST` request
5. Copy the **Request Headers**
   - Firefox: right-click a header, **Copy All**
   - Chrome and co.: right-click the request, **Copy as cURL**
6. Paste into setup, then <kbd>Ctrl</kbd> <kbd>D</kbd> on a blank line

On macOS, with the request copied as cURL: `pbpaste | moroder setup`.

Safari: enable **Settings › Advanced › Show features for web developers**
first, to get DevTools.

## Without an account

```bash
moroder -a
```

Search and play anything. There is no library, no personalised home, and some
age-restricted or Premium-only tracks will not play.

## Afterwards

- **Library stopped loading:** the session expired. Run `moroder setup` again.
- **Sign out:** delete `~/.config/moroder/browser.json`.
- **Something else:** `moroder doctor`, then [Troubleshooting](troubleshooting.md).

---

[README](../README.md) · [Install](install.md) · **Sign in** · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)
