// Internal media playback engine (implementation). See mediaplayer.h for
// the architecture overview.
//
// Decode pipeline per player, all on the player's worker thread:
//
//   avformat_open_input (local file, or curl temp file for https)
//     -> av_read_frame
//          -> video: avcodec -> sws_scale(YUV->RGBA) -> publish frame
//                    (paced against the wall clock so a 15 fps clip
//                     costs 15 publishes per second, not 500 decodes)
//          -> audio: avcodec -> swr(stereo s16) -> PCM ring -> SDL callback
//
// The UI thread never blocks: it reads atomics, copies the latest frame
// under a short-lived mutex, and talks to SDL audio through the callback.

#include "mediaplayer.h"

#include <SDL2/SDL.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>

// Optional FFmpeg: when the av* development headers are absent the whole
// engine degrades to a stub (players fail to open with a clear error,
// widgets show a "player unavailable" stage, the rest of the browser is
// unaffected — mpv remains the playback path there).
#if defined(__has_include)
#  if __has_include(<libavcodec/avcodec.h>)
#    define MB_HAVE_FFMPEG 1
#  endif
#endif

#ifdef MB_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <curl/curl.h>

#endif // MB_HAVE_FFMPEG

namespace browser {
namespace media {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[maybe_unused]] static double nowTicksSec() {
    return (double)SDL_GetTicks() / 1000.0;
}

static bool endsWithCi(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = (char)std::tolower((unsigned char)s[s.size() - n + i]);
        if (a != suffix[i]) return false;
    }
    return true;
}

// Extension heuristic shared with the click/navigate path. Local files
// without a known extension are left alone — the player will still try
// to open anything it is handed.
bool isMediaUrl(const std::string& url) {
    std::string u = url;
    size_t cut = u.find_first_of("?#");
    if (cut != std::string::npos) u.resize(cut);
    static const char* kExt[] = {
        ".mp4", ".m4v", ".mkv", ".webm", ".m3u8", ".mpd", ".avi", ".mov",
        ".ts", ".ogv", ".flv", ".wmv", ".mp3", ".m4a", ".aac", ".flac",
        ".wav", ".opus", ".ogg", ".oga",
    };
    for (const char* e : kExt)
        if (endsWithCi(u, e)) return true;
    return false;
}

// Global frame counter (bumped by every player publish) so the scheduler
// can detect "a new frame arrived anywhere" with one atomic read.
static std::atomic<uint64_t> g_framesPublished{0};
[[maybe_unused]] static void noteFramePublished() { g_framesPublished.fetch_add(1); }
bool anyNewFrames(uint64_t& seenSeq) {
    uint64_t cur = g_framesPublished.load();
    if (cur == seenSeq) return false;
    seenSeq = cur;
    return true;
}

// ---------------------------------------------------------------------------
// Remote https materialization: our minimal FFmpeg build has no TLS, so
// https:// media is streamed to a temp file by libcurl (which the browser
// already links) on the worker thread. http:// goes straight into ffmpeg.
// ---------------------------------------------------------------------------

#ifdef MB_HAVE_FFMPEG

