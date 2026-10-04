// Internal media playback engine for <video> / <audio>.
//
// The old pipeline handed every media URL to an external mpv process.
// This module gives the browser its own media stack — the way Firefox and
// Chrome decode video in-process — built on FFmpeg (demux + decode) and
// SDL (textures + audio output):
//
//   demux (avformat)  ->  decode (avcodec)  ->  sws_scale to RGBA
//        |                                        -> SDL_UpdateTexture (renderer)
//        ->  decode -> swr to stereo s16  ->  SDL audio callback
//
// Threading model: one worker thread per player runs the demux/decode
// loop. The UI thread only ever takes the frame mutex to copy the latest
// RGBA frame out, and reads atomics for state (playing/paused/position).
// Audio is consumed by SDL's callback thread from a mutex-guarded PCM
// ring. No locks are held while calling into SDL render functions.
//
// Headless builds (SDL built --disable-audio, dummy video driver) work
// too: SDL_INIT_AUDIO failure merely disables sound, and video timing
// falls back to the wall clock.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward-declare the FFmpeg C types in the GLOBAL namespace (the
// header's member declarations must resolve to ::AVCodecContext etc.,
// not to shadow types injected into browser::media). Complete
// definitions only exist in the FFmpeg-backed build.
struct AVCodecContext;
struct AVFormatContext;
struct AVPacket;
struct SwrContext;
struct SwsContext;

namespace browser {
namespace media {

// Defined when the internal playback engine has real decoders (FFmpeg
// present at compile time). Tests and UI code use this to skip playback
// paths in stub builds; the widgets then show "player unavailable".
#if defined(__has_include)
#  if __has_include(<libavcodec/avcodec.h>)
#    ifndef MB_MEDIA_ENABLED
#      define MB_MEDIA_ENABLED 1
#    endif
#  endif
#endif

// One decoded video frame, converted to flat RGBA (row 0 first, stride =
// width * 4). Copied out under the player's frame mutex.
struct MediaFrame {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    double pts = 0.0;      // presentation time (seconds into the stream)
    uint64_t seq = 0;      // bumped for every new frame
};

// A single media element's playback engine. Owns the demuxer, decoders,
// worker thread and (when available) the SDL audio device.
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();

    // Open `url` (local path, file://, http/https URL) and start the
    // worker thread. The thread is paused-on-first-frame: one frame is
    // decoded so the UI can preview it, then playback waits for play().
    // Non-blocking for remote URLs: the download (if TLS must be
    // tunneled through curl) happens on the worker thread; ok() turns
    // true once demuxing starts. `err` receives a human-readable reason
    // on failure.
    bool open(const std::string& url, std::string* err = nullptr);
    void close();

    // State (atomics / cheap).
    bool valid() const { return state_ == State::Ok; }
    bool failed() const { return state_ == State::Failed; }
    const std::string& url() const { return url_; }
    const std::string& error() const { return error_; }
    bool hasVideo() const { return hasVideo_; }
    bool hasAudio() const { return hasAudio_; }
    int  videoW() const { return videoW_; }
    int  videoH() const { return videoH_; }

    // Transport.
    void play();
    void pause();
    void toggle() { playing() ? pause() : play(); }
    bool playing() const { return playing_.load(); }
    bool paused()  const { return valid() && !playing_.load() && !ended(); }
    bool ended()   const { return ended_.load(); }
    void setLoop(bool l) { loop_ = l; }
    bool loop()    const { return loop_; }

    // Current position / total duration, seconds (0 when unknown).
    double position() const;
    double duration() const { return duration_.load(); }
    // Seek to `sec` (clamped). Safe from the UI thread.
    void seek(double sec);

    // Volume 0..1 and mute (applied in the SDL audio callback).
    void setVolume(float v) { volume_.store(v < 0.f ? 0.f : v > 1.f ? 1.f : v); }
    float volume() const { return volume_.load(); }
    void setMuted(bool m) { muted_.store(m); }
    bool muted() const { return muted_.load(); }

    // Latest frame access. copyFrame() copies the current frame (if any)
    // into `out` and returns true; seq lets callers skip redundant
    // texture uploads (only re-upload when seq changed).
    bool copyFrame(MediaFrame& out) const;
    uint64_t frameSeq() const { return frameSeq_.load(); }

    // True when the worker produced the preview frame (or reached EOF /
    // failed) — used by screenshot mode to wait for something to draw.
    bool firstFrameReady() const;

    // Stricter: an actual frame exists, or the stream proved to have no
    // (decodable) video. Used by waitForFirstFrames.
    bool firstFrameDecided() const {
        return previewFrameDone_.load() || ended_.load() || failed();
    }

