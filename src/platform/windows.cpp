// Windows side of include/utils/platform.hpp (Win32: CreateProcess, anonymous
// pipes, the console API). Strings are UTF-8 inside Moroder, converted to
// UTF-16 at the Win32 boundary.
#include "../../include/utils/platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <io.h>
#include <process.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace platform {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// One argument for a CreateProcess command line (the MSVC runtime's rules:
// backslashes are literal unless they precede a quote).
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0; ; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { i++; backslashes++; }
        if (i == arg.size()) { out.append(backslashes * 2, L'\\'); break; }
        if (arg[i] == L'"') { out.append(backslashes * 2 + 1, L'\\'); out.push_back(L'"'); }
        else { out.append(backslashes, L'\\'); out.push_back(arg[i]); }
    }
    out.push_back(L'"');
    return out;
}

HANDLE stdHandle(int fd) {
    return GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}

}

void initConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    for (DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = GetStdHandle(which);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

bool isTerminal(int fd) {
    DWORD mode = 0;
    return GetConsoleMode(stdHandle(fd), &mode) != 0;
}

void setEnv(const std::string& name, const std::string& value) {
    _putenv_s(name.c_str(), value.c_str());
    SetEnvironmentVariableW(wide(name).c_str(), wide(value).c_str());
}

fs::path homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
    if (const wchar_t* p = _wgetenv(L"USERPROFILE"); p && *p) return fs::path(p);
    return {};
}

int terminalColumns() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        const int cols = info.srWindow.Right - info.srWindow.Left + 1;
        if (cols > 20) return cols;
    }
    return 80;
}

// CONSOLE_FONT_INFOEX and GetCurrentConsoleFontEx sit behind NOGDI in the
// headers (kept on project-wide: GDI's RGB macro clashes with FTXUI's).
struct ConsoleFontInfoEx {
    ULONG cbSize;
    DWORD nFont;
    COORD dwFontSize;
    UINT FontFamily;
    UINT FontWeight;
    WCHAR FaceName[32];   // LF_FACESIZE
};
extern "C" __declspec(dllimport) BOOL WINAPI GetCurrentConsoleFontEx(HANDLE, BOOL, ConsoleFontInfoEx*);

std::string consoleFontName() {
    ConsoleFontInfoEx info{};
    info.cbSize = sizeof(info);
    if (!GetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &info)) return {};
    return narrow(info.FaceName);
}

bool legacyConsoleWindow() {
    // conhost's window is a ConsoleWindowClass; a pseudo console's is a hidden
    // PseudoConsoleWindow. ConEmu keeps a real one, but hidden.
    const HWND w = GetConsoleWindow();
    if (!w || !IsWindowVisible(w)) return false;
    wchar_t cls[64] = L"";
    GetClassNameW(w, cls, 64);
    return std::wstring(cls) == L"ConsoleWindowClass";
}

int run(const std::string& command_line) {
    // cmd.exe needs the whole line quoted once more when it starts with a quote.
    return _wsystem((L"\"" + wide(command_line) + L"\"").c_str());
}

int removeConsoleSettings(const std::string& exe_path) {
    std::wstring key = L"Console\\" + wide(exe_path);
    for (size_t i = 8; i < key.size(); i++) if (key[i] == L'\\' || key[i] == L'/') key[i] = L'_';
    const LSTATUS r = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    return r == ERROR_SUCCESS ? 1 : r == ERROR_FILE_NOT_FOUND ? 0 : -1;
}

int shell(const std::string& command_line, std::string* out, bool merge_stderr) {
    // cmd.exe needs the whole line quoted once more when it starts with a quote.
    const std::wstring cmd = L"\"" + wide(command_line + (merge_stderr ? " 2>&1" : "")) + L"\"";
    FILE* p = _wpopen(cmd.c_str(), L"rb");
    if (!p) return -1;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) if (out) out->append(buf);
    const int status = _pclose(p);
    if (out) {   // CRLF -> LF
        std::string clean;
        for (char c : *out) if (c != '\r') clean.push_back(c);
        *out = clean;
    }
    return status;
}

