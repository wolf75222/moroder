[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · **Troubleshooting**

# Troubleshooting

## Troubleshooting

<details>
<summary><b>The library is empty, or shows an auth error</b></summary>

Your session expired. Cookies do not last forever.

```bash
moroder setup
```
</details>

<details>
<summary><b>Setup says "the cookie has no SAPISID"</b></summary>

The headers came from a signed-out tab, or from somewhere other than YouTube
Music. Reload `music.youtube.com`, confirm you are signed in, and copy the
headers from a request under `/youtubei/` rather than from anywhere else on the
page.
</details>

<details>
<summary><b>LibreWolf: the session keeps dying</b></summary>

LibreWolf clears cookies on shutdown by default, which wipes the session every
time you close it. Under **Settings → Privacy & Security**, either turn that off
or add an exception for `youtube.com`, then sign in again and re-run setup.
</details>

<details>
<summary><b>Setup cannot find my browser profile</b></summary>

This only affects stream cookies, never signing in. Point it at the directory
holding `cookies.sqlite`:

```bash
moroder setup --profile ~/.mozilla/firefox/xxxxxxxx.default-release
```

Or set `MPV_COOKIES_PATH` in the config by hand.
</details>

<details>
<summary><b>Some / All tracks refuse to play</b></summary>

If only some tracks refuse to play, this has to do with age-restricted and premium tracks that need premium youtube music to play.
Check if `MPV_COOKIES_PATH` points at your browser profile directory and `BROWSER` names the right browser. Sometimes cookies for a
non premium account can introduce playback issues on all tracks. Try `--anonymous` when launching the app to see if playback works.
You can also check logs for `MPV: stream cookies from …`;

In `--anonymous` mode stream cookies are always disabled, and the log says
`MPV: anonymous mode, stream cookies disabled`.
</details>

<details>
<summary><b>Covers do not render</b></summary>

Character-art covers need a terminal with truecolour support. Check
`$COLORTERM` is `truecolor` or `24bit`.
</details>

<details>
<summary><b>playerctl does not see it</b></summary>

Check the service registered:

```bash
busctl --user list | grep moroder
```

If it is missing, the log will say why - `MPRIS: could not claim …` names the
D-Bus error. A second instance cannot claim the name while the first holds it.
</details>

## Limitations

- Linux only
- Playlist editing, likes and subscriptions are not visible
- The **New playlist** and **Sign in** sidebar buttons are not wired up yet
- Anonymous mode has no library, no personalised home, and no stream cookies
- One instance at a time owns the MPRIS name

---

[README](../README.md) · [Install](install.md) · [Sign in](sign-in.md) · [Usage](usage.md) · [Configuration](configuration.md) · [Integrations](integrations.md) · [Internals](internals.md) · **Troubleshooting**
