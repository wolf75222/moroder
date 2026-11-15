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
# MORODER_NO_PATH=1 sh install.sh        leaves the shell files alone
# MORODER_SOURCE=~/moroder sh install.sh builds that checkout instead of the release

set -eu

REPO="wolf75222/moroder"
API="https://api.github.com/repos/$REPO"

# ---------------------------------------------------------------------------
# Output: a live list of tasks, the independent ones running side by side.
# Without a terminal (or with NO_COLOR) it prints one plain line per task.
# ---------------------------------------------------------------------------

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != "dumb" ]; then LIVE=1; else LIVE=0; fi
if [ "$LIVE" = 1 ]; then
    ESC=$(printf '\033')
    RED="${ESC}[31m" GRN="${ESC}[32m" YEL="${ESC}[33m" DIM="${ESC}[2m" BLD="${ESC}[1m" RST="${ESC}[0m" CL="$(printf '\r')${ESC}[2K"
else
    ESC="" RED="" GRN="" YEL="" DIM="" BLD="" RST="" CL=""
fi

COLS=$(tput cols 2>/dev/null || echo 80)
TMP=$(mktemp -d)
ST="$TMP/state"; LOGS="$TMP/logs"
mkdir -p "$ST" "$LOGS"
PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null || true; done
    [ "$LIVE" = 1 ] && printf '%s[?25h' "${ESC}"
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

have() { command -v "$1" >/dev/null 2>&1; }
die()  { printf '\n  %s✗%s %s\n\n' "${RED}" "${RST}" "$*" >&2; exit 1; }

TASKS=""
task() { TASKS="$TASKS $1"; eval "LABEL_$1=\"\$2\""; }       # name, label
set_() { printf '%s\n%s\n' "$2" "${3:-}" > "$ST/$1.new"; mv -f "$ST/$1.new" "$ST/$1"; }

FRAME=0
spin() {
    set -- ⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
    shift $((FRAME % 10))
    printf '%s' "$1"
}

line() {
    eval "label=\$LABEL_$1"
    state=wait detail=""
    if [ -f "$ST/$1" ]; then { read -r state; read -r detail || true; } < "$ST/$1"; fi
    case $state in
        run)  icon="${YEL}$(spin)${RST}" ;;
        ok)   icon="${GRN}✓${RST}" ;;
        fail) icon="${RED}✗${RST}" ;;
        skip) icon="${DIM}–${RST}" ;;
        *)    icon="${DIM}·${RST}"; detail="" ;;
    esac
    case $detail in *"${ESC}"*) ;; *) detail=$(printf '%s' "$detail" | cut -c1-$((COLS - 22))) ;; esac
    printf '%s  %s  %-14s %s%s%s\n' "${CL}" "$icon" "$label" "${DIM}" "$detail" "${RST}"
    DRAWN=$((DRAWN + 1))
    # While it runs, what it is doing right now: the last line of its log.
    if [ "$state" = run ] && [ "$LIVE" = 1 ] && [ -s "$LOGS/$1" ]; then
        last=$(tail -n 1 "$LOGS/$1" | tr -d '\r' | sed "s/${ESC}\[[0-9;]*[A-Za-z]//g" | cut -c1-$((COLS - 10)))
        [ -n "$last" ] && { printf '%s     %s└ %s%s\n' "${CL}" "${DIM}" "$last" "${RST}"; DRAWN=$((DRAWN + 1)); }
    fi
}

DRAWN=0
draw() {
    [ "$DRAWN" -gt 0 ] && printf '%s[%dA%s[J' "${ESC}" "$DRAWN" "${ESC}"
    DRAWN=0
    for t in $TASKS; do line "$t"; done
}

