/* Sign in to YouTube Music in a browser window of its own, and take the
session from the browser through its DevTools protocol (see
src/setup/browser_window.cpp). */
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace setup {

// The Chromium browser the window would open in ("Chrome", "Edge"...), "" if none.
std::string browserWindowName();

struct WindowOptions {
    bool headless = false;                                  // tests
    std::string url = "https://accounts.google.com/ServiceLogin?continue=https%3A%2F%2Fmusic.youtube.com%2F";
    std::function<bool(const std::string& cookie)> done;   // default: a YouTube sign-in
};

/* Opens the window and waits until you're signed in (cancelled() polled
meanwhile): the youtube.com Cookie header, or "" with error set. */
std::string signInWithBrowserWindow(const std::filesystem::path& profile_dir,
                                    const std::function<bool()>& cancelled, std::string& error,
                                    const WindowOptions& options = {});

}
