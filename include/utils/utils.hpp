/*
When I was fifteen, sixteen, when I really started to code,
I definitely wanted to make a YouTube player.
It was almost impossible because—it was—the dream was so big
That I didn't see any chance because
I was living in a little town; was studying.
And when I finally broke away from school and became a programmer
I thought, "Well, now I may have a little bit of a chance"
Because all I really wanted to do is code
And not only code, but design something clean.

At that time, on the internet, in '09, '10, they already had TUIs,
So I would open my laptop, would go to a café,
Code maybe thirty minutes,
I think I had about seven, eight functions,
I would partially sleep at the keyboard
Because I didn't want to drive home,
And that helped me for about
Almost two years to survive in the beginning.

I wanted to make a player with the feel of the '80s,
The look of the '90s, the speed of the 2000s,
And then have an interface of the future.
And I said, "Wait a second, I know C++,
Why don't I use C++ which is the language of the future?"

And I didn't have any idea what to do,
But I knew I needed a loop, so I put a loop on the main thread
Which then was synced to the ftxui display.
I knew that could be the feel of the future,
*/

#pragma once

#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <cstdlib>
#include <string>
#include <cctype>

namespace fs = std::filesystem;

namespace utils {

inline fs::path resolve_path(const char* path_macro) {
    fs::path p(path_macro);

    if (p.is_absolute()) {
        return p;
    }

    // HOME, or USERPROFILE on Windows.
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    if (!home) {
        throw std::runtime_error("HOME not set");
    }

    return fs::path(home) / p;
}

/* ensure_dir
- Throws on failure. The exception used to be constructed and then dropped, so
  a directory that could not be created carried on silently and only surfaced
  later as an unrelated file error
*/
inline void ensure_dir(const fs::path& path) {
    std::error_code ec;

    if (fs::exists(path, ec)) return;

    fs::create_directories(path, ec);

    // create_directories returns false when the directory already exists, so
    // the error code is what decides, not the return value.
    if (ec) {
        throw std::runtime_error("Failed to create: " + path.string() + " (" + ec.message() + ")");
    }
}

/* home
- Empty rather than throwing, for the callers that want to fall back instead
  of failing
*/
inline fs::path home() {
    const char* h = std::getenv("HOME");
    if (!h || !*h) h = std::getenv("USERPROFILE");     // Windows
    return h ? fs::path(h) : fs::path();
}

inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";

    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/* tildify
- The inverse of resolve_path: puts ~ back in front of a path under HOME, so
  what gets written to the config stays readable and portable between machines
*/
inline std::string tildify(const fs::path& path) {
    const fs::path h = home();
    if (h.empty()) return path.string();

    const std::string full = path.string();
    const std::string prefix = h.string();

    if (full.rfind(prefix, 0) == 0) {
        if (full.size() == prefix.size()) return "~";
        if (full[prefix.size()] == '/' || full[prefix.size()] == '\\') return "~" + full.substr(prefix.size());
    }

    return full;
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
};

/* Text lengths in characters, not bytes: cutting "Schäfer" in the middle of
"ä" printed a broken character, and accented titles were cut too early.
*/
inline size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

// The first `chars` characters of s.
inline std::string utf8Prefix(const std::string& s, size_t chars) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == chars) break;
            n++;
        }
        i++;
    }
    return s.substr(0, i);
}

inline std::string trimSuffix(const std::string& str, size_t max_size, const std::string& suffix) {
    if (utf8Length(str) <= max_size)
        return str;

    const size_t suffix_len = utf8Length(suffix);
    if (suffix_len >= max_size)
        return utf8Prefix(suffix, max_size);

    // No space before the suffix ("Cornelia ..." reads as a cut word).
    std::string head = utf8Prefix(str, max_size - suffix_len);
    while (!head.empty() && head.back() == ' ') head.pop_back();
    return head + suffix;
}

inline std::string trimWordsSuffix(const std::string& str, size_t max_size, const std::string& suffix) {
    if (utf8Length(str) <= max_size)
        return str;

    const size_t suffix_len = utf8Length(suffix);
    if (suffix_len >= max_size)
        return utf8Prefix(suffix, max_size);

    size_t limit = max_size - suffix_len;

    std::string result;
    std::istringstream iss(str);
    std::string word;

    while (iss >> word) {
        std::string candidate = result.empty() ? word : result + " " + word;

        if (utf8Length(candidate) > limit)
            break;

        result = candidate;
    }

    if (result.empty())
        return utf8Prefix(str, limit) + suffix;

    return result + suffix;
}

}