# run TASK...: runs t_<task> for each, at the same time, and waits for all.
run() {
    for t in "$@"; do
        set_ "$t" run ""
        (
            if "t_$t" >> "$LOGS/$t" 2>&1; then
                { read -r s; read -r d || true; } < "$ST/$t"
                [ "$s" = run ] && set_ "$t" ok "$d"
            else
                { read -r s; read -r d || true; } < "$ST/$t"
                [ "$s" = fail ] || set_ "$t" fail "${d:-failed}"
            fi
            exit 0
        ) &
        eval "PID_$t=$!"
        PIDS="$PIDS $!"
    done
    while :; do
        alive=0
        for t in "$@"; do eval "kill -0 \$PID_$t 2>/dev/null" && alive=1; done
        [ "$LIVE" = 1 ] && draw
        [ "$alive" = 1 ] || break
        sleep 0.1
        FRAME=$((FRAME + 1))
    done
    for t in "$@"; do eval "wait \$PID_$t" 2>/dev/null || true; done
    [ "$LIVE" = 1 ] || for t in "$@"; do line "$t"; done
    for t in "$@"; do
        { read -r s; read -r d || true; } < "$ST/$t"
        if [ "$s" = fail ]; then
            printf '\n  %s%s%s\n' "${DIM}" "last lines of the $t log:" "${RST}"
            tail -n 15 "$LOGS/$t" | sed 's/^/    /'
            die "$(eval "printf '%s' \"\$LABEL_$t\"") failed"
        fi
    done
}

# bar DONE TOTAL [UNIT]: ██████░░░░  42%
bar() {
    awk -v a="$1" -v b="$2" -v u="${3:-}" -v c="${RED}" -v r="${RST}" -v d="${DIM}" 'BEGIN {
        p = b > 0 ? a / b : 0; if (p > 1) p = 1; w = 24; n = int(p * w + 0.5)
        s = ""; for (i = 0; i < n; i++) s = s "█"
        t = ""; for (i = n; i < w; i++) t = t "░"
        printf "%s%s%s%s%s %3d%%", r, c, s, r d, t, p * 100
        if (u == "MB") printf "  %.1f / %.1f MB", a / 1048576, b / 1048576
    }'
}

# ---------------------------------------------------------------------------
# What to install
# ---------------------------------------------------------------------------

have curl || die "curl is needed"
have tar  || die "tar is needed"

OS=$(uname -s)
ARCH=$(uname -m)
START=$(date +%s)

VERSION="${MORODER_VERSION:-}"
if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL "$API/releases/latest" | sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n1)
    [ -n "$VERSION" ] || die "couldn't find the latest release of $REPO (offline?)"
fi
VERSION=${VERSION#v}

if [ "$OS" = "Darwin" ]; then
    have brew || die "Homebrew is needed: https://brew.sh"
    PREFIX="${MORODER_PREFIX:-$(brew --prefix)}"
    DEPS="mpv yt-dlp jpeg-turbo libpng"
elif [ "$OS" = "Linux" ]; then
    PREFIX="${MORODER_PREFIX:-$HOME/.local}"
    YTVENV="$PREFIX/share/moroder/yt-dlp"

    # What the build needs, in each distribution's names. pkgconfig(...) lets
    # dnf and zypper pick whatever package provides the library.
    if have pacman; then
        PM="pacman"; PKGS="base-devel cmake git pkgconf mpv curl libjpeg-turbo libpng systemd yt-dlp"
        INSTALL="pacman -S --needed --noconfirm"
    elif have dnf; then
        PM="dnf"; PKGS="gcc-c++ make cmake git pkgconf-pkg-config 'pkgconfig(mpv)' 'pkgconfig(libcurl)' 'pkgconfig(libjpeg)' 'pkgconfig(libpng)' 'pkgconfig(libsystemd)' yt-dlp"
        INSTALL="dnf install -y"
    elif have zypper; then
        PM="zypper"; PKGS="gcc-c++ make cmake git pkg-config 'pkgconfig(mpv)' 'pkgconfig(libcurl)' 'pkgconfig(libjpeg)' 'pkgconfig(libpng)' 'pkgconfig(libsystemd)' yt-dlp"
        INSTALL="zypper --non-interactive install"
    elif have apt-get; then
        # Debian/Ubuntu's yt-dlp lags behind YouTube: a private, current one is set up instead.
        PM="apt"; PKGS="build-essential cmake git pkg-config libmpv-dev libcurl4-openssl-dev libjpeg-dev libpng-dev libsystemd-dev python3-venv"
        INSTALL="apt-get install -y"
    else
        PM=""
    fi

    missing=""
    for c in cmake git c++ pkg-config; do have "$c" || missing="$missing $c"; done
    for m in mpv libcurl; do pkg-config --exists "$m" 2>/dev/null || missing="$missing $m"; done
    if [ "$PM" = apt ]; then
        have yt-dlp || python3 -c 'import venv, ensurepip' 2>/dev/null || missing="$missing python3-venv"
    else
        have yt-dlp || missing="$missing yt-dlp"
    fi

    if [ -n "$missing" ]; then
        [ -n "$PM" ] || die "missing:$missing (install them with your package manager, then run this again)"
        SUDO=""
        if [ "$(id -u)" -ne 0 ]; then
            if have sudo; then SUDO="sudo"; elif have doas; then SUDO="doas"; else die "missing:$missing (run as root, or install sudo)"; fi
        fi
        [ "$PM" = apt ] && INSTALL="apt-get update -qq && $SUDO env DEBIAN_FRONTEND=noninteractive $INSTALL"
        printf '\n  %s!%s Missing to build Moroder:%s\n' "${YEL}" "${RST}" "$missing"
        printf '    %s%s %s %s%s\n' "${DIM}" "$SUDO" "$INSTALL" "$PKGS" "${RST}"
        answer=y
        if [ -r /dev/tty ]; then
            printf '\n  %s?%s Install them now%s? %s[Y/n]%s ' "${BLD}" "${RST}" "${SUDO:+ (asks for your password)}" "${DIM}" "${RST}"
            read -r answer < /dev/tty || answer=n
        elif [ -n "$SUDO" ]; then
            die "run the command above, then this again"
        fi
        case $answer in [nN]*) die "run the command above, then this again" ;; esac
        printf '\n'
        # shellcheck disable=SC2086
        sh -c "$SUDO $INSTALL $PKGS" || die "the package install failed"
    fi
