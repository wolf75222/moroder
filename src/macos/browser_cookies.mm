#include "../../include/services/browser_cookies.hpp"
#include "../../include/utils/utils.hpp"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonCrypto.h>
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
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

/* Key file
- The Keychain only lets a program in silently if that exact build was
  "always allowed"; every rebuild or update asks again, and the app reads the
  store at launch and every 10 minutes. So the derived AES key is asked for
  once and kept in ~/.config/moroder/<browser>.key (0600), next to
  browser.json, which already holds the session it unlocks
- If the key stops working (the browser reset its store), it is asked again
*/
fs::path keyFile(const BrowserInfo& info) {
    const char* home = getenv("HOME");
    if (!home) return {};
    std::string name = info.account;
    for (auto& ch : name) ch = (ch == ' ') ? '-' : static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return fs::path(home) / ".config" / "moroder" / (name + ".key");
}

bool readKeyFile(const BrowserInfo& info, std::vector<uint8_t>& key) {
    // Only a plain file of ours that nobody else can read (not a symlink).
    struct stat st{};
    const fs::path file = keyFile(info);
    if (lstat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) return false;

    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return false;
    std::vector<uint8_t> k((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (k.size() != kCCKeySizeAES128) return false;
    key = k;
    return true;
}

void writeKeyFile(const BrowserInfo& info, const std::vector<uint8_t>& key) {
    const fs::path file = keyFile(info);
    if (file.empty()) return;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);

    // Created 0600 from the start (no window where it is world-readable), and
    // never through a symlink.
    const fs::path tmp = file.string() + ".tmp";
    unlink(tmp.c_str());
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    fchmod(fd, 0600);
    const bool written = write(fd, key.data(), key.size()) == static_cast<ssize_t>(key.size());
    close(fd);
    if (!written || rename(tmp.c_str(), file.c_str()) != 0) unlink(tmp.c_str());
}

// The AES key: from memory, else the key file, else the Keychain (once).
bool storeKey(const BrowserInfo& info, std::vector<uint8_t>& key, std::string& error, bool from_keychain = false) {
    static std::mutex m;
    static std::map<std::string, std::vector<uint8_t>> cache;

    std::lock_guard lock(m);
    if (!from_keychain) {
        if (auto it = cache.find(info.service); it != cache.end()) { key = it->second; return true; }
        if (readKeyFile(info, key)) { cache[info.service] = key; return true; }
    }

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
    writeKeyFile(info, derived);
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
    return browsers().count(utils::lower(browser.substr(0, browser.find(':')))) > 0;
}

namespace {

// A profile's cookie store: Network/Cookies (Chromium 96+), else Cookies.
fs::path cookieStore(const fs::path& profile) {
    std::error_code ec;
    for (const fs::path& p : {profile / "Network" / "Cookies", profile / "Cookies"})
        if (fs::exists(p, ec)) return p;
    return {};
}

// Default first, then Profile 1, Profile 2...
std::vector<fs::path> profileDirs(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(root / "Default", ec)) out.push_back(root / "Default");
    std::vector<fs::path> more;
    for (auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && e.path().filename().string().rfind("Profile ", 0) == 0) more.push_back(e.path());
    std::sort(more.begin(), more.end());
    out.insert(out.end(), more.begin(), more.end());
    return out;
}

// Whether the store has a YouTube login cookie (read from a private copy: the
// browser keeps it locked). Names only, nothing decrypted.
bool hasYouTubeLogin(const fs::path& store) {
    std::string tmpl = (fs::temp_directory_path() / "moroder-probe-XXXXXX.db").string();
    const int fd = mkstemps(tmpl.data(), 3);
    if (fd < 0) return false;
    close(fd);
    std::error_code ec;
    fs::copy_file(store, tmpl, fs::copy_options::overwrite_existing, ec);
    bool found = false;
    sqlite3* db = nullptr;
    if (!ec && sqlite3_open_v2(tmpl.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        const char* sql = "SELECT 1 FROM cookies WHERE host_key IN ('.youtube.com', 'youtube.com') "
                          "AND name IN ('SAPISID', '__Secure-3PAPISID', '__Secure-1PAPISID') LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) found = true;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    fs::remove(tmpl, ec);
    return found;
}

// The name the browser shows for a profile ("Personal", "Work"), from Local State.
std::string profileName(const fs::path& root, const std::string& dir) {
    NSString* path = [NSString stringWithUTF8String:(root / "Local State").c_str()];
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (!data) return dir;
    NSDictionary* json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:[NSDictionary class]]) return dir;
    id cache = json[@"profile"][@"info_cache"][[NSString stringWithUTF8String:dir.c_str()]];
    if (![cache isKindOfClass:[NSDictionary class]]) return dir;
    id name = cache[@"name"];
    return [name isKindOfClass:[NSString class]] && [name length] ? std::string([name UTF8String]) : dir;
}

}

