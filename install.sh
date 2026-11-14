#!/bin/sh
# Moroder installer / updater.
#
#   curl -fsSL https://raw.githubusercontent.com/wolf75222/moroder/main/install.sh | sh
#
# - macOS (Apple silicon): installs the dependencies with Homebrew and the
#   latest release's build (checksum verified); other Macs build from source
# - Linux: builds the latest release from source into ~/.local
# - Puts `moroder` on your PATH. Run it again to update.
#
# MORODER_VERSION=1.2.0 sh install.sh   installs a given version
# MORODER_FROM_SOURCE=1 sh install.sh   always builds from source
# MORODER_PREFIX=~/opt sh install.sh     installs under another prefix

set -eu

REPO="wolf75222/moroder"
API="https://api.github.com/repos/$REPO"

say()  { printf '\033[1;31m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have curl || fail "curl is needed"
have tar  || fail "tar is needed"

OS=$(uname -s)
ARCH=$(uname -m)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Which version: the one asked for, or the latest release.
VERSION="${MORODER_VERSION:-}"
if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL "$API/releases/latest" | sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n1)
    [ -n "$VERSION" ] || fail "couldn't find the latest release of $REPO"
fi
VERSION=${VERSION#v}
say "Moroder $VERSION"

# Dependencies, and where it goes.
if [ "$OS" = "Darwin" ]; then
    have brew || fail "Homebrew is needed: https://brew.sh"
    PREFIX="${MORODER_PREFIX:-$(brew --prefix)}"
    for f in mpv yt-dlp jpeg-turbo libpng; do
        brew list --versions "$f" >/dev/null 2>&1 || { say "brew install $f"; brew install "$f"; }
    done
elif [ "$OS" = "Linux" ]; then
    PREFIX="${MORODER_PREFIX:-$HOME/.local}"
    missing=""
    for c in cmake git c++ pkg-config yt-dlp; do have "$c" || missing="$missing $c"; done
    pkg-config --exists mpv 2>/dev/null || missing="$missing libmpv-dev"
    pkg-config --exists libcurl 2>/dev/null || missing="$missing libcurl-dev"
    [ -z "$missing" ] || fail "install first:$missing  (Debian/Ubuntu: sudo apt install build-essential cmake git pkg-config libmpv-dev libcurl4-openssl-dev yt-dlp)"
else
    fail "unsupported system: $OS"
fi
mkdir -p "$PREFIX/bin" "$PREFIX/share/moroder"

ASSET="moroder-$VERSION-macos-arm64"
URL="https://github.com/$REPO/releases/download/v$VERSION/$ASSET.tar.gz"

if [ "$OS" = "Darwin" ] && [ "$ARCH" = "arm64" ] && [ -z "${MORODER_FROM_SOURCE:-}" ] \
   && curl -fsSL -o "$TMP/$ASSET.tar.gz" "$URL" 2>/dev/null; then
    say "Downloaded the macOS build"
    curl -fsSL -o "$TMP/$ASSET.tar.gz.sha256" "$URL.sha256"
    ( cd "$TMP" && shasum -a 256 -c "$ASSET.tar.gz.sha256" >/dev/null ) || fail "checksum mismatch, not installing"
    tar -xzf "$TMP/$ASSET.tar.gz" -C "$TMP"
    install -m 755 "$TMP/$ASSET/bin/moroder" "$PREFIX/bin/moroder"
    rm -rf "$PREFIX/share/moroder/Moroder.app"
    cp -R "$TMP/$ASSET/share/moroder/Moroder.app" "$PREFIX/share/moroder/"
    xattr -dr com.apple.quarantine "$PREFIX/bin/moroder" "$PREFIX/share/moroder/Moroder.app" 2>/dev/null || true
else
    say "Building from source (a few minutes the first time)"
    if [ "$OS" = "Darwin" ]; then
        for f in cmake pkg-config; do brew list --versions "$f" >/dev/null 2>&1 || brew install "$f"; done
    fi
    git clone --depth 1 --branch "v$VERSION" "https://github.com/$REPO.git" "$TMP/src" >/dev/null 2>&1 \
        || fail "couldn't fetch v$VERSION"
    cmake -S "$TMP/src" -B "$TMP/build" -DCMAKE_BUILD_TYPE=Release -DMORODER_VERSION="$VERSION" >/dev/null
    cmake --build "$TMP/build" -j >/dev/null
    cmake --install "$TMP/build" --prefix "$PREFIX" >/dev/null
fi

# On the PATH, for this shell's next sessions (MORODER_NO_PATH=1: leave the shell files alone).
[ -n "${MORODER_NO_PATH:-}" ] || case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *)
        rc="$HOME/.profile"
        case "${SHELL:-}" in */zsh) rc="$HOME/.zshrc" ;; */bash) rc="$HOME/.bashrc" ;; esac
        if ! grep -qs "$PREFIX/bin" "$rc"; then
            printf '\n# Moroder\nexport PATH="%s/bin:$PATH"\n' "$PREFIX" >> "$rc"
            say "Added $PREFIX/bin to your PATH in $rc (open a new terminal)"
        fi
        ;;
esac

say "Installed: $("$PREFIX/bin/moroder" --version 2>/dev/null || echo "moroder $VERSION")"
[ -f "$HOME/.config/moroder/browser.json" ] || say "Next: moroder setup   (sign in once), then: moroder"
