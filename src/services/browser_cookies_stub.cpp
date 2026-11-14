// Cookie-store reading is macOS-only for now (see browser_cookies.mm).
#include "../../include/services/browser_cookies.hpp"

bool services::isCookieStoreBrowser(const std::string&) { return false; }

std::string services::readYouTubeCookies(const std::string&, std::string& error) {
    error = "reading a browser's cookie store is only supported on macOS";
    return "";
}
