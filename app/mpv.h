#pragma once
#include <string>

namespace browser {
namespace mpv {

// True when an `mpv` binary is on PATH. Cached after the first call.
// mpv (with yt-dlp installed) plays YouTube watch URLs, direct media
// files and streams — this is how the browser "plays video" without
// carrying a video stack: spawn mpv, keep the web view for comments,
// description and related links.
bool available();

// Absolute path of the mpv binary ("" when not available). Cached.
// The launcher execs this full path so playback cannot break because of
// a stripped or odd PATH inside the detached child process.
std::string path();

// Launch mpv for `url` without blocking the UI. Returns false only if
// the fork itself failed. mpv's stderr/stdout go to logPath() (truncated
// on every launch) so failures are diagnosable; the browser probes
// alive() ~2 s later and surfaces logTail() in the status bar when mpv
// died instantly.
bool play(const std::string& url);

// Is the mpv process from the most recent play() still running? The
// kernel auto-reaps our children (SIGCHLD is SIG_IGN), so a dead mpv
// reliably reports "no such process".
bool alive();

// Last `maxLines` meaningful lines of the mpv log, joined with " | "
// ("" when the log is empty or missing).
std::string logTail(int maxLines = 3);

// Path of the diagnostic log, truncated on every launch.
std::string logPath();

// True when either yt-dlp or youtube-dl is on PATH. mpv's ytdl_hook
// script needs one of them to resolve YouTube watch URLs into streams;
// without it mpv exits within a second with an opaque error.
bool ytDlpAvailable();

// URLs that mpv can only play through a yt-dlp-family extractor
// (youtube.com, m./music. subdomains, youtu.be short links).
bool needsYtDlp(const std::string& url);

// Heuristic media-URL test used at click time: if a link points at an
// obvious media file we hand it to mpv instead of downloading the whole
// file with curl only to print "Unsupported content type".
bool isMediaFile(const std::string& url);

} // namespace mpv
} // namespace browser
