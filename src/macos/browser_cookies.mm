#include "../../include/services/browser_cookies.hpp"
#include "../../include/utils/utils.hpp"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonCrypto.h>
#include <sqlite3.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct BrowserInfo {
    const char* dir;        // under ~/Library/Application Support
    const char* service;    // Keychain item
    const char* account;
};

const std::map<std::string, BrowserInfo>& browsers() {
    static const std::map<std::string, BrowserInfo> table = {
        {"arc",      {"Arc/User Data",                "Arc Safe Storage",            "Arc"}},
        {"chrome",   {"Google/Chrome",                "Chrome Safe Storage",         "Chrome"}},
        {"brave",    {"BraveSoftware/Brave-Browser",  "Brave Safe Storage",          "Brave"}},
        {"edge",     {"Microsoft Edge",               "Microsoft Edge Safe Storage", "Microsoft Edge"}},
        {"chromium", {"Chromium",                     "Chromium Safe Storage",       "Chromium"}},
        {"vivaldi",  {"Vivaldi",                      "Vivaldi Safe Storage",        "Vivaldi"}},
    };
    return table;
}

// The AES key, derived once per run from the Keychain secret.
bool storeKey(const BrowserInfo& info, std::vector<uint8_t>& key, std::string& error) {
    static std::mutex m;
    static std::map<std::string, std::vector<uint8_t>> cache;

    std::lock_guard lock(m);
    if (auto it = cache.find(info.service); it != cache.end()) { key = it->second; return true; }

    NSDictionary* query = @{
        (__bridge id)kSecClass:       (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: [NSString stringWithUTF8String:info.service],
        (__bridge id)kSecAttrAccount: [NSString stringWithUTF8String:info.account],
        (__bridge id)kSecReturnData:  @YES,
    };

    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (st != errSecSuccess || !result) {
        error = std::string("no access to the Keychain item \"") + info.service + "\" (" + std::to_string((int)st) + ")";
        return false;
    }

    NSData* secret = (__bridge_transfer NSData*)result;

    // Chromium on macOS: PBKDF2-HMAC-SHA1, salt "saltysalt", 1003 rounds, 16 bytes.
    std::vector<uint8_t> derived(kCCKeySizeAES128);
    const char salt[] = "saltysalt";
    CCKeyDerivationPBKDF(kCCPBKDF2, (const char*)secret.bytes, secret.length,
                         (const uint8_t*)salt, sizeof(salt) - 1, kCCPRFHmacAlgSHA1, 1003,
                         derived.data(), derived.size());

    cache[info.service] = derived;
    key = derived;
    return true;
}

// "v10" + AES-128-CBC (IV of 16 spaces, PKCS7). Since store version 24 the
// plaintext starts with a 32-byte SHA-256 of the host.
bool decrypt(const std::vector<uint8_t>& key, const void* blob, size_t n, bool hashed_prefix, std::string& out) {
    if (n < 3 || memcmp(blob, "v10", 3) != 0) return false;

    const uint8_t* data = (const uint8_t*)blob + 3;
    const size_t len = n - 3;

    std::vector<uint8_t> plain(len + kCCBlockSizeAES128);
    size_t produced = 0;
    uint8_t iv[kCCBlockSizeAES128];
    memset(iv, ' ', sizeof(iv));

    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.data(), key.size(), iv,
                data, len, plain.data(), plain.size(), &produced) != kCCSuccess) {
        return false;
    }

    size_t skip = hashed_prefix && produced >= 32 ? 32 : 0;
    out.assign((const char*)plain.data() + skip, produced - skip);
    return true;
}

}

bool services::isCookieStoreBrowser(const std::string& browser) {
    return browsers().count(utils::lower(browser)) > 0;
}

std::string services::readYouTubeCookies(const std::string& browser, std::string& error) {
    auto it = browsers().find(utils::lower(browser));
    if (it == browsers().end()) { error = "unsupported browser '" + browser + "'"; return ""; }
    const BrowserInfo& info = it->second;

    const char* home = getenv("HOME");
    if (!home) { error = "HOME not set"; return ""; }

    const fs::path root = fs::path(home) / "Library" / "Application Support" / info.dir;

    // The default profile, else the first one that has a cookie store.
    fs::path store;
    std::error_code ec;
    if (fs::exists(root / "Default" / "Cookies", ec)) {
        store = root / "Default" / "Cookies";
    } else {
        for (auto& e : fs::directory_iterator(root, ec)) {
            if (e.path().filename().string().rfind("Profile", 0) == 0 && fs::exists(e.path() / "Cookies", ec)) {
                store = e.path() / "Cookies";
                break;
            }
        }
    }
    if (store.empty()) { error = "no cookie store under " + root.string(); return ""; }

    std::vector<uint8_t> key;
    if (!storeKey(info, key, error)) return "";

    // The browser keeps the database open and locked: read a private copy.
    const fs::path tmp = fs::temp_directory_path() / ("moroder-cookies-" + std::to_string(getpid()) + ".db");
    fs::copy_file(store, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "could not copy " + store.string() + ": " + ec.message(); return ""; }
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove(p, e); } } cleanup{tmp};

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(tmp.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "could not open the cookie store";
        sqlite3_close(db);
        return "";
    }

    int version = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key='version'", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        version = atoi((const char*)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);

    // Domain cookies first, then host cookies for music.youtube.com override.
    const char* sql =
        "SELECT name, value, encrypted_value FROM cookies "
        "WHERE host_key IN ('.youtube.com', 'youtube.com', '.music.youtube.com', 'music.youtube.com') "
        "ORDER BY length(host_key)";

    std::map<std::string, std::string> jar;
    std::vector<std::string> order;

    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::string name = (const char*)sqlite3_column_text(st, 0);
            std::string value = (const char*)sqlite3_column_text(st, 1);

            if (value.empty()) {
                const void* blob = sqlite3_column_blob(st, 2);
                const int n = sqlite3_column_bytes(st, 2);
                if (!blob || !decrypt(key, blob, (size_t)n, version >= 24, value)) continue;
            }

            if (!jar.count(name)) order.push_back(name);
            jar[name] = value;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (jar.empty()) { error = "no YouTube cookies in " + browser + "; sign in to music.youtube.com there"; return ""; }

    std::string header;
    for (const std::string& name : order) {
        if (!header.empty()) header += "; ";
        header += name + "=" + jar[name];
    }
    return header;
}