else
    die "unsupported system: $OS"
fi

ASSET="moroder-$VERSION-macos-arm64"
URL="https://github.com/$REPO/releases/download/v$VERSION/$ASSET.tar.gz"
PREBUILT=0
if [ "$OS" = "Darwin" ] && [ "$ARCH" = "arm64" ] && [ -z "${MORODER_FROM_SOURCE:-}" ] \
   && curl -fsIL -o /dev/null "$URL" 2>/dev/null; then
    PREBUILT=1
fi

# ---------------------------------------------------------------------------
# Tasks
# ---------------------------------------------------------------------------

t_deps() {
    if [ "$OS" = "Linux" ]; then
        if [ -x "$YTVENV/bin/yt-dlp" ] || { [ "$PM" = apt ] && ! have yt-dlp; }; then
            # Its own Python environment: current, and importable by the player.
            if [ ! -x "$YTVENV/bin/yt-dlp" ]; then
                set_ deps run "setting up yt-dlp"
                python3 -m venv "$YTVENV" || return 1
            else
                set_ deps run "updating yt-dlp"
            fi
            "$YTVENV/bin/pip" install -q -U "yt-dlp[default]" || return 1
            mkdir -p "$PREFIX/bin" && ln -sf "$YTVENV/bin/yt-dlp" "$PREFIX/bin/yt-dlp" || return 1
            set_ deps run "${PM:-system} packages, yt-dlp $("$YTVENV/bin/yt-dlp" --version)"
        else
            set_ deps run "${PM:-system} packages, yt-dlp $(yt-dlp --version 2>/dev/null) from ${PM:-the system}"
        fi
        return 0
    fi
    export HOMEBREW_NO_ENV_HINTS=1 HOMEBREW_NO_INSTALL_CLEANUP=1
    need=""
    for f in $DEPS; do brew list --versions "$f" >/dev/null 2>&1 || need="$need $f"; done
    total=$(echo $need | wc -w | tr -d ' ')
    i=0
    for f in $need; do
        i=$((i + 1))
        set_ deps run "installing $f ($i/$total)"
        brew install "$f" || return 1
    done
    if [ -z "$need" ] && [ -n "${MORODER_UPDATING:-}" ]; then
        set_ deps run "updating yt-dlp"
        brew upgrade yt-dlp || true
    fi
    set_ deps run "${need:+installed$need, }yt-dlp $(yt-dlp --version 2>/dev/null)"
}