    // Called by the app's tick/poll: if the decoder finished the media
    // and loop_ is set, restarts from 0. Returns true when a restart
    // happened (callers may repaint).
    bool serviceLoopRestart();

private:
    enum class State { Empty, Loading, Ok, Failed };
    void decodeLoop_();
    void decodeAudioPacket_(struct AVCodecContext* ctx, struct AVPacket* pkt);
    void decodeVideoPacket_(struct AVCodecContext* ctx, struct AVPacket* pkt);
    void pushPcm_(const uint8_t* data, int bytes);
    double wallClock_() const;   // playback position from the wall clock
    void resetClock_(double pos);

    std::string url_;
    std::string error_;
    std::atomic<State> state_{State::Empty};
    std::atomic<bool> quit_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> ended_{false};
    // play() called while still Loading (autoplay races the async open):
    // latched here and honored by the worker once demuxing is done.
    std::atomic<bool> playWhenReady_{false};
    std::atomic<bool> seekFlag_{false};
    std::atomic<double> seekTarget_{0.0};
    std::atomic<bool> loop_{false};

    // Stream facts (written by worker before state_->Ok, read by UI).
    std::atomic<bool>   hasVideo_{false}, hasAudio_{false};
    std::atomic<int>    videoW_{0}, videoH_{0};
    std::atomic<double> duration_{0.0};

    // Wall-clock playback basis, guarded by clockM_ (UI seeks vs worker).
    mutable std::mutex clockM_;
    double clockBaseSec_ = 0.0;      // position at clockBaseTicks_
    double clockBaseTicks_ = 0.0;    // SDL_GetTicks()/1000.0
    bool   clockRunning_ = false;    // ticking while playing

    // Latest decoded frame.
    mutable std::mutex frameM_;
    MediaFrame frame_;
    std::atomic<uint64_t> frameSeq_{0};
    std::atomic<bool> previewFrameDone_{false};

    // Audio output (created on the worker, torn down in close()).
    // v2.12: pcm_ is now std::vector<uint8_t> (bytes) instead of int16_t
    // because the audio device format can be either S16 or F32 — the
    // callback treats the bytes as the device's native format.
    std::mutex audioM_;
    std::vector<uint8_t> pcm_;      // interleaved stereo, format = audioFmt_
    size_t pcmPos_ = 0;
    unsigned int audioDev_ = 0;
    int audioRate_ = 0;
    int audioFmt_ = 0;             // SDL_AudioFormat (AUDIO_F32SYS etc.)
    int outBytesPerSample_ = 4;    // 4 for F32, 2 for S16 (set by swr setup)
    std::atomic<float> volume_{0.9f};
    std::atomic<bool> muted_{false};

    // FFmpeg handles live only on the worker thread.
    struct AVFormatContext* fmt_ = nullptr;
    struct AVCodecContext*  vctx_ = nullptr;
    struct AVCodecContext*  actx_ = nullptr;
    struct SwsContext*      sws_ = nullptr;
    struct SwrContext*      swr_ = nullptr;
    int vStream_ = -1, aStream_ = -1;
    std::vector<uint8_t> rgbaScratch_;   // worker-only staging buffer
    std::thread worker_;
    bool workerJoined_ = false;

    // SDL audio callback (free function, needs private access).
    friend void audioCallbackC(void* userdata, unsigned char* stream,
                               int len);
};

// ---------------------------------------------------------------------------
// Player registry: maps resolved media URLs to live players. Layout boxes
// only carry the URL string; the renderer/browser look the player up here.
// ---------------------------------------------------------------------------

// Get (or lazily create) the player for `url`. Creation starts the worker
// (paused preview state). Never blocks the calling thread.
std::shared_ptr<MediaPlayer> acquirePlayer(const std::string& url);

// Lookup without creating.
std::shared_ptr<MediaPlayer> findPlayer(const std::string& url);

// Drop players whose URL is not in `keep` (used on navigation: the new
// page replaces the old page's media). Players still referenced elsewhere
// (shared_ptr held by the caller) keep running; the registry entry dies.
void prunePlayersExcept(const std::unordered_set<std::string>& keep);

// Tear everything down (quit, join threads, close audio). Called at exit
// and on document swaps that should stop all sound.
void stopAllPlayers();

// Number of live players (tests / status line).
int playerCount();

// True when any player is actively playing (drives the repaint scheduler).
bool anyPlaying();

// True when any player produced a NEW frame since the caller last saw
// `seenSeq` (single global counter helper for the tick/paint loop).
bool anyNewFrames(uint64_t& seenSeq);

// Wait until every player has produced its preview frame (or `timeoutMs`
// elapsed). Screenshot mode uses this to deterministically capture the
// first video frame. Returns the number of players still without a frame.
int waitForFirstFrames(int timeoutMs);

// True when the URL looks like media the internal player handles
// (extension heuristic, shared with the mpv fallback path).
bool isMediaUrl(const std::string& url);

} // namespace media
} // namespace browser
