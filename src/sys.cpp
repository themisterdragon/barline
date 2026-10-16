#include "sys.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#endif

namespace sys {

#ifdef _WIN32
// Windows takes file names as UTF-16; Barline keeps them as UTF-8
static std::wstring wide(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

bool is_dir(const std::string &path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool exists(const std::string &path) {
#ifdef _WIN32
    return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

long long file_size(const std::string &path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long long)st.st_size : -1;
#endif
}

bool make_dir(const std::string &path, int mode) {
#ifdef _WIN32
    (void)mode;
    CreateDirectoryW(wide(path).c_str(), nullptr);
#else
    mkdir(path.c_str(), (mode_t)mode);
#endif
    return is_dir(path);
}

std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(wide(dir + "/*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = narrow(f.cFileName);
        if (n != "." && n != "..") out.push_back(n);
    } while (FindNextFileW(h, &f));
    FindClose(h);
#else
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d))
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) out.push_back(e->d_name);
        closedir(d);
    }
#endif
    return out;
}

bool replace_file(const std::string &from, const std::string &to) {
#ifdef _WIN32
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

static bool write_raw(const std::string &path, const std::string &text, bool append) {
#ifdef _WIN32
    HANDLE h = CreateFileW(wide(path).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = text.empty() || (WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size());
    CloseHandle(h);
    return ok;
#else
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0600);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    if (!append) ok = fsync(fd) == 0 && ok;
    close(fd);
    return ok;
#endif
}

bool write_private(const std::string &path, const std::string &text) {
    std::string tmp = path + ".tmp";
    if (!write_raw(tmp, text, false)) return false;
    return replace_file(tmp, path);
}

bool append_private(const std::string &path, const std::string &text) { return write_raw(path, text, true); }

std::string exe_path() {
#ifdef _WIN32
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, b, (DWORD)(sizeof b / sizeof b[0]));
    if (!n) return "";
    std::string s = narrow(b);
    for (auto &c : s)
        if (c == '\\') c = '/';
    return s;
#elif defined(__APPLE__)
    char b[4096];
    uint32_t n = sizeof b;
    if (_NSGetExecutablePath(b, &n) != 0) return "";
    char r[4096];
    return realpath(b, r) ? r : b;
#else
    char b[4096];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    if (n <= 0) return "";
    b[n] = 0;
    return b;
#endif
}

std::string host_name() {
    char h[256] = {};
#ifdef _WIN32
    DWORD n = sizeof h;
    if (!GetComputerNameA(h, &n)) return "";
#else
    if (gethostname(h, sizeof h - 1) != 0) return "";
#endif
    std::string s = h;
    return s.substr(0, s.find('.'));
}

bool is_terminal() {
#ifdef _WIN32
    return _isatty(0) && _isatty(1);
#else
    return isatty(0) && isatty(1);
#endif
}

bool double_clicked() {
#ifdef _WIN32
    DWORD list[2];
    if (GetConsoleProcessList(list, 2) == 1) {   // nobody else in this console: Explorer made it for us
        FreeConsole();
        return true;
    }
#endif
    return false;
}

void local_time(std::time_t t, std::tm *out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

#ifdef _WIN32
static HANDLE g_lock = INVALID_HANDLE_VALUE;
static HANDLE open_lock(const std::string &path) {
    // no sharing: while one copy holds it open, nobody else can open it
    return CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
bool take_lock(const std::string &path) {
    if (g_lock != INVALID_HANDLE_VALUE) return true;
    g_lock = open_lock(path);
    return g_lock != INVALID_HANDLE_VALUE;
}
bool lock_is_held(const std::string &path) {
    if (!exists(path)) return false;
    HANDLE h = open_lock(path);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}
#else
static int g_lock = -1;
bool take_lock(const std::string &path) {
    if (g_lock >= 0) return true;
    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return true;   // can't make the file: don't stop Barline over it
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    g_lock = fd;
    return true;
}
bool lock_is_held(const std::string &path) {
    int fd = open(path.c_str(), O_RDWR);   // (no file: it's never run, so it isn't running)
    if (fd < 0) return false;
    bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    return held;
}
#endif

bool edit_text_file(const std::string &path) {
#ifdef _WIN32
    // the editor named in EDITOR, else Notepad
    const char *e = std::getenv("EDITOR");
    std::wstring cmd = L"\"" + wide(e && *e ? e : "notepad.exe") + L"\" \"" + wide(path) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c",
              "e=\"${VISUAL:-$EDITOR}\"; if [ -z \"$e\" ]; then if command -v nano >/dev/null 2>&1; then e=nano; else e=vi; fi; fi; exec $e \"$1\"",
              "sh", path.c_str(), (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    if (pid > 0) waitpid(pid, &status, 0);
    return pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) != 127;
#endif
}

void fine_timers() {
#ifdef _WIN32
    static bool done = false;
    if (done) return;
    done = true;
    // timeBeginPeriod(1), looked up so Barline doesn't need winmm just for this
    if (HMODULE m = LoadLibraryA("winmm.dll"))
        if (auto f = (UINT(WINAPI *)(UINT))(void *)GetProcAddress(m, "timeBeginPeriod")) f(1);
#endif
}

void audio_thread_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    // "Pro Audio" scheduling (MMCSS), when it's there
    if (HMODULE m = LoadLibraryA("avrt.dll"))
        if (auto f = (HANDLE(WINAPI *)(LPCWSTR, LPDWORD))(void *)GetProcAddress(m, "AvSetMmThreadCharacteristicsW")) {
            DWORD task = 0;
            f(L"Pro Audio", &task);
        }
#elif defined(__APPLE__)
    // a time-constraint thread, as Core Audio's own are: about 3 ms of work in every 3 ms
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ms = 1e6 * tb.denom / tb.numer;   // mach units in a millisecond
    thread_time_constraint_policy_data_t p;
    p.period = (uint32_t)(3 * ms);
    p.computation = (uint32_t)(1 * ms);
    p.constraint = (uint32_t)(2 * ms);
    p.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
    sched_param p{};
    p.sched_priority = 70;   // works when the user may (rtprio in limits.conf); harmless otherwise
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
#endif
}

}  // namespace sys