# yt-dlp needs a JavaScript runtime to solve YouTube's challenges: without
# one, streams don't resolve. Deno into PREFIX/bin, unless one is there.
t_js() {
    for js in deno node bun; do
        if have "$js"; then set_ js run "$js (already installed)"; return 0; fi
    done
    case "$ARCH" in x86_64|amd64) target=x86_64-unknown-linux-gnu ;; aarch64|arm64) target=aarch64-unknown-linux-gnu ;; *) set_ js skip "no Deno build for $ARCH"; return 0 ;; esac
    have unzip || { set_ js fail "unzip is needed to install Deno"; return 1; }
    set_ js run "downloading Deno"
    curl -fsSL -o "$TMP/deno.zip" "https://github.com/denoland/deno/releases/latest/download/deno-$target.zip" || return 1
    unzip -oq "$TMP/deno.zip" -d "$TMP/deno" || return 1
    mkdir -p "$PREFIX/bin" && install -m 755 "$TMP/deno/deno" "$PREFIX/bin/deno" || return 1
    set_ js run "deno $("$PREFIX/bin/deno" --version 2>/dev/null | head -n1 | cut -d' ' -f2)"
}

t_tools() {      # building on a Mac also needs these
    for f in cmake pkg-config; do
        brew list --versions "$f" >/dev/null 2>&1 && continue
        set_ tools run "installing $f"
        brew install "$f" || return 1
    done
    set_ tools run "cmake, pkg-config"
}

t_download() {
    out="$TMP/$ASSET.tar.gz"
    total=$(curl -sIL "$URL" | tr -d '\r' | awk 'tolower($1) == "content-length:" { n = $2 } END { print n + 0 }')
    curl -fsSL -o "$out" "$URL" &
    cp=$!
    while kill -0 "$cp" 2>/dev/null; do
        got=$(wc -c < "$out" 2>/dev/null || echo 0)
        set_ download run "$(bar "$got" "$total" MB)"
        sleep 0.1
    done
    wait "$cp" || return 1
    set_ download run "verifying the checksum"
    curl -fsSL -o "$out.sha256" "$URL.sha256" || return 1
    ( cd "$TMP" && shasum -a 256 -c "$ASSET.tar.gz.sha256" ) || { set_ download fail "checksum mismatch, nothing installed"; return 1; }
    tar -xzf "$out" -C "$TMP" || return 1
    set_ download run "$(awk -v b="$total" 'BEGIN { printf "%.1f MB", b / 1048576 }'), checksum ok"
}

t_fetch() {
    if [ -n "${MORODER_SOURCE:-}" ]; then          # a local checkout (development)
        set_ fetch run "copying $MORODER_SOURCE"
        mkdir -p "$TMP/src" && ( cd "$MORODER_SOURCE" && tar --exclude=./build --exclude=./.git -cf - . ) | ( cd "$TMP/src" && tar -xf - ) || return 1
        set_ fetch run "local checkout"
        return 0
    fi
    set_ fetch run "git clone v$VERSION"
    git clone --depth 1 --branch "v$VERSION" "https://github.com/$REPO.git" "$TMP/src" || { set_ fetch fail "couldn't fetch v$VERSION"; return 1; }
    set_ fetch run "v$VERSION"
}

t_build() {
    set_ build run "configuring"
    extra=""
    # Media keys and desktop widgets go through D-Bus (libsystemd); without it, still builds.
    [ "$OS" = Linux ] && ! pkg-config --exists libsystemd 2>/dev/null && extra="-DMORODER_ENABLE_MPRIS=OFF"
    cmake -S "$TMP/src" -B "$TMP/build" -DCMAKE_BUILD_TYPE=Release -DMORODER_VERSION="$VERSION" $extra || return 1
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    [ "$jobs" -gt 8 ] && jobs=8
    cmake --build "$TMP/build" -j "$jobs" >> "$LOGS/build" 2>&1 &
    bp=$!
    while kill -0 "$bp" 2>/dev/null; do
        pct=$(grep -o '\[ *[0-9]*%\]' "$LOGS/build" 2>/dev/null | tail -n 1 | tr -dc '0-9')
        set_ build run "$(bar "${pct:-0}" 100)  $jobs jobs"
        sleep 0.2
    done
    wait "$bp" || return 1
    set_ build run "$jobs jobs"
}

