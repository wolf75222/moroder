# Contributing

Thanks for wanting to help. Bug reports, ideas and pull requests are all welcome.

## Reporting a bug

[Open an issue](https://github.com/wolf75222/moroder/issues/new) with:

- your system (macOS, Linux distribution, Windows) and terminal;
- `moroder --version`;
- what you did, what you expected, what happened;
- the output of `moroder doctor` if it's about playback or sign-in.

`moroder doctor` also shows where the log is (`~/.local/state/moroder/logs/moroder.log`). Before you paste it, remove anything personal. **Never paste cookies or the contents of `browser.json`.**

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/moroder
```

You need a C++17 compiler, CMake, libcurl and libmpv. [docs/install.md](../docs/install.md) lists the packages for each system.

To try it without touching your own setup, run it with a throwaway home and no sign-in:

```bash
HOME=$(mktemp -d) ./build/moroder -a
```

## Pull requests

- One change per pull request, with a short description of what it fixes or adds.
- Follow the code around you: C++17, 4 spaces, comments that say why.
- Keep the interface minimal: show state with what is already on screen (a selected tab, a spinner) rather than new lines of text.
- Make sure it builds on the three systems. CI checks macOS, Linux and Windows on every pull request.

By contributing, you agree that your work is released under the [GNU AGPL v3](../LICENSE).
