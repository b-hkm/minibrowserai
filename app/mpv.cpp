#include "mpv.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <unistd.h>   // fork, execv, access, setsid, dup2, kill

#include "../net/url.h"

namespace browser {
namespace mpv {

// ---------------------------------------------------------------------------
// PATH helpers
// ---------------------------------------------------------------------------

static bool executable(const std::string& p) {
    return ::access(p.c_str(), X_OK) == 0;
}

// Search PATH for `name`; return the absolute path ("" when not found).
static std::string findInPath(const std::string& name) {
    const char* pathEnv = ::getenv("PATH");
    if (!pathEnv || pathEnv[0] == '\0') return "";
    std::string spath = pathEnv;
    size_t start = 0;
    while (start <= spath.size()) {
        size_t colon = spath.find(':', start);
        std::string dir = spath.substr(
            start, colon == std::string::npos ? std::string::npos
                                              : colon - start);
        if (dir.empty()) dir = ".";
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
        std::string cand = dir + "/" + name;
        if (executable(cand)) return cand;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

bool available() {
    static bool cached = !path().empty();
    return cached;
}

std::string path() {
    static std::string cached = findInPath("mpv");
    return cached;
}

// ---------------------------------------------------------------------------
// Diagnostic log: everything mpv prints lands here (truncated per launch)
// ---------------------------------------------------------------------------

std::string logPath() {
    const char* tmp = ::getenv("TMPDIR");
    return std::string(tmp && tmp[0] ? tmp : "/tmp") +
           "/mini-browser-mpv.log";
}

std::string logTail(int maxLines) {
    if (maxLines <= 0) return "";
    FILE* f = std::fopen(logPath().c_str(), "r");
    if (!f) return "";
    std::string lines[8];
    int n = 0;
    if (maxLines > 8) maxLines = 8;
    char buf[512];
    while (std::fgets(buf, sizeof buf, f)) {
        size_t len = std::strlen(buf);
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (len == 0) continue;
        lines[n % maxLines] = buf;   // ring buffer keeps the LAST maxLines
        ++n;
    }
    std::fclose(f);
    std::string out;
    int first = n < maxLines ? 0 : n - maxLines;
    for (int i = first; i < n; ++i) {
        const std::string& l = lines[i % maxLines];
        if (out.size() + l.size() > 160) {          // status-bar friendly
            if (out.empty() && l.size() > 160) out = l.substr(0, 160);
            break;
        }
        if (!out.empty()) out += " | ";
        out += l;
    }
    return out;
}

// ---------------------------------------------------------------------------
// yt-dlp detection (mpv needs it for YouTube watch URLs)
// ---------------------------------------------------------------------------

bool ytDlpAvailable() {
    static bool cached = [] {
        return !findInPath("yt-dlp").empty() || !findInPath("youtube-dl").empty();
    }();
    return cached;
}

static bool hostIs(const std::string& host, const char* domain) {
    if (host == domain) return true;
    std::string dot = std::string(".") + domain;
    return host.size() > dot.size() &&
           host.compare(host.size() - dot.size(), dot.size(), dot) == 0;
}

bool needsYtDlp(const std::string& url) {
    Url u = parseUrl(url);
    if (!u.valid) return false;
    return hostIs(u.host, "youtube.com") || hostIs(u.host, "youtu.be") ||
           hostIs(u.host, "youtube-nocookie.com");
}

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

static std::atomic<pid_t> g_lastPid{0};

bool play(const std::string& url) {
    if (url.empty()) return false;
    std::string exe = path();                    // absolute, cached
    if (exe.empty()) return false;

    pid_t pid = ::fork();
    if (pid < 0) return false;              // fork failed: report to caller
    if (pid == 0) {
        // Child. Detach from the browser's process group and session so
        // closing the browser never kills playback mid-video.
        ::setsid();
        // Route mpv's stderr/stdout into a diagnostic log instead of
        // /dev/null: when mpv dies on a URL the browser can show WHY
        // (missing yt-dlp, stale extractor, codec problems, ...).
        const std::string lp = logPath();
        int fd = ::open(lp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) ::close(fd);
            ::dprintf(STDERR_FILENO, "[mini-browser] mpv %s\n", url.c_str());
        }
        if (!::freopen("/dev/null", "r", stdin)) { /* ignore */ }
        // --no-terminal: no status-line chatter, but errors and warnings
        // still reach stderr (-> the log). yt-dlp (if installed) is used
        // by mpv itself to resolve YouTube watch URLs into streams.
        ::execl(exe.c_str(), "mpv", "--no-terminal", url.c_str(),
                (char*)nullptr);
        // exec failed — leave a trace, then die.
        ::dprintf(STDERR_FILENO, "[mini-browser] exec failed (errno=%d)\n",
                  errno);
        ::_exit(127);
    }

    // Parent: remember the pid for alive(), auto-reap children so long
    // browsing sessions don't pile up zombie mpv processes. The browser
    // never wait()s for anything else, so making SIGCHLD "ignore"
    // (kernel reaps) is safe here.
    g_lastPid.store(pid);
    struct sigaction sa {};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGCHLD, &sa, nullptr);
    return true;
}

bool alive() {
    pid_t pid = g_lastPid.load();
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0;
}

// ---------------------------------------------------------------------------
// Media URL heuristic
// ---------------------------------------------------------------------------

static bool endsWithCi(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = (char)std::tolower((unsigned char)s[s.size() - n + i]);
        if (a != suffix[i]) return false;
    }
    return true;
}

bool isMediaFile(const std::string& url) {
    // Strip query/fragment: "video.mp4?t=30" is still a video.
    std::string u = url;
    size_t cut = u.find_first_of("?#");
    if (cut != std::string::npos) u.resize(cut);
    static const char* kExt[] = {
        ".mp4", ".mkv", ".webm", ".m3u8", ".mpd", ".avi", ".mov", ".ts",
        ".ogv", ".flv", ".mp3", ".m4a", ".flac", ".wav", ".opus", ".ogg",
    };
    for (const char* e : kExt)
        if (endsWithCi(u, e)) return true;
    return false;
}

} // namespace mpv
} // namespace browser