t_install() {
    mkdir -p "$PREFIX/bin" "$PREFIX/share/moroder" || return 1
    if [ "$PREBUILT" = 1 ]; then
        install -m 755 "$TMP/$ASSET/bin/moroder" "$PREFIX/bin/moroder" || return 1
        rm -rf "$PREFIX/share/moroder/Moroder.app"
        cp -R "$TMP/$ASSET/share/moroder/Moroder.app" "$PREFIX/share/moroder/" || return 1
        xattr -dr com.apple.quarantine "$PREFIX/bin/moroder" "$PREFIX/share/moroder/Moroder.app" 2>/dev/null || true
    else
        cmake --install "$TMP/build" --prefix "$PREFIX" || return 1
    fi
    set_ install run "$PREFIX/bin/moroder"
}

t_path() {
    if [ -n "${MORODER_NO_PATH:-}" ]; then set_ path skip "left alone (MORODER_NO_PATH)"; return 0; fi
    case ":$PATH:" in *":$PREFIX/bin:"*) set_ path run "already on it"; return 0 ;; esac
    case "${SHELL:-}" in
        */fish)
            rc="$HOME/.config/fish/conf.d/moroder.fish"
            mkdir -p "$(dirname "$rc")" && printf 'fish_add_path "%s/bin"\n' "$PREFIX" > "$rc" || return 1
            set_ path run "added in $rc (open a new terminal)"
            return 0 ;;
        */zsh)  rc="$HOME/.zshrc" ;;
        */bash) rc="$HOME/.bashrc" ;;
        *)      rc="$HOME/.profile" ;;
    esac
    if grep -qs "$PREFIX/bin" "$rc"; then set_ path run "in $rc (open a new terminal)"; return 0; fi
    printf '\n# Moroder\nexport PATH="%s/bin:$PATH"\n' "$PREFIX" >> "$rc" || return 1
    set_ path run "added to $rc (open a new terminal)"
}

# ---------------------------------------------------------------------------
# Go
# ---------------------------------------------------------------------------

if [ "$PREBUILT" = 1 ]; then how="prebuilt"; else how="from source"; fi
printf '\n  %s%s▶ Moroder %s%s  %s%s, %s%s\n\n' "${BLD}" "${RED}" "$VERSION" "${RST}" "${DIM}" "$([ "$OS" = Darwin ] && echo macOS || echo $OS) $ARCH" "$how" "${RST}"
printf '  %sInstalling into%s\n' "${DIM}" "${RST}"
printf '    %-9s %s\n' "program" "$PREFIX/bin/moroder"
[ "$OS" = "Darwin" ] && printf '    %-9s %s\n' "helper" "$PREFIX/share/moroder/Moroder.app  (cover art in notifications)"
printf '    %-9s %s\n' "settings" "~/.config/moroder  (created by moroder setup)"
[ "$OS" = "Darwin" ] && printf '    %-9s %s\n' "with" "Homebrew: $DEPS"
printf '\n'
[ "$LIVE" = 1 ] && printf '%s[?25l' "${ESC}"

task deps "Dependencies"
if [ "$PREBUILT" = 1 ]; then
    task download "Download"
    task install  "Install"
    task path     "PATH"
    run deps download             # side by side
else
    [ "$OS" = "Darwin" ] && task tools "Build tools"
    [ "$OS" = "Linux" ] && task js "JS runtime"
    task fetch   "Source"
    task build   "Compile"
    task install "Install"
    task path    "PATH"
    if [ "$OS" = "Darwin" ]; then run deps tools fetch; else run deps js fetch; fi
    run build
fi
run install
run path

[ "$LIVE" = 1 ] && printf '%s[?25h' "${ESC}"
took=$(( $(date +%s) - START ))
printf '\n  %s✓%s %sMoroder %s%s is installed %s(%ss)%s\n' "${GRN}" "${RST}" "${BLD}" "$VERSION" "${RST}" "${DIM}" "$took" "${RST}"
printf '    %s%s  %s%s\n' "${DIM}" "$PREFIX/bin/moroder" "$(du -h "$PREFIX/bin/moroder" 2>/dev/null | cut -f1)" "${RST}"
if [ -f "$HOME/.config/moroder/browser.json" ]; then
    printf '    Run %smoroder%s\n\n' "${BLD}" "${RST}"
else
    printf '    Next: %smoroder setup%s to sign in once, then %smoroder%s\n' "${BLD}" "${RST}" "${BLD}" "${RST}"
    printf '    %sNo account? moroder -a   Help: https://github.com/%s/blob/main/docs/sign-in.md%s\n\n' "${DIM}" "$REPO" "${RST}"
fi
