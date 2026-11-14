// Opens a headless Chromium browser on a throwaway profile, lets YouTube set
// its first cookies, and reads them back over the DevTools protocol: checks
// the WebSocket client and the browser lookup (src/setup/browser_window.cpp)
// on a machine with Chrome / Edge, such as CI.
#include "../include/setup/browser_window.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>

int main() {
    const std::string browser = setup::browserWindowName();
    if (browser.empty()) { std::puts("SKIP: no Chromium browser installed"); return 0; }

    const auto profile = std::filesystem::temp_directory_path() / "moroder-browser-window-test";
    std::error_code ec;
    std::filesystem::remove_all(profile, ec);

    setup::WindowOptions options;
    options.headless = true;
    options.url = "https://www.youtube.com/";
    options.done = [](const std::string& cookie) { return !cookie.empty(); };

    const auto start = std::chrono::steady_clock::now();
    std::string error;
    const std::string cookie = setup::signInWithBrowserWindow(profile,
        [&] { return std::chrono::steady_clock::now() - start > std::chrono::seconds(60); }, error, options);
    std::filesystem::remove_all(profile, ec);

    if (cookie.empty()) { std::printf("FAIL: %s with %s\n", error.c_str(), browser.c_str()); return 1; }
    std::printf("OK: %zu bytes of youtube.com cookies from %s over DevTools\n", cookie.size(), browser.c_str());
    return 0;
}
