#include "../../include/services/notify.hpp"

#include <spdlog/spdlog.h>

#include "../../include/utils/platform.hpp"

#include <filesystem>
#include <mutex>
#include <cstdlib>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <vector>

namespace {

// Runs argv without a shell, so titles never need escaping.
void spawnAndReap(std::vector<std::string> args) { platform::spawnDetached(std::move(args)); }

}

#ifdef __APPLE__
namespace {

/* helperApp
- Moroder.app sits next to the binary in a build tree, and in
  <prefix>/share/moroder once installed
- Registered with LaunchServices once, so Notification Center knows its name
  and icon even if it was never opened from Finder
*/
std::string helperBinary() {
    static std::once_flag once;
    static std::string binary;

    std::call_once(once, [] {
        namespace fs = std::filesystem;

        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) != 0) return;

        std::error_code ec;
        const fs::path exe = fs::canonical(buf, ec);
        if (ec) return;

        for (const fs::path& app : { exe.parent_path() / "Moroder.app",
                                     exe.parent_path().parent_path() / "share" / "moroder" / "Moroder.app" }) {
            const fs::path bin = app / "Contents" / "MacOS" / "moroder-notify";
            if (fs::exists(bin, ec)) {
                binary = bin.string();
                spawnAndReap({ "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",
                               "-f", app.string() });
                return;
            }
        }
    });

    return binary;
}

}
#endif

bool services::copyToClipboard(const std::string& text) {
#ifdef __APPLE__
    std::vector<std::string> cmd = { "pbcopy" };
#elif defined(_WIN32)
    std::vector<std::string> cmd = { "powershell", "-NoProfile", "-Command", "[Console]::InputEncoding=[Text.Encoding]::UTF8; Set-Clipboard -Value ([Console]::In.ReadToEnd())" };
#else
    std::vector<std::string> cmd = getenv("WAYLAND_DISPLAY") ? std::vector<std::string>{ "wl-copy" }
                                                             : std::vector<std::string>{ "xclip", "-selection", "clipboard" };
#endif
    platform::Process p;
    if (!p.start(cmd)) return false;
    p.write(text);
    p.closeStdin();
    return p.wait() == 0;
}

void services::openUrl(const std::string& url) { platform::openUrl(url); }

void services::notifyTrack(const std::string& title, const std::string& subtitle, const std::string& cover) {
    if (title.empty()) return;

#ifdef __APPLE__
    // The helper app gives the notification Moroder's icon and the cover.
    const std::string helper = helperBinary();
    if (!helper.empty()) {
        spawnAndReap({ helper, title, subtitle, cover });
        return;
    }

    // Fallback: osascript. Title and text go in as script arguments, never
    // spliced into the script.
    spawnAndReap({
        "osascript",
        "-e", "on run argv",
        "-e", "display notification (item 2 of argv) with title (item 1 of argv)",
        "-e", "end run",
        title, subtitle,
    });
#elif defined(_WIN32)
    /* A Windows toast through PowerShell's WinRT access. Title and text come
    in on stdin, one per line: -Command would splice extra arguments into the
    script itself.
    */
    static const char* script =
        "[Console]::InputEncoding=[Text.Encoding]::UTF8;$l=[Console]::In.ReadToEnd() -split \"`n\";"
        "$null=[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime];"
        "$t=[Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
        "$n=$t.GetElementsByTagName('text');$null=$n.Item(0).AppendChild($t.CreateTextNode($l[0]));"
        "$null=$n.Item(1).AppendChild($t.CreateTextNode($l[1]));"
        "$id='{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe';"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($id).Show([Windows.UI.Notifications.ToastNotification]::new($t))";
    auto oneLine = [](std::string v) { for (char& c : v) if (c == '\n' || c == '\r') c = ' '; return v; };
    platform::spawnDetached({ "powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", script },
                            oneLine(title) + "\n" + oneLine(subtitle) + "\n");
    (void)cover;
#else
    spawnAndReap({ "notify-send", "--app-name=Moroder", "--expire-time=4000", title, subtitle });
#endif
}