std::vector<std::pair<std::string, std::string>> services::signedInProfiles(const std::string& browser) {
    std::vector<std::pair<std::string, std::string>> out;
    auto it = browsers().find(utils::lower(browser));
    const fs::path home = utils::home();
    if (it == browsers().end() || home.empty()) return out;
    const fs::path root = home / "Library" / "Application Support" / it->second.dir;
    for (const fs::path& dir : profileDirs(root)) {
        const fs::path store = cookieStore(dir);
        if (!store.empty() && hasYouTubeLogin(store))
            out.push_back({dir.filename().string(), profileName(root, dir.filename().string())});
    }
    return out;
}

std::string services::readYouTubeCookies(const std::string& spec, std::string& error) {
    const size_t colon = spec.find(':');
    const std::string browser = spec.substr(0, colon);
    const std::string wanted = colon == std::string::npos ? "" : spec.substr(colon + 1);

    auto it = browsers().find(utils::lower(browser));
    if (it == browsers().end()) { error = "unsupported browser '" + browser + "'"; return ""; }
    const BrowserInfo& info = it->second;

    const fs::path home = utils::home();
    if (home.empty()) { error = "HOME not set"; return ""; }

    const fs::path root = home / "Library" / "Application Support" / info.dir;

    // The profile asked for, else the first one signed in, else the first one.
    fs::path store;
    std::error_code ec;
    if (!wanted.empty()) {
        store = cookieStore(root / wanted);
    } else {
        for (const fs::path& dir : profileDirs(root)) {
            const fs::path s = cookieStore(dir);
            if (s.empty()) continue;
            if (store.empty()) store = s;
            if (hasYouTubeLogin(s)) { store = s; break; }
        }
    }
    if (store.empty()) { error = "no cookie store under " + root.string(); return ""; }

    std::vector<uint8_t> key;
    if (!storeKey(info, key, error)) return "";

    // The browser keeps the database open and locked: read a private copy,
    // in a unique 0600 temp file.
    std::string tmpl = (fs::temp_directory_path() / "moroder-cookies-XXXXXX.db").string();
    const int tfd = mkstemps(tmpl.data(), 3);
    if (tfd < 0) { error = "could not create a temp file"; return ""; }
    close(tfd);
    const fs::path tmp = tmpl;
    fs::copy_file(store, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "could not copy " + store.string() + ": " + ec.message(); fs::remove(tmp, ec); return ""; }
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

    for (int attempt = 0; attempt < 2 && jar.empty(); attempt++) {
        int encrypted = 0, failed = 0;

        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string name = (const char*)sqlite3_column_text(st, 0);
                std::string value = (const char*)sqlite3_column_text(st, 1);

                if (value.empty()) {
                    const void* blob = sqlite3_column_blob(st, 2);
                    const int n = sqlite3_column_bytes(st, 2);
                    encrypted++;
                    if (!blob || !decrypt(key, blob, (size_t)n, version >= 24, value)) { failed++; continue; }
                }

                if (!jar.count(name)) order.push_back(name);
                jar[name] = value;
            }
        }
        sqlite3_finalize(st);

        // The saved key no longer opens the store: ask the Keychain again.
        if (attempt == 0 && encrypted > 0 && failed == encrypted) {
            jar.clear();
            order.clear();
            if (!storeKey(info, key, error, true)) break;
        } else {
            break;
        }
    }
    sqlite3_close(db);

    if (jar.empty()) { error = "no YouTube cookies in " + browser + "; sign in to music.youtube.com there"; return ""; }

    std::string header;
    for (const std::string& name : order) {
        if (!header.empty()) header += "; ";
        header += name + "=" + jar[name];
    }
    return header;
}