std::string selfPath() {
    wchar_t self[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof self / sizeof self[0]));
    return n ? narrow(std::wstring(self, n)) : std::string();
}

std::string which(const std::string& program) {
    std::string out;
    if (shell("where " + shellQuote(program) + " 2>NUL", &out) != 0) return "";
    const size_t nl = out.find('\n');
    return nl == std::string::npos ? out : out.substr(0, nl);
}

std::string shellQuote(const std::string& value) {
    // cmd.exe: double quotes, inner quotes doubled; % can't be escaped inside
    // quotes, so it is left as is (Moroder never passes user text with %VAR%).
    std::string out = "\"";
    for (char c : value) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

const char* quietStderr() { return " 2>NUL"; }

// ---------------------------------------------------------------------------

struct Process::Impl {
    HANDLE process = nullptr;
    HANDLE to = nullptr;      // our end of the child's stdin
    HANDLE from = nullptr;    // our end of the child's stdout
    int status = -1;          // exit code once it ended; -1 when killed or unknown
};

Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() { kill(); }

bool Process::start(const std::vector<std::string>& args, bool merge_stderr) {
    kill();
    if (args.empty()) return false;

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) return false;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) { CloseHandle(in_read); CloseHandle(in_write); return false; }
    // Our ends stay with us; only the child's ends are inherited.
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    // No NUL when stderr goes to stdout: the fallback below sends it there.
    HANDLE nul = merge_stderr ? INVALID_HANDLE_VALUE
                              : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring cmdline;
    for (const auto& a : args) { if (!cmdline.empty()) cmdline += L' '; cmdline += quoteArg(wide(a)); }

    // Only these handles reach the child (two when stderr shares stdout's
    // pipe), whatever else is inheritable.
    HANDLE inherit[3] = {in_read, out_write, nul};
    const SIZE_T inherited = (nul != INVALID_HANDLE_VALUE ? 3 : 2) * sizeof(HANDLE);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attr_buf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inherited, nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = nul != INVALID_HANDLE_VALUE ? nul : out_write;
    si.lpAttributeList = attrs_ok ? attrs : nullptr;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                   nullptr, nullptr, &si.StartupInfo, &pi);
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(in_write); CloseHandle(out_read); return false; }

    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess; impl_->to = in_write; impl_->from = out_read;
    return true;
}

bool Process::running() const {
    if (!impl_->process) return false;
    if (WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT) return true;
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(impl_->process, &code);
    impl_->status = static_cast<int>(code);
    CloseHandle(impl_->process);
    impl_->process = nullptr;
    return false;
}

bool Process::write(const std::string& data) {
    if (!impl_->to) return false;
    size_t done = 0;
    while (done < data.size()) {
        DWORD n = 0;
        if (!WriteFile(impl_->to, data.data() + done, static_cast<DWORD>(data.size() - done), &n, nullptr) || n == 0) return false;
        done += n;
    }
    return true;
}

int Process::read(std::string& into, int timeout_ms) {
    if (!impl_->from) return -1;
    // Anonymous pipes can't be waited on: peek until data, EOF or the timeout.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(impl_->from, nullptr, 0, nullptr, &available, nullptr)) return -1;   // closed
        if (available > 0) {
            char buf[16384];
            DWORD n = 0;
            if (!ReadFile(impl_->from, buf, std::min<DWORD>(available, sizeof buf), &n, nullptr) || n == 0) return -1;
            into.append(buf, n);
            return static_cast<int>(n);
        }
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        Sleep(10);
    }
}

void Process::closeStdin() {
    if (impl_->to) { CloseHandle(impl_->to); impl_->to = nullptr; }
}

void Process::kill() {
    if (impl_->process) {
        TerminateProcess(impl_->process, 1);
        WaitForSingleObject(impl_->process, 2000);
        CloseHandle(impl_->process);
        impl_->status = -1;
    }
    closeStdin();
    if (impl_->from) CloseHandle(impl_->from);
    impl_->process = nullptr; impl_->from = nullptr;
}

