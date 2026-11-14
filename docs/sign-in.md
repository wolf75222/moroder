[README](../README.md) · [Install](install.md) · **Sign in** · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · [Troubleshooting](troubleshooting.md)

# Sign in

Moroder uses the YouTube Music session your browser already has. No password
is typed into it and nothing is sent anywhere except YouTube.

First, sign in at [music.youtube.com](https://music.youtube.com) in your
browser. Then pick the line for your browser.

| Browser | What to run |
|:--|:--|
| Chrome, Arc, Brave, Edge, Vivaldi, Chromium (macOS) | `moroder setup --browser chrome` (or `arc`, `brave`...) |
| Firefox, Zen, LibreWolf, Safari, or any browser on Linux | `moroder setup --browser firefox` (or your browser), then paste the headers |
| No account | `moroder -a` |

## Chromium browsers on macOS: automatic

`moroder setup --browser arc` reads the session straight from the browser.
macOS asks to let moroder use "Arc Safe Storage": choose **Always Allow**.

The session is then refreshed on its own: as long as you stay signed in in the
browser, moroder stays signed in.

## Other browsers: paste the headers

Setup asks you to copy one request from the browser:

1. Open [music.youtube.com](https://music.youtube.com), signed in
2. Open DevTools (<kbd>⌘</kbd> <kbd>⌥</kbd> <kbd>I</kbd> on macOS, <kbd>F12</kbd> on Linux), then the **Network** tab
3. Reload the page and filter the requests for `/youtubei/`
4. Click any `POST` request
5. Copy the **Request Headers**
   - Firefox: right-click a header, **Copy All**
   - Chrome and co.: right-click the request, **Copy as cURL**
6. Paste into setup, then <kbd>Ctrl</kbd> <kbd>D</kbd> on a blank line

On macOS, with the request copied as cURL, this does it in one go:

```bash
pbpaste | moroder setup
```

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
