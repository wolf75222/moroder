/* PLATFORM
- What differs between macOS / Linux and Windows, behind one small interface:
  the rest of Moroder calls these instead of POSIX or Win32 directly
- src/platform/posix.cpp: the code Moroder always ran on macOS and Linux
- src/platform/windows.cpp: the same on Win32 (CreateProcess, anonymous
  pipes, the console API)
*/
#pragma once

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace platform {

namespace fs = std::filesystem;

// At startup: UTF-8 and escape codes in the console (Windows); nothing on POSIX.
void initConsole();

// stdin = 0, stdout = 1, stderr = 2.
bool isTerminal(int fd);

void setEnv(const std::string& name, const std::string& value);

// HOME, or USERPROFILE on Windows. Empty when neither is set.
fs::path homeDir();

// Columns of the terminal on stdout, 80 when unknown.
int terminalColumns();

// The font of the console window stdout is in (Windows' own console); ""
// elsewhere or when unknown. Under Windows Terminal it says nothing useful.
std::string consoleFontName();

/* shell
- Runs a command line through the system shell (sh, cmd.exe), its stdout
  (and stderr when merge_stderr) into *out; returns the exit code, -1 when it
  could not start
*/
int shell(const std::string& command_line, std::string* out, bool merge_stderr = false);

// Full path of a program on the PATH, "" when absent.
std::string which(const std::string& program);

// This program's own path ("" if unknown). On Windows, where the installed
// copy lives even when this terminal's PATH doesn't list its folder yet.
std::string selfPath();

// Quotes one argument for a shell() command line.
std::string shellQuote(const std::string& value);

// Discards a command's stderr in a shell() command line: " 2>/dev/null" / " 2>NUL".
const char* quietStderr();

/* Process
- A program with pipes to its stdin and stdout, stderr discarded (or mixed
  into stdout with merge_stderr); argv[0] is looked up on the PATH. Killed
  when destroyed
*/
class Process {
public:
    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool start(const std::vector<std::string>& argv, bool merge_stderr = false);
    bool running() const;

    bool write(const std::string& data);
    // Waits up to timeout_ms: >0 bytes appended to into, 0 nothing yet, -1 closed.
    int read(std::string& into, int timeout_ms);

    void closeStdin();
    void kill();          // kills and reaps
    int wait();           // exit code

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Runs argv in the background (thread), its input from stdin_data, output
// discarded, reaped when done. For notifications and openers.
void spawnDetached(std::vector<std::string> argv, std::string stdin_data = "");

// Opens an https URL in the default browser.
void openUrl(const std::string& url);

/* restart
- Replaces this process with argv (the newly installed version): exec on
  POSIX; on Windows the new one runs, and this one exits with its code
*/
[[noreturn]] void restart(char** argv);

/* RawInput
- The terminal's input byte by byte, without echo or line editing, for
  `moroder keys` and the terminal capability probe. Restored when destroyed
*/
class RawInput {
public:
    RawInput();
    ~RawInput();
    bool ok() const { return ok_; }
    // Waits up to timeout_ms: bytes read, 0 when none, -1 on error.
    int read(char* buf, int size, int timeout_ms);

private:
    bool ok_ = false;
    struct Saved;
    std::unique_ptr<Saved> saved_;
};

// Writes straight to the terminal (stdout), unbuffered.
bool writeTerminal(const std::string& bytes);

// Readable and writable by its owner only (0600). No-op on Windows, where a
// file under the user's profile is private already.
bool restrictToOwner(const fs::path& file);

// Modification time in seconds since the epoch, 0 when unknown.
long long modifiedAt(const fs::path& file);

/* writePrivateFile
- Writes content to file, readable by its owner only, never through a
  symlink planted in its place (O_NOFOLLOW on POSIX)
*/
bool writePrivateFile(const fs::path& file, const std::string& content);

// gmtime_r / gmtime_s.
bool utcTime(long long seconds, struct tm* out);

}