int Process::wait() {
    closeStdin();
    if (impl_->process) {
        DWORD code = static_cast<DWORD>(-1);
        WaitForSingleObject(impl_->process, INFINITE);
        GetExitCodeProcess(impl_->process, &code);
        CloseHandle(impl_->process);
        impl_->status = static_cast<int>(code);
    }
    if (impl_->from) CloseHandle(impl_->from);
    impl_->process = nullptr; impl_->from = nullptr;
    return impl_->status;
}

void spawnDetached(std::vector<std::string> args, std::string stdin_data) {
    std::thread([args = std::move(args), stdin_data = std::move(stdin_data)] {
        Process p;
        if (!p.start(args)) return;
        if (!stdin_data.empty()) p.write(stdin_data);
        p.closeStdin();
        std::string ignored;
        while (p.read(ignored, 1000) >= 0) ignored.clear();
        p.wait();
    }).detach();
}

void openUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return;
    ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void restart(char** argv) {
    // No exec on Windows: the new one runs in this console (same path: the
    // installer replaced the file), and this one leaves with its exit code.
    wchar_t self[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof self / sizeof self[0]));
    std::wstring cmdline = quoteArg(n ? std::wstring(self, n) : std::wstring(L"moroder.exe"));
    for (int i = 1; argv[i]; i++) cmdline += L" " + quoteArg(wide(argv[i]));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(n ? self : nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) std::exit(0);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    std::exit(static_cast<int>(code));
}

// ---------------------------------------------------------------------------

struct RawInput::Saved { DWORD in_mode = 0; DWORD out_mode = 0; };

RawInput::RawInput() : saved_(std::make_unique<Saved>()) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(in, &saved_->in_mode)) return;
    GetConsoleMode(out, &saved_->out_mode);
    // Keys arrive as the same escape sequences a Unix terminal sends.
    DWORD mode = saved_->in_mode;
    mode &= ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    ok_ = SetConsoleMode(in, mode) != 0;
    SetConsoleMode(out, saved_->out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

RawInput::~RawInput() {
    if (!ok_) return;
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_->in_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved_->out_mode);
}

int RawInput::read(char* buf, int size, int timeout_ms) {
    if (!ok_) return -1;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (WaitForSingleObject(in, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) return 0;
    // ReadFile on a VT-mode console gives the bytes; key-up and focus events
    // count as "ready" without producing any, so skip those.
    DWORD pending = 0;
    if (GetNumberOfConsoleInputEvents(in, &pending) && pending > 0) {
        INPUT_RECORD rec;
        DWORD n = 0;
        bool has_key = false;
        std::vector<INPUT_RECORD> recs(pending);
        if (PeekConsoleInputW(in, recs.data(), pending, &n))
            for (DWORD i = 0; i < n; i++) if (recs[i].EventType == KEY_EVENT && recs[i].Event.KeyEvent.bKeyDown) has_key = true;
        if (!has_key) { ReadConsoleInputW(in, &rec, 1, &n); return 0; }
    }
    DWORD n = 0;
    if (!ReadFile(in, buf, static_cast<DWORD>(size), &n, nullptr)) return -1;
    return static_cast<int>(n);
}

bool writeTerminal(const std::string& bytes) {
    DWORD n = 0;
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) && n == bytes.size();
}

bool restrictToOwner(const fs::path&) { return true; }

bool writePrivateFile(const fs::path& file, const std::string& content) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

bool utcTime(long long seconds, struct tm* out) {
    const time_t t = static_cast<time_t>(seconds);
    return gmtime_s(out, &t) == 0;
}

long long modifiedAt(const fs::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.wstring().c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER t;
    t.LowPart = data.ftLastWriteTime.dwLowDateTime;
    t.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return static_cast<long long>(t.QuadPart / 10000000ULL) - 11644473600LL;   // 1601 -> 1970
}

}