static std::string urlHashName(const std::string& url) {
    // FNV-1a 64 -> 16 hex chars. Not cryptographic; just a cache key.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
    char buf[32];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

// Returns a local file path when the URL must be downloaded first ("" when
// ffmpeg can open it directly). Reuses a previous download when it is
// fresh enough (same URL, exists, non-empty, downloaded this session).
static std::string tempPathForRemote(const std::string& url) {
    const char* tmp = getenv("TMPDIR");
    std::string dir = std::string(tmp && tmp[0] ? tmp : "/tmp") +
                      "/mini-browser-media";
    return dir + "/" + urlHashName(url) + ".bin";
}

static bool downloadToFile(const std::string& url, const std::string& path,
                           std::string* err) {
    // Ensure the cache directory exists — nothing else ever creates it,
    // and fopen() will not make parent directories for us. (v2.4 bug:
    // remote-https playback always failed with "cannot create temp file"
    // because $TMPDIR/mini-browser-media was never mkdir'ed; local-file
    // tests could not catch it.)
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            if (err) *err = "cannot create cache dir " + dir + ": " +
                            std::strerror(errno);
            // Fall through: fopen gives the definitive verdict anyway.
        }
    }
    CURL* c = curl_easy_init();
    if (!c) { if (err) *err = "curl init failed"; return false; }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        curl_easy_cleanup(c);
        if (err) *err = std::string("cannot create temp file: ") +
                        std::strerror(errno);
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, (void*)f);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "MiniBrowser/2.5");
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)2ull * 1024 * 1024 * 1024);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    bool ok = (rc == CURLE_OK) && (code == 0 || (code >= 200 && code < 300));
    std::fclose(f);
    if (!ok) {
        std::remove(path.c_str());
        if (err) *err = "download failed (curl " + std::to_string((int)rc) +
                        ", http " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

#endif // MB_HAVE_FFMPEG (https materialization)

// ---------------------------------------------------------------------------
// SDL audio plumbing
// ---------------------------------------------------------------------------

#ifdef MB_HAVE_FFMPEG

void audioCallbackC(void* userdata, unsigned char* stream, int len);

// One-time SDL audio subsystem init (headless SDL builds may have audio
// compiled out — SDL_Init(SDL_INIT_AUDIO) then fails and we play silent).
static bool ensureAudioSubsys() {
    static bool inited = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
    return inited;
}

MediaPlayer::MediaPlayer() = default;

MediaPlayer::~MediaPlayer() { close(); }

bool MediaPlayer::open(const std::string& url, std::string* err) {
    if (url.empty()) { if (err) *err = "empty url"; return false; }
    if (state_ == State::Loading || state_ == State::Ok) return true; // already open/opening
    url_ = url;
    error_.clear();
    ended_ = false;
    playing_ = false;
    state_ = State::Loading;
    worker_ = std::thread([this]() { decodeLoop_(); });
    return true;
}

void MediaPlayer::close() {
    quit_ = true;
    playing_ = false;
    playWhenReady_ = false;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lk(audioM_);
    if (audioDev_) {
        SDL_CloseAudioDevice(audioDev_);
        audioDev_ = 0;
    }
    pcm_.clear();
    pcmPos_ = 0;
    state_ = State::Empty;
}

void MediaPlayer::play() {
    if (state_ != State::Ok) {
        // Autoplay races the async open (remote download, demux probing):
        // latch the request so the worker starts playback the moment the
        // stream is ready instead of dropping it.
        if (state_ == State::Loading) playWhenReady_ = true;
        return;
    }
    if (ended_) {           // replay from the top, like every real player
        seek(0.0);
        ended_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(clockM_);
        if (!clockRunning_) {
            clockBaseTicks_ = nowTicksSec();
            clockRunning_ = true;
        }
    }
    // Drop stale queued PCM so unpausing doesn't flush old audio.
    {
        std::lock_guard<std::mutex> lk(audioM_);
        pcm_.clear();
        pcmPos_ = 0;
    }
    playing_ = true;
    if (audioDev_) SDL_PauseAudioDevice(audioDev_, 0);
}

void MediaPlayer::pause() {
    playWhenReady_ = false;
    if (state_ != State::Ok) return;
    {
        std::lock_guard<std::mutex> lk(clockM_);
        if (clockRunning_) {
            clockBaseSec_ += nowTicksSec() - clockBaseTicks_;
            clockRunning_ = false;
        }
    }
    playing_ = false;
    if (audioDev_) SDL_PauseAudioDevice(audioDev_, 1);
    std::lock_guard<std::mutex> lk(audioM_);
    pcm_.clear();
    pcmPos_ = 0;
}

double MediaPlayer::position() const {
    std::lock_guard<std::mutex> lk(clockM_);
    double p = clockBaseSec_;
    if (clockRunning_) p += nowTicksSec() - clockBaseTicks_;
    double dur = duration_.load();
    if (dur > 0 && p > dur) p = dur;
    return p;
}

void MediaPlayer::resetClock_(double pos) {
    std::lock_guard<std::mutex> lk(clockM_);
    clockBaseSec_ = pos;
    clockBaseTicks_ = nowTicksSec();
    clockRunning_ = playing_.load();
}

// Wall-clock master clock: base position + elapsed ticks while running.
// Audio PCM is queued best-effort on top of this (no audio-clock sync in
// v1; drift is small for typical clips and the headless build has no
// audio device at all).
double MediaPlayer::wallClock_() const {
    std::lock_guard<std::mutex> lk(clockM_);
    double p = clockBaseSec_;
    if (clockRunning_) p += nowTicksSec() - clockBaseTicks_;
    return p;
}

void MediaPlayer::seek(double sec) {
    if (state_ != State::Ok) return;
    double dur = duration_.load();
    if (dur > 0) sec = std::min(std::max(0.0, sec), std::max(0.0, dur - 0.05));
    seekTarget_ = sec < 0 ? 0 : sec;
    seekFlag_ = true;
    ended_ = false;
    {
        std::lock_guard<std::mutex> lk(audioM_);
        pcm_.clear();
        pcmPos_ = 0;
    }
    resetClock_(sec);
}

bool MediaPlayer::copyFrame(MediaFrame& out) const {
    std::lock_guard<std::mutex> lk(frameM_);
    if (frame_.rgba.empty()) return false;
    out = frame_;
    return true;
}

bool MediaPlayer::firstFrameReady() const {
    return previewFrameDone_.load() || ended_.load() ||
           failed() || state_ == State::Ok;
}
bool MediaPlayer::serviceLoopRestart() { return false; } // loop lives in worker_()

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void MediaPlayer::decodeLoop_() {
    avformat_network_init();

    // ---- acquire something ffmpeg can open --------------------------------
    std::string openPath = url_;
    if (openPath.compare(0, 7, "file://") == 0) openPath = openPath.substr(7);
    bool remote = openPath.compare(0, 7, "http://") == 0 ||
                  openPath.compare(0, 8, "https://") == 0;
    if (remote && openPath.compare(0, 8, "https://") == 0) {
        // No TLS inside the minimal ffmpeg: stream to a temp file first.
        std::string tmp = tempPathForRemote(url_);
        std::string derr;
        if (!downloadToFile(openPath, tmp, &derr)) {
            error_ = derr;
            state_ = State::Failed;
            previewFrameDone_ = true;
            return;
        }
        openPath = tmp;
    }

    // ---- demuxer ----------------------------------------------------------
    fmt_ = avformat_alloc_context();
    if (avformat_open_input(&fmt_, openPath.c_str(), nullptr, nullptr) != 0) {
        error_ = "cannot open media: " + url_;
        state_ = State::Failed;
        previewFrameDone_ = true;
        return;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        error_ = "cannot read stream info";
        avformat_close_input(&fmt_);
        state_ = State::Failed;
        previewFrameDone_ = true;
        return;
    }
    duration_ = fmt_->duration > 0 ? fmt_->duration / (double)AV_TIME_BASE : 0.0;

    vStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    aStream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vStream_ >= 0) {
        AVStream* st = fmt_->streams[vStream_];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (dec) {
            vctx_ = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(vctx_, st->codecpar);
            if (avcodec_open2(vctx_, dec, nullptr) == 0) {
                hasVideo_ = true;
                videoW_ = vctx_->width;
                videoH_ = vctx_->height;
                sws_ = sws_getContext(vctx_->width, vctx_->height,
                                      vctx_->pix_fmt,
                                      vctx_->width, vctx_->height,
                                      AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                      nullptr, nullptr, nullptr);
            }
        }
    }
    if (aStream_ >= 0) {
        AVStream* st = fmt_->streams[aStream_];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (dec) {
            actx_ = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(actx_, st->codecpar);
            if (avcodec_open2(actx_, dec, nullptr) == 0) hasAudio_ = true;
        }
    }
    if (!hasVideo_ && !hasAudio_) {
        error_ = "no decodable audio/video stream";
        state_ = State::Failed;
        previewFrameDone_ = true;
        return;
    }

    // ---- audio device -----------------------------------------------------
    if (hasAudio_ && ensureAudioSubsys()) {
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 2048;
        want.callback = audioCallbackC;
        want.userdata = this;
        audioDev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audioDev_) {
            audioRate_ = have.freq;
            SDL_PauseAudioDevice(audioDev_, 1);   // start paused
        }
    }

    // swr for audio -> stereo s16 at the device rate.
    if (hasAudio_) {
        AVChannelLayout outCh = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&swr_, &outCh, AV_SAMPLE_FMT_S16,
                            audioRate_ > 0 ? audioRate_ : 48000,
                            &actx_->ch_layout, actx_->sample_fmt,
                            actx_->sample_rate, 0, nullptr);
        if (swr_ && swr_init(swr_) < 0) { swr_free(&swr_); swr_ = nullptr; }
    }

    state_ = State::Ok;
    resetClock_(0.0);

    // Preview: decode exactly one frame so a paused <video controls>
    // shows its poster frame immediately (Chrome shows the first frame
    // too, unless preload=none).
    if (hasVideo_ && !playing_.load()) {
        // Stop as soon as ONE frame is in hand: decoding ahead to EOF
        // here both wasted CPU and (worse) tripped ended_ before the user
        // ever pressed play, parking the decoder while autoplay latched.
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frm = av_frame_alloc();
        int guard = 0;
        while (!quit_ && frameSeq_.load() == 0 && guard++ < 256) {
            int r = av_read_frame(fmt_, pkt);
            if (r < 0) { ended_ = true; break; }
            if (pkt->stream_index == vStream_) {
                decodeVideoPacket_(vctx_, pkt);
                if (frameSeq_.load() > 0) break;   // preview frame ready
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        av_frame_free(&frm);
    }
    previewFrameDone_ = true;

    // Latched autoplay (play() arrived while Loading): start now.
    // ORDER MATTERS: playing_ must be true before resetClock_ so the
    // clock basis is captured in the "running" state.
    if (playWhenReady_.exchange(false) && !quit_.load()) {
        playing_ = true;
        resetClock_(0.0);
        if (audioDev_) SDL_PauseAudioDevice(audioDev_, 0);
    }

    // ---- main decode loop --------------------------------------------------
    AVPacket* pkt = av_packet_alloc();
    while (!quit_) {
        if (state_ != State::Ok) break;
        if (seekFlag_.exchange(false)) {
            double t = seekTarget_.load();
            av_seek_frame(fmt_, -1, (int64_t)(t * AV_TIME_BASE),
                          AVSEEK_FLAG_BACKWARD);
            if (vctx_) avcodec_flush_buffers(vctx_);
            if (actx_) avcodec_flush_buffers(actx_);
            ended_ = false;
            continue;
        }
        if (ended_.load()) { SDL_Delay(40); continue; }   // parked at end
        if (!playing_.load()) { SDL_Delay(30); continue; }

        int r = av_read_frame(fmt_, pkt);
        if (r < 0) {
            if (r == AVERROR_EOF || avio_feof(fmt_->pb)) {
                // Flush the decoders so trailing frames surface, then
                // either restart (loop) or park at the end. A looped
                // stream that was playing stays playing; one that was
                // only previewing (never played) must NOT autoplay.
                if (hasVideo_) decodeVideoPacket_(vctx_, nullptr);
                if (hasAudio_) decodeAudioPacket_(actx_, nullptr);
                bool wasPlaying = playing_.load();
                if (loop_ && wasPlaying) {
                    av_seek_frame(fmt_, -1, 0, AVSEEK_FLAG_BACKWARD);
                    if (vctx_) avcodec_flush_buffers(vctx_);
                    if (actx_) avcodec_flush_buffers(actx_);
                    resetClock_(0.0);
                    continue;
                }
                ended_ = true;
                playing_ = false;
                resetClock_(duration_.load());
            }
            SDL_Delay(10);
            continue;
        }
        if (pkt->stream_index == vStream_ && vctx_)
            decodeVideoPacket_(vctx_, pkt);
        else if (pkt->stream_index == aStream_ && actx_)
            decodeAudioPacket_(actx_, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // ---- teardown (worker side) -------------------------------------------
    if (audioDev_) SDL_PauseAudioDevice(audioDev_, 1);
    if (sws_)   { sws_freeContext(sws_); sws_ = nullptr; }
    if (swr_)   { swr_free(&swr_); swr_ = nullptr; }
    if (vctx_)  { avcodec_free_context(&vctx_); vctx_ = nullptr; }
    if (actx_)  { avcodec_free_context(&actx_); actx_ = nullptr; }
    if (fmt_)   { avformat_close_input(&fmt_); fmt_ = nullptr; }
}

void MediaPlayer::decodeVideoPacket_(AVCodecContext* ctx, AVPacket* pkt) {
    if (avcodec_send_packet(ctx, pkt) < 0) return;
    AVFrame* frm = av_frame_alloc();
    while (avcodec_receive_frame(ctx, frm) >= 0) {
        double pts = frm->pts * av_q2d(fmt_->streams[vStream_]->time_base);

        // Paused (preview done, or pausing mid-play): keep at most the
        // preview frame, never keep decoding ahead.
        if (!playing_.load() &&
            (previewFrameDone_.load() || frameSeq_.load() > 0)) {
            av_frame_unref(frm);
            break;
        }
        if (previewFrameDone_.load() && playing_.load()) {
            // Pace against the wall clock: publishing early just burns
            // CPU overwriting frames nobody displays yet.
            while (!quit_ && playing_.load() && !seekFlag_.load() &&
                   wallClock_() < pts - 0.004)
                SDL_Delay(4);
        }
        if (quit_) { av_frame_unref(frm); break; }

        int w = frm->width, h = frm->height;
        rgbaScratch_.resize((size_t)w * h * 4);
        uint8_t* dst[4] = { rgbaScratch_.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { w * 4, 0, 0, 0 };
        if (sws_) {
            sws_scale(sws_, frm->data, frm->linesize, 0, h, dst, dstStride);
            {
                std::lock_guard<std::mutex> lk(frameM_);
                frame_.w = w;
                frame_.h = h;
                frame_.pts = pts;
                frame_.rgba = rgbaScratch_;
                frame_.seq = frameSeq_.load() + 1;
                frameSeq_.store(frame_.seq);
            }
            noteFramePublished();
        }
        av_frame_unref(frm);
        if (!playing_.load()) break;   // published preview; stop here
    }
    av_frame_free(&frm);
}

void MediaPlayer::decodeAudioPacket_(AVCodecContext* ctx, AVPacket* pkt) {
    if (avcodec_send_packet(ctx, pkt) < 0) return;
    AVFrame* frm = av_frame_alloc();
    while (avcodec_receive_frame(ctx, frm) >= 0) {
        if (!swr_) { av_frame_unref(frm); continue; }
        int outRate = audioRate_ > 0 ? audioRate_ : 48000;
        int outMax = (int)((int64_t)frm->nb_samples * outRate / actx_->sample_rate) + 64;
        std::vector<uint8_t> out((size_t)outMax * 2 * 2);   // stereo * s16
        uint8_t* outPtr = out.data();
        int got = swr_convert(swr_, &outPtr, outMax,
                              (const uint8_t**)frm->extended_data,
                              frm->nb_samples);
        if (got > 0) pushPcm_(out.data(), got * 2 * 2);
        av_frame_unref(frm);
    }
    av_frame_free(&frm);
}

void MediaPlayer::pushPcm_(const uint8_t* data, int bytes) {
    std::lock_guard<std::mutex> lk(audioM_);
    if (!playing_.load()) return;
    // Cap the queue at ~1.5 s; drop the oldest when it overflows (the
    // audio clock is not our master clock, so losing old samples beats
    // growing RAM forever on a stalled sink).
    size_t cap = (size_t)(audioRate_ > 0 ? audioRate_ : 48000) * 2 * 2;
    if (pcm_.size() - pcmPos_ + (size_t)bytes > cap) {
        size_t excess = (size_t)bytes - (pcm_.size() - pcmPos_);
        pcmPos_ += (excess / 4) * 4;   // keep 4-byte (frame) alignment
    }
    pcm_.insert(pcm_.end(), data, data + bytes);
}

// SDL callback thread: drain the PCM ring, scale by volume, silence-pad.
void audioCallbackC(void* userdata, unsigned char* stream, int len) {
    auto* p = static_cast<MediaPlayer*>(userdata);
    std::lock_guard<std::mutex> lk(p->audioM_);
    size_t avail = p->pcm_.size() - p->pcmPos_;
    size_t want = (size_t)len;
    size_t take = want < avail ? want : avail;
    take -= take % 4;
    if (p->muted_.load()) {
        std::memset(stream, 0, (size_t)len);
        p->pcmPos_ += take;
    } else {
        std::memcpy(stream, p->pcm_.data() + p->pcmPos_, take);
        float vol = p->volume_.load();
        if (vol < 0.999f) {
            int16_t* s = (int16_t*)stream;
            for (size_t i = 0; i < take / 2; ++i)
                s[i] = (int16_t)(s[i] * vol);
        }
        if (take < want) std::memset(stream + take, 0, want - take);
        p->pcmPos_ += take;
    }
    // Compact the ring when the head is stale (amortized O(1)).
    if (p->pcmPos_ > (1u << 20)) {
        p->pcm_.erase(p->pcm_.begin(), p->pcm_.begin() + (long)p->pcmPos_);
        p->pcmPos_ = 0;
    }
}

#else // !MB_HAVE_FFMPEG — stub engine: no playback, clear error string

MediaPlayer::MediaPlayer() = default;
MediaPlayer::~MediaPlayer() = default;

bool MediaPlayer::open(const std::string& url, std::string* err) {
    (void)url;
    url_ = url;
    error_ = "player unavailable (built without FFmpeg)";
    state_ = State::Failed;
    previewFrameDone_ = true;
    if (err) *err = error_;
    return false;
}
void MediaPlayer::close() {}
void MediaPlayer::play() {}
void MediaPlayer::pause() {}
double MediaPlayer::position() const { return 0.0; }
void MediaPlayer::seek(double) {}
bool MediaPlayer::copyFrame(MediaFrame&) const { return false; }
bool MediaPlayer::firstFrameReady() const { return false; }
bool MediaPlayer::serviceLoopRestart() { return false; }
void MediaPlayer::decodeLoop_() {}
void MediaPlayer::decodeAudioPacket_(AVCodecContext*, AVPacket*) {}
void MediaPlayer::decodeVideoPacket_(AVCodecContext*, AVPacket*) {}
void MediaPlayer::pushPcm_(const uint8_t*, int) {}
double MediaPlayer::wallClock_() const { return 0.0; }
void MediaPlayer::resetClock_(double) {}

#endif // MB_HAVE_FFMPEG

// ---------------------------------------------------------------------------
// Player registry
// ---------------------------------------------------------------------------

namespace {
std::mutex g_regM;
std::unordered_map<std::string, std::shared_ptr<MediaPlayer>> g_players;
}

std::shared_ptr<MediaPlayer> acquirePlayer(const std::string& url) {
    std::lock_guard<std::mutex> lk(g_regM);
    auto it = g_players.find(url);
    if (it != g_players.end()) return it->second;
    auto p = std::make_shared<MediaPlayer>();
    p->open(url);
    g_players[url] = p;
    return p;
}

std::shared_ptr<MediaPlayer> findPlayer(const std::string& url) {
    std::lock_guard<std::mutex> lk(g_regM);
    auto it = g_players.find(url);
    return it == g_players.end() ? nullptr : it->second;
}

void prunePlayersExcept(const std::unordered_set<std::string>& keep) {
    std::lock_guard<std::mutex> lk(g_regM);
    for (auto it = g_players.begin(); it != g_players.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        it->second->pause();
        it->second->close();
        it = g_players.erase(it);
    }
}

void stopAllPlayers() {
    std::lock_guard<std::mutex> lk(g_regM);
    for (auto& kv : g_players) {
        kv.second->pause();
        kv.second->close();
    }
    g_players.clear();
}

int playerCount() {
    std::lock_guard<std::mutex> lk(g_regM);
    return (int)g_players.size();
}

bool anyPlaying() {
    std::lock_guard<std::mutex> lk(g_regM);
    for (auto& kv : g_players)
        if (kv.second->playing()) return true;
    return false;
}

int waitForFirstFrames(int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    for (;;) {
        size_t pending = 0;
        {
            std::lock_guard<std::mutex> lk(g_regM);
            for (auto& kv : g_players)
                if (!kv.second->firstFrameDecided()) ++pending;
        }
        if (pending == 0) return 0;
        if (std::chrono::steady_clock::now() >= deadline) return (int)pending;
        SDL_Delay(10);
    }
}

} // namespace media
} // namespace browser
