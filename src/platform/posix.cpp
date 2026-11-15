// macOS and Linux side of include/utils/platform.hpp.
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <cstdint>
#include <filesystem>
#include "../../include/utils/platform.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace platform {

void initConsole() {
    // A child that died (the resolver's Python) must not kill us on the next write.
    std::signal(SIGPIPE, SIG_IGN);
}

bool isTerminal(int fd) { return ::isatty(fd) != 0; }

void setEnv(const std::string& name, const std::string& value) { ::setenv(name.c_str(), value.c_str(), 1); }

fs::path homeDir() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path();
}

int terminalColumns() {
    winsize w{};
    return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 20 ? w.ws_col : 80;
}

int shell(const std::string& command_line, std::string* out, bool merge_stderr) {
    FILE* p = popen((command_line + (merge_stderr ? " 2>&1" : "")).c_str(), "r");
    if (!p) return -1;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) if (out) out->append(buf);
    const int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string selfPath() {
    std::error_code ec;
#ifdef __APPLE__
    char buf[4096]; uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return std::filesystem::weakly_canonical(buf, ec).string();
    return "";
#else
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}

std::string which(const std::string& program) {
    std::string out;
    if (shell("command -v " + shellQuote(program) + " 2>/dev/null", &out) != 0) return "";
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

const char* quietStderr() { return " 2>/dev/null"; }

// ---------------------------------------------------------------------------

struct Process::Impl {
    pid_t pid = 0;
    int to = -1, from = -1;
    int status = -1;          // exit code once reaped; -1 when killed or unknown
};

namespace {

// A pipe whose ends are close-on-exec from the start: another thread's spawn
// can't inherit them (Linux has no POSIX_SPAWN_CLOEXEC_DEFAULT). The child's
// own ends are dup2'd onto 0 / 1, which clears the flag there.
bool cloexecPipe(int fds[2]) {
#ifdef __linux__
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

int exitCode(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }

}

Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() { kill(); }

bool Process::start(const std::vector<std::string>& args) {
    kill();
    if (args.empty()) return false;
    int in[2], out[2];
    if (!cloexecPipe(in)) return false;
    if (!cloexecPipe(out)) { close(in[0]); close(in[1]); return false; }
#ifdef F_SETNOSIGPIPE
    fcntl(in[1], F_SETNOSIGPIPE, 1);
#endif

    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (auto& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    // Only the fds set up above reach the child (other spawns' pipes would
    // keep their readers from ever seeing EOF).
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = 0;
    const bool ok = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ) == 0;
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if (!ok) { close(in[1]); close(out[0]); return false; }

    impl_->pid = pid; impl_->to = in[1]; impl_->from = out[0]; impl_->status = -1;
    return true;
}

bool Process::running() const {
    if (impl_->pid <= 0) return false;
    int status = 0;
    const pid_t r = waitpid(impl_->pid, &status, WNOHANG);
    if (r == 0) return true;                       // still running
    impl_->status = r == impl_->pid ? exitCode(status) : -1;
    impl_->pid = 0;                                // reaped
    return false;
}

bool Process::write(const std::string& data) {
    if (impl_->to < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(impl_->to, data.data() + done, data.size() - done);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
        done += static_cast<size_t>(n);
    }
    return true;
}

int Process::read(std::string& into, int timeout_ms) {
    if (impl_->from < 0) return -1;
    pollfd p{impl_->from, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    char buf[16384];
    const ssize_t n = ::read(impl_->from, buf, sizeof buf);
    if (n <= 0) return -1;
    into.append(buf, static_cast<size_t>(n));
    return static_cast<int>(n);
}

void Process::closeStdin() {
    if (impl_->to >= 0) { close(impl_->to); impl_->to = -1; }
}

void Process::kill() {
    if (impl_->pid > 0) {
        ::kill(impl_->pid, SIGKILL);
        waitpid(impl_->pid, nullptr, 0);
        impl_->status = -1;                        // killed: not a success
    }
    closeStdin();
    if (impl_->from >= 0) close(impl_->from);
    impl_->pid = 0; impl_->from = -1;
}

int Process::wait() {
    closeStdin();
    if (impl_->pid > 0) {
        int status = 0;
        impl_->status = waitpid(impl_->pid, &status, 0) == impl_->pid ? exitCode(status) : -1;
    }
    if (impl_->from >= 0) close(impl_->from);
    impl_->pid = 0; impl_->from = -1;
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
#ifdef __APPLE__
    spawnDetached({ "open", url });
#else
    spawnDetached({ "xdg-open", url });
#endif
}

void restart(char** argv) {
    // The installed one, found on the PATH (not necessarily argv[0]).
    execvp("moroder", argv);
    execvp(argv[0], argv);
    std::_Exit(0);
}

// ---------------------------------------------------------------------------

struct RawInput::Saved { termios old{}; };

RawInput::RawInput() : saved_(std::make_unique<Saved>()) {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_->old) != 0) return;
    termios raw = saved_->old;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    ok_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

RawInput::~RawInput() {
    if (ok_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_->old);
}

int RawInput::read(char* buf, int size, int timeout_ms) {
    if (!ok_) return -1;
    pollfd p{STDIN_FILENO, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    const ssize_t n = ::read(STDIN_FILENO, buf, static_cast<size_t>(size));
    return n < 0 ? -1 : static_cast<int>(n);
}

bool writeTerminal(const std::string& bytes) {
    return ::write(STDOUT_FILENO, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
}

bool restrictToOwner(const fs::path& file) { return chmod(file.c_str(), S_IRUSR | S_IWUSR) == 0; }

bool writePrivateFile(const fs::path& file, const std::string& content) {
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = ::write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size());
    ::close(fd);
    return ok;
}

bool utcTime(long long seconds, struct tm* out) {
    const time_t t = static_cast<time_t>(seconds);
    return gmtime_r(&t, out) != nullptr;
}

long long modifiedAt(const fs::path& file) {
    struct stat st{};
    return ::stat(file.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0;
}

}
