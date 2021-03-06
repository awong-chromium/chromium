// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "webkit/media/webmediaplayer_impl.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/callback.h"
#include "base/message_loop_proxy.h"
#include "base/metrics/histogram.h"
#include "base/string_number_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "media/audio/null_audio_sink.h"
#include "media/base/bind_to_loop.h"
#include "media/base/filter_collection.h"
#include "media/base/limits.h"
#include "media/base/media_log.h"
#include "media/base/pipeline.h"
#include "media/base/video_frame.h"
#include "media/filters/audio_renderer_impl.h"
#include "media/filters/chunk_demuxer.h"
#include "media/filters/video_renderer_base.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebRect.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebSize.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebString.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebURL.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebMediaSource.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebRuntimeFeatures.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebView.h"
#include "v8/include/v8.h"
#include "webkit/media/buffered_data_source.h"
#include "webkit/media/filter_helpers.h"
#include "webkit/media/webaudiosourceprovider_impl.h"
#include "webkit/media/webmediaplayer_delegate.h"
#include "webkit/media/webmediaplayer_params.h"
#include "webkit/media/webmediaplayer_util.h"
#include "webkit/media/webmediasourceclient_impl.h"
#include "webkit/media/webvideoframe_impl.h"
#include "webkit/plugins/ppapi/ppapi_webplugin_impl.h"

using WebKit::WebCanvas;
using WebKit::WebMediaPlayer;
using WebKit::WebRect;
using WebKit::WebSize;
using WebKit::WebString;
using media::PipelineStatus;

namespace {

// Amount of extra memory used by each player instance reported to V8.
// It is not exact number -- first, it differs on different platforms,
// and second, it is very hard to calculate. Instead, use some arbitrary
// value that will cause garbage collection from time to time. We don't want
// it to happen on every allocation, but don't want 5k players to sit in memory
// either. Looks that chosen constant achieves both goals, at least for audio
// objects. (Do not worry about video objects yet, JS programs do not create
// thousands of them...)
const int kPlayerExtraMemory = 1024 * 1024;

// Limits the range of playback rate.
//
// TODO(kylep): Revisit these.
//
// Vista has substantially lower performance than XP or Windows7.  If you speed
// up a video too much, it can't keep up, and rendering stops updating except on
// the time bar. For really high speeds, audio becomes a bottleneck and we just
// use up the data we have, which may not achieve the speed requested, but will
// not crash the tab.
//
// A very slow speed, ie 0.00000001x, causes the machine to lock up. (It seems
// like a busy loop). It gets unresponsive, although its not completely dead.
//
// Also our timers are not very accurate (especially for ogg), which becomes
// evident at low speeds and on Vista. Since other speeds are risky and outside
// the norms, we think 1/16x to 16x is a safe and useful range for now.
const float kMinRate = 0.0625f;
const float kMaxRate = 16.0f;

// Prefix for histograms related to Encrypted Media Extensions.
const char* kMediaEme = "Media.EME.";
}  // namespace

namespace webkit_media {

#define COMPILE_ASSERT_MATCHING_ENUM(name) \
  COMPILE_ASSERT(static_cast<int>(WebMediaPlayer::CORSMode ## name) == \
                 static_cast<int>(BufferedResourceLoader::k ## name), \
                 mismatching_enums)
COMPILE_ASSERT_MATCHING_ENUM(Unspecified);
COMPILE_ASSERT_MATCHING_ENUM(Anonymous);
COMPILE_ASSERT_MATCHING_ENUM(UseCredentials);
#undef COMPILE_ASSERT_MATCHING_ENUM

#define BIND_TO_RENDER_LOOP(function) \
  media::BindToLoop(main_loop_, base::Bind(function, AsWeakPtr()))

#define BIND_TO_RENDER_LOOP_1(function, arg1) \
  media::BindToLoop(main_loop_, base::Bind(function, AsWeakPtr(), arg1))

#define BIND_TO_RENDER_LOOP_2(function, arg1, arg2) \
  media::BindToLoop(main_loop_, base::Bind(function, AsWeakPtr(), arg1, arg2))

static WebKit::WebTimeRanges ConvertToWebTimeRanges(
    const media::Ranges<base::TimeDelta>& ranges) {
  WebKit::WebTimeRanges result(ranges.size());
  for (size_t i = 0; i < ranges.size(); i++) {
    result[i].start = ranges.start(i).InSecondsF();
    result[i].end = ranges.end(i).InSecondsF();
  }
  return result;
}

static void LogMediaSourceError(const scoped_refptr<media::MediaLog>& media_log,
                                const std::string& error) {
  media_log->AddEvent(media_log->CreateMediaSourceErrorEvent(error));
}

WebMediaPlayerImpl::WebMediaPlayerImpl(
    WebKit::WebFrame* frame,
    WebKit::WebMediaPlayerClient* client,
    base::WeakPtr<WebMediaPlayerDelegate> delegate,
    const WebMediaPlayerParams& params)
    : frame_(frame),
      network_state_(WebMediaPlayer::NetworkStateEmpty),
      ready_state_(WebMediaPlayer::ReadyStateHaveNothing),
      main_loop_(base::MessageLoopProxy::current()),
      filter_collection_(new media::FilterCollection()),
      media_thread_("MediaPipeline"),
      paused_(true),
      seeking_(false),
      playback_rate_(0.0f),
      pending_seek_(false),
      pending_seek_seconds_(0.0f),
      client_(client),
      delegate_(delegate),
      media_log_(params.media_log()),
      accelerated_compositing_reported_(false),
      incremented_externally_allocated_memory_(false),
      is_local_source_(false),
      supports_save_(true),
      starting_(false),
      pending_repaint_(false) {
  media_log_->AddEvent(
      media_log_->CreateEvent(media::MediaLogEvent::WEBMEDIAPLAYER_CREATED));

  CHECK(media_thread_.Start());
  pipeline_ = new media::Pipeline(
      media_thread_.message_loop_proxy(), media_log_);

  // Let V8 know we started new thread if we did not do it yet.
  // Made separate task to avoid deletion of player currently being created.
  // Also, delaying GC until after player starts gets rid of starting lag --
  // collection happens in parallel with playing.
  //
  // TODO(enal): remove when we get rid of per-audio-stream thread.
  main_loop_->PostTask(
      FROM_HERE,
      base::Bind(&WebMediaPlayerImpl::IncrementExternallyAllocatedMemory,
                 AsWeakPtr()));

  // Also we want to be notified of |main_loop_| destruction.
  MessageLoop::current()->AddDestructionObserver(this);

  media::SetDecryptorReadyCB set_decryptor_ready_cb;
  if (WebKit::WebRuntimeFeatures::isEncryptedMediaEnabled()) {
    decryptor_.reset(new ProxyDecryptor(
        client,
        frame,
        BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnKeyAdded),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnKeyError),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnKeyMessage),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnNeedKey)));
    set_decryptor_ready_cb = base::Bind(&ProxyDecryptor::SetDecryptorReadyCB,
                                        base::Unretained(decryptor_.get()));
  }

  // Create the GPU video decoder if factories were provided.
  if (params.gpu_factories()) {
    filter_collection_->GetVideoDecoders()->push_back(
        new media::GpuVideoDecoder(
            media_thread_.message_loop_proxy(),
            params.gpu_factories()));
  }

  // Create default video renderer.
  scoped_ptr<media::VideoRenderer> video_renderer(
      new media::VideoRendererBase(
          media_thread_.message_loop_proxy(),
          set_decryptor_ready_cb,
          base::Bind(&WebMediaPlayerImpl::FrameReady, base::Unretained(this)),
          BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::SetOpaque),
          true));
  filter_collection_->SetVideoRenderer(video_renderer.Pass());

  // Create default audio renderer using the null sink if no sink was provided.
  audio_source_provider_ = new WebAudioSourceProviderImpl(
      params.audio_renderer_sink() ? params.audio_renderer_sink() :
      new media::NullAudioSink());
  scoped_ptr<media::AudioRenderer> audio_renderer(
      new media::AudioRendererImpl(
        media_thread_.message_loop_proxy(),
        audio_source_provider_,
        set_decryptor_ready_cb));
  filter_collection_->SetAudioRenderer(audio_renderer.Pass());
}

WebMediaPlayerImpl::~WebMediaPlayerImpl() {
  DCHECK(main_loop_->BelongsToCurrentThread());
  Destroy();
  media_log_->AddEvent(
      media_log_->CreateEvent(media::MediaLogEvent::WEBMEDIAPLAYER_DESTROYED));

  if (delegate_)
    delegate_->PlayerGone(this);

  // Remove destruction observer if we're being destroyed but the main thread is
  // still running.
  if (MessageLoop::current())
    MessageLoop::current()->RemoveDestructionObserver(this);
}

namespace {

// Helper enum for reporting scheme histograms.
enum URLSchemeForHistogram {
  kUnknownURLScheme,
  kMissingURLScheme,
  kHttpURLScheme,
  kHttpsURLScheme,
  kFtpURLScheme,
  kChromeExtensionURLScheme,
  kJavascriptURLScheme,
  kFileURLScheme,
  kBlobURLScheme,
  kDataURLScheme,
  kFileSystemScheme,
  kMaxURLScheme = kFileSystemScheme  // Must be equal to highest enum value.
};

URLSchemeForHistogram URLScheme(const GURL& url) {
  if (!url.has_scheme()) return kMissingURLScheme;
  if (url.SchemeIs("http")) return kHttpURLScheme;
  if (url.SchemeIs("https")) return kHttpsURLScheme;
  if (url.SchemeIs("ftp")) return kFtpURLScheme;
  if (url.SchemeIs("chrome-extension")) return kChromeExtensionURLScheme;
  if (url.SchemeIs("javascript")) return kJavascriptURLScheme;
  if (url.SchemeIs("file")) return kFileURLScheme;
  if (url.SchemeIs("blob")) return kBlobURLScheme;
  if (url.SchemeIs("data")) return kDataURLScheme;
  if (url.SchemeIs("filesystem")) return kFileSystemScheme;
  return kUnknownURLScheme;
}

}  // anonymous namespace

void WebMediaPlayerImpl::load(const WebKit::WebURL& url, CORSMode cors_mode) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  LoadSetup(url);

  // Otherwise it's a regular request which requires resolving the URL first.
  GURL gurl(url);
  data_source_ = new BufferedDataSource(
      main_loop_, frame_, media_log_, base::Bind(
          &WebMediaPlayerImpl::NotifyDownloading, AsWeakPtr()));
  data_source_->Initialize(
      url, static_cast<BufferedResourceLoader::CORSMode>(cors_mode),
      base::Bind(
          &WebMediaPlayerImpl::DataSourceInitialized,
          AsWeakPtr(), gurl));

  is_local_source_ = !gurl.SchemeIs("http") && !gurl.SchemeIs("https");

  BuildDefaultCollection(data_source_,
                         media_thread_.message_loop_proxy(),
                         filter_collection_.get());
}

void WebMediaPlayerImpl::load(const WebKit::WebURL& url,
                              WebKit::WebMediaSource* media_source,
                              CORSMode cors_mode) {
  scoped_ptr<WebKit::WebMediaSource> ms(media_source);
  LoadSetup(url);

  // Media source pipelines can start immediately.
  chunk_demuxer_ = new media::ChunkDemuxer(
      BIND_TO_RENDER_LOOP_1(&WebMediaPlayerImpl::OnDemuxerOpened,
                            base::Passed(&ms)),
      BIND_TO_RENDER_LOOP_2(&WebMediaPlayerImpl::OnNeedKey, "", ""),
      base::Bind(&LogMediaSourceError, media_log_));

  BuildMediaSourceCollection(chunk_demuxer_,
                             media_thread_.message_loop_proxy(),
                             filter_collection_.get());
  supports_save_ = false;
  StartPipeline();
}

void WebMediaPlayerImpl::LoadSetup(const WebKit::WebURL& url) {
  GURL gurl(url);
  UMA_HISTOGRAM_ENUMERATION("Media.URLScheme", URLScheme(gurl), kMaxURLScheme);

  // Handle any volume/preload changes that occurred before load().
  setVolume(GetClient()->volume());
  setPreload(GetClient()->preload());

  SetNetworkState(WebMediaPlayer::NetworkStateLoading);
  SetReadyState(WebMediaPlayer::ReadyStateHaveNothing);
  media_log_->AddEvent(media_log_->CreateLoadEvent(url.spec()));
}

void WebMediaPlayerImpl::cancelLoad() {
  DCHECK(main_loop_->BelongsToCurrentThread());
}

void WebMediaPlayerImpl::play() {
  DCHECK(main_loop_->BelongsToCurrentThread());

  paused_ = false;
  pipeline_->SetPlaybackRate(playback_rate_);

  media_log_->AddEvent(media_log_->CreateEvent(media::MediaLogEvent::PLAY));

  if (delegate_)
    delegate_->DidPlay(this);
}

void WebMediaPlayerImpl::pause() {
  DCHECK(main_loop_->BelongsToCurrentThread());

  paused_ = true;
  pipeline_->SetPlaybackRate(0.0f);
  paused_time_ = pipeline_->GetMediaTime();

  media_log_->AddEvent(media_log_->CreateEvent(media::MediaLogEvent::PAUSE));

  if (delegate_)
    delegate_->DidPause(this);
}

bool WebMediaPlayerImpl::supportsFullscreen() const {
  DCHECK(main_loop_->BelongsToCurrentThread());
  return true;
}

bool WebMediaPlayerImpl::supportsSave() const {
  DCHECK(main_loop_->BelongsToCurrentThread());
  return supports_save_;
}

void WebMediaPlayerImpl::seek(float seconds) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  if (starting_ || seeking_) {
    pending_seek_ = true;
    pending_seek_seconds_ = seconds;
    if (chunk_demuxer_)
      chunk_demuxer_->CancelPendingSeek();
    return;
  }

  media_log_->AddEvent(media_log_->CreateSeekEvent(seconds));

  base::TimeDelta seek_time = ConvertSecondsToTimestamp(seconds);

  // Update our paused time.
  if (paused_)
    paused_time_ = seek_time;

  seeking_ = true;

  if (chunk_demuxer_)
    chunk_demuxer_->StartWaitingForSeek();

  // Kick off the asynchronous seek!
  pipeline_->Seek(
      seek_time,
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineSeek));
}

void WebMediaPlayerImpl::setEndTime(float seconds) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // TODO(hclam): add method call when it has been implemented.
  return;
}

void WebMediaPlayerImpl::setRate(float rate) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // TODO(kylep): Remove when support for negatives is added. Also, modify the
  // following checks so rewind uses reasonable values also.
  if (rate < 0.0f)
    return;

  // Limit rates to reasonable values by clamping.
  if (rate != 0.0f) {
    if (rate < kMinRate)
      rate = kMinRate;
    else if (rate > kMaxRate)
      rate = kMaxRate;
  }

  playback_rate_ = rate;
  if (!paused_) {
    pipeline_->SetPlaybackRate(rate);
  }
}

void WebMediaPlayerImpl::setVolume(float volume) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  pipeline_->SetVolume(volume);
}

void WebMediaPlayerImpl::setVisible(bool visible) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // TODO(hclam): add appropriate method call when pipeline has it implemented.
  return;
}

#define COMPILE_ASSERT_MATCHING_ENUM(webkit_name, chromium_name) \
    COMPILE_ASSERT(static_cast<int>(WebMediaPlayer::webkit_name) == \
                   static_cast<int>(webkit_media::chromium_name), \
                   mismatching_enums)
COMPILE_ASSERT_MATCHING_ENUM(PreloadNone, NONE);
COMPILE_ASSERT_MATCHING_ENUM(PreloadMetaData, METADATA);
COMPILE_ASSERT_MATCHING_ENUM(PreloadAuto, AUTO);
#undef COMPILE_ASSERT_MATCHING_ENUM

void WebMediaPlayerImpl::setPreload(WebMediaPlayer::Preload preload) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  if (data_source_)
    data_source_->SetPreload(static_cast<webkit_media::Preload>(preload));
}

bool WebMediaPlayerImpl::totalBytesKnown() {
  DCHECK(main_loop_->BelongsToCurrentThread());

  return pipeline_->GetTotalBytes() != 0;
}

bool WebMediaPlayerImpl::hasVideo() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  return pipeline_->HasVideo();
}

bool WebMediaPlayerImpl::hasAudio() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  return pipeline_->HasAudio();
}

WebKit::WebSize WebMediaPlayerImpl::naturalSize() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  gfx::Size size;
  pipeline_->GetNaturalVideoSize(&size);
  return WebKit::WebSize(size);
}

bool WebMediaPlayerImpl::paused() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  return pipeline_->GetPlaybackRate() == 0.0f;
}

bool WebMediaPlayerImpl::seeking() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing)
    return false;

  return seeking_;
}

float WebMediaPlayerImpl::duration() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing)
    return std::numeric_limits<float>::quiet_NaN();

  double duration;
  if (chunk_demuxer_) {
    duration = chunk_demuxer_->GetDuration();
  } else {
    duration = GetPipelineDuration();
  }

  // Make sure super small durations don't get truncated to 0 and
  // large durations don't get converted to infinity by the double -> float
  // conversion.
  //
  // TODO(acolwell): Remove when WebKit is changed to report duration as a
  // double.
  if (duration > 0.0 && duration < std::numeric_limits<double>::infinity()) {
    duration = std::max(duration,
                        static_cast<double>(std::numeric_limits<float>::min()));
    duration = std::min(duration,
                        static_cast<double>(std::numeric_limits<float>::max()));
  }

  return static_cast<float>(duration);
}

float WebMediaPlayerImpl::currentTime() const {
  DCHECK(main_loop_->BelongsToCurrentThread());
  if (paused_)
    return static_cast<float>(paused_time_.InSecondsF());
  return static_cast<float>(pipeline_->GetMediaTime().InSecondsF());
}

int WebMediaPlayerImpl::dataRate() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // TODO(hclam): Add this method call if pipeline has it in the interface.
  return 0;
}

WebMediaPlayer::NetworkState WebMediaPlayerImpl::networkState() const {
  DCHECK(main_loop_->BelongsToCurrentThread());
  return network_state_;
}

WebMediaPlayer::ReadyState WebMediaPlayerImpl::readyState() const {
  DCHECK(main_loop_->BelongsToCurrentThread());
  return ready_state_;
}

const WebKit::WebTimeRanges& WebMediaPlayerImpl::buffered() {
  DCHECK(main_loop_->BelongsToCurrentThread());
  WebKit::WebTimeRanges web_ranges(
      ConvertToWebTimeRanges(pipeline_->GetBufferedTimeRanges()));
  buffered_.swap(web_ranges);
  return buffered_;
}

float WebMediaPlayerImpl::maxTimeSeekable() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // If we haven't even gotten to ReadyStateHaveMetadata yet then just
  // return 0 so that the seekable range is empty.
  if (ready_state_ < WebMediaPlayer::ReadyStateHaveMetadata)
    return 0.0f;

  // We don't support seeking in streaming media.
  if (data_source_ && data_source_->IsStreaming())
    return 0.0f;
  return duration();
}

bool WebMediaPlayerImpl::didLoadingProgress() const {
  DCHECK(main_loop_->BelongsToCurrentThread());
  return pipeline_->DidLoadingProgress();
}

unsigned long long WebMediaPlayerImpl::totalBytes() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  return pipeline_->GetTotalBytes();
}

void WebMediaPlayerImpl::setSize(const WebSize& size) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // Don't need to do anything as we use the dimensions passed in via paint().
}

void WebMediaPlayerImpl::paint(WebCanvas* canvas,
                               const WebRect& rect,
                               uint8_t alpha) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  if (!accelerated_compositing_reported_) {
    accelerated_compositing_reported_ = true;
    // Normally paint() is only called in non-accelerated rendering, but there
    // are exceptions such as webgl where compositing is used in the WebView but
    // video frames are still rendered to a canvas.
    UMA_HISTOGRAM_BOOLEAN(
        "Media.AcceleratedCompositingActive",
        frame_->view()->isAcceleratedCompositingActive());
  }

  // Avoid locking and potentially blocking the video rendering thread while
  // painting in software.
  scoped_refptr<media::VideoFrame> video_frame;
  {
    base::AutoLock auto_lock(lock_);
    video_frame = current_frame_;
  }
  gfx::Rect gfx_rect(rect);
  skcanvas_video_renderer_.Paint(video_frame, canvas, gfx_rect, alpha);
}

bool WebMediaPlayerImpl::hasSingleSecurityOrigin() const {
  if (data_source_)
    return data_source_->HasSingleOrigin();
  return true;
}

bool WebMediaPlayerImpl::didPassCORSAccessCheck() const {
  if (data_source_)
    return data_source_->DidPassCORSAccessCheck();
  return false;
}

WebMediaPlayer::MovieLoadType WebMediaPlayerImpl::movieLoadType() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // Disable seeking while streaming.
  if (data_source_ && data_source_->IsStreaming())
    return WebMediaPlayer::MovieLoadTypeLiveStream;
  return WebMediaPlayer::MovieLoadTypeUnknown;
}

float WebMediaPlayerImpl::mediaTimeForTimeValue(float timeValue) const {
  return ConvertSecondsToTimestamp(timeValue).InSecondsF();
}

unsigned WebMediaPlayerImpl::decodedFrameCount() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  media::PipelineStatistics stats = pipeline_->GetStatistics();
  return stats.video_frames_decoded;
}

unsigned WebMediaPlayerImpl::droppedFrameCount() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  media::PipelineStatistics stats = pipeline_->GetStatistics();
  return stats.video_frames_dropped;
}

unsigned WebMediaPlayerImpl::audioDecodedByteCount() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  media::PipelineStatistics stats = pipeline_->GetStatistics();
  return stats.audio_bytes_decoded;
}

unsigned WebMediaPlayerImpl::videoDecodedByteCount() const {
  DCHECK(main_loop_->BelongsToCurrentThread());

  media::PipelineStatistics stats = pipeline_->GetStatistics();
  return stats.video_bytes_decoded;
}

WebKit::WebVideoFrame* WebMediaPlayerImpl::getCurrentFrame() {
  base::AutoLock auto_lock(lock_);
  if (current_frame_)
    return new WebVideoFrameImpl(current_frame_);
  return NULL;
}

void WebMediaPlayerImpl::putCurrentFrame(
    WebKit::WebVideoFrame* web_video_frame) {
  if (!accelerated_compositing_reported_) {
    accelerated_compositing_reported_ = true;
    DCHECK(frame_->view()->isAcceleratedCompositingActive());
    UMA_HISTOGRAM_BOOLEAN("Media.AcceleratedCompositingActive", true);
  }
  delete web_video_frame;
}

// Helper enum for reporting generateKeyRequest/addKey histograms.
enum MediaKeyException {
  kUnknownResultId,
  kSuccess,
  kKeySystemNotSupported,
  kInvalidPlayerState,
  kMaxMediaKeyException
};

static MediaKeyException MediaKeyExceptionForUMA(
    WebMediaPlayer::MediaKeyException e) {
  switch (e) {
    case WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported:
      return kKeySystemNotSupported;
    case WebMediaPlayer::MediaKeyExceptionInvalidPlayerState:
      return kInvalidPlayerState;
    case WebMediaPlayer::MediaKeyExceptionNoError:
      return kSuccess;
    default:
      return kUnknownResultId;
  }
}

// Helper for converting |key_system| name and exception |e| to a pair of enum
// values from above, for reporting to UMA.
static void ReportMediaKeyExceptionToUMA(
    const std::string& method,
    const WebString& key_system,
    WebMediaPlayer::MediaKeyException e) {
  MediaKeyException result_id = MediaKeyExceptionForUMA(e);
  DCHECK_NE(result_id, kUnknownResultId) << e;
  base::LinearHistogram::FactoryGet(
      kMediaEme + KeySystemNameForUMA(key_system) + "." + method, 1,
      kMaxMediaKeyException, kMaxMediaKeyException + 1,
      base::Histogram::kUmaTargetedHistogramFlag)->Add(result_id);
}

WebMediaPlayer::MediaKeyException
WebMediaPlayerImpl::generateKeyRequest(const WebString& key_system,
                                       const unsigned char* init_data,
                                       unsigned init_data_length) {
  WebMediaPlayer::MediaKeyException e =
      GenerateKeyRequestInternal(key_system, init_data, init_data_length);
  ReportMediaKeyExceptionToUMA("generateKeyRequest", key_system, e);
  return e;
}

WebMediaPlayer::MediaKeyException
WebMediaPlayerImpl::GenerateKeyRequestInternal(
    const WebString& key_system,
    const unsigned char* init_data,
    unsigned init_data_length) {
  if (!IsSupportedKeySystem(key_system))
    return WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported;

  // We do not support run-time switching between key systems for now.
  if (current_key_system_.isEmpty())
    current_key_system_ = key_system;
  else if (key_system != current_key_system_)
    return WebMediaPlayer::MediaKeyExceptionInvalidPlayerState;

  DVLOG(1) << "generateKeyRequest: " << key_system.utf8().data() << ": "
           << std::string(reinterpret_cast<const char*>(init_data),
                          static_cast<size_t>(init_data_length));

  // TODO(xhwang): We assume all streams are from the same container (thus have
  // the same "type") for now. In the future, the "type" should be passed down
  // from the application.
  if (!decryptor_->GenerateKeyRequest(key_system.utf8(),
                                      init_data_type_,
                                      init_data, init_data_length)) {
    current_key_system_.reset();
    return WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported;
  }

  return WebMediaPlayer::MediaKeyExceptionNoError;
}

WebMediaPlayer::MediaKeyException WebMediaPlayerImpl::addKey(
    const WebString& key_system,
    const unsigned char* key,
    unsigned key_length,
    const unsigned char* init_data,
    unsigned init_data_length,
    const WebString& session_id) {
  WebMediaPlayer::MediaKeyException e = AddKeyInternal(
      key_system, key, key_length, init_data, init_data_length, session_id);
  ReportMediaKeyExceptionToUMA("addKey", key_system, e);
  return e;
}


WebMediaPlayer::MediaKeyException WebMediaPlayerImpl::AddKeyInternal(
    const WebString& key_system,
    const unsigned char* key,
    unsigned key_length,
    const unsigned char* init_data,
    unsigned init_data_length,
    const WebString& session_id) {
  DCHECK(key);
  DCHECK_GT(key_length, 0u);

  if (!IsSupportedKeySystem(key_system))
    return WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported;

  if (current_key_system_.isEmpty() || key_system != current_key_system_)
    return WebMediaPlayer::MediaKeyExceptionInvalidPlayerState;

  DVLOG(1) << "addKey: " << key_system.utf8().data() << ": "
           << std::string(reinterpret_cast<const char*>(key),
                          static_cast<size_t>(key_length)) << ", "
           << std::string(reinterpret_cast<const char*>(init_data),
                          static_cast<size_t>(init_data_length))
           << " [" << session_id.utf8().data() << "]";

  decryptor_->AddKey(key_system.utf8(), key, key_length,
                     init_data, init_data_length, session_id.utf8());
  return WebMediaPlayer::MediaKeyExceptionNoError;
}

WebMediaPlayer::MediaKeyException WebMediaPlayerImpl::cancelKeyRequest(
    const WebString& key_system,
    const WebString& session_id) {
  WebMediaPlayer::MediaKeyException e =
      CancelKeyRequestInternal(key_system, session_id);
  ReportMediaKeyExceptionToUMA("cancelKeyRequest", key_system, e);
  return e;
}

WebMediaPlayer::MediaKeyException
WebMediaPlayerImpl::CancelKeyRequestInternal(
    const WebString& key_system,
    const WebString& session_id) {
  if (!IsSupportedKeySystem(key_system))
    return WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported;

  if (current_key_system_.isEmpty() || key_system != current_key_system_)
    return WebMediaPlayer::MediaKeyExceptionInvalidPlayerState;

  decryptor_->CancelKeyRequest(key_system.utf8(), session_id.utf8());
  return WebMediaPlayer::MediaKeyExceptionNoError;
}

void WebMediaPlayerImpl::WillDestroyCurrentMessageLoop() {
  Destroy();
}

void WebMediaPlayerImpl::Repaint() {
  DCHECK(main_loop_->BelongsToCurrentThread());
  GetClient()->repaint();

  base::AutoLock auto_lock(lock_);
  pending_repaint_ = false;
}

void WebMediaPlayerImpl::OnPipelineSeek(PipelineStatus status) {
  DCHECK(main_loop_->BelongsToCurrentThread());
  starting_ = false;
  seeking_ = false;
  if (pending_seek_) {
    pending_seek_ = false;
    seek(pending_seek_seconds_);
    return;
  }

  if (status != media::PIPELINE_OK) {
    OnPipelineError(status);
    return;
  }

  // Update our paused time.
  if (paused_)
    paused_time_ = pipeline_->GetMediaTime();

  GetClient()->timeChanged();
}

void WebMediaPlayerImpl::OnPipelineEnded() {
  DCHECK(main_loop_->BelongsToCurrentThread());
  GetClient()->timeChanged();
}

void WebMediaPlayerImpl::OnPipelineError(PipelineStatus error) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing) {
    // Any error that occurs before reaching ReadyStateHaveMetadata should
    // be considered a format error.
    SetNetworkState(WebMediaPlayer::NetworkStateFormatError);
    Repaint();
    return;
  }

  switch (error) {
    case media::PIPELINE_OK:
      NOTREACHED() << "PIPELINE_OK isn't an error!";
      break;

    case media::PIPELINE_ERROR_NETWORK:
    case media::PIPELINE_ERROR_READ:
      SetNetworkState(WebMediaPlayer::NetworkStateNetworkError);
      break;

    // TODO(vrk): Because OnPipelineInitialize() directly reports the
    // NetworkStateFormatError instead of calling OnPipelineError(), I believe
    // this block can be deleted. Should look into it! (crbug.com/126070)
    case media::PIPELINE_ERROR_INITIALIZATION_FAILED:
    case media::PIPELINE_ERROR_COULD_NOT_RENDER:
    case media::PIPELINE_ERROR_URL_NOT_FOUND:
    case media::DEMUXER_ERROR_COULD_NOT_OPEN:
    case media::DEMUXER_ERROR_COULD_NOT_PARSE:
    case media::DEMUXER_ERROR_NO_SUPPORTED_STREAMS:
    case media::DECODER_ERROR_NOT_SUPPORTED:
      SetNetworkState(WebMediaPlayer::NetworkStateFormatError);
      break;

    case media::PIPELINE_ERROR_DECODE:
    case media::PIPELINE_ERROR_ABORT:
    case media::PIPELINE_ERROR_OPERATION_PENDING:
    case media::PIPELINE_ERROR_INVALID_STATE:
      SetNetworkState(WebMediaPlayer::NetworkStateDecodeError);
      break;

    case media::PIPELINE_ERROR_DECRYPT:
      // Decrypt error.
      base::Histogram::FactoryGet(
          (kMediaEme + KeySystemNameForUMA(current_key_system_) +
           ".DecryptError"),
          1, 1000000, 50,
          base::Histogram::kUmaTargetedHistogramFlag)->Add(1);
      // TODO(xhwang): Change to use NetworkStateDecryptError once it's added in
      // Webkit (see http://crbug.com/124486).
      SetNetworkState(WebMediaPlayer::NetworkStateDecodeError);
      break;

    case media::PIPELINE_STATUS_MAX:
      NOTREACHED() << "PIPELINE_STATUS_MAX isn't a real error!";
      break;
  }

  // Repaint to trigger UI update.
  Repaint();
}

void WebMediaPlayerImpl::OnPipelineBufferingState(
    media::Pipeline::BufferingState buffering_state) {
  DVLOG(1) << "OnPipelineBufferingState(" << buffering_state << ")";

  switch (buffering_state) {
    case media::Pipeline::kHaveMetadata:
      SetReadyState(WebMediaPlayer::ReadyStateHaveMetadata);
      break;
    case media::Pipeline::kPrerollCompleted:
      SetReadyState(WebMediaPlayer::ReadyStateHaveEnoughData);
      break;
  }

  // Repaint to trigger UI update.
  Repaint();
}

void WebMediaPlayerImpl::OnDemuxerOpened(
    scoped_ptr<WebKit::WebMediaSource> media_source) {
  DCHECK(main_loop_->BelongsToCurrentThread());
  media_source->open(new WebMediaSourceClientImpl(chunk_demuxer_));
}

void WebMediaPlayerImpl::OnKeyAdded(const std::string& key_system,
                                    const std::string& session_id) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  base::Histogram::FactoryGet(
      kMediaEme + KeySystemNameForUMA(key_system) + ".KeyAdded",
      1, 1000000, 50,
      base::Histogram::kUmaTargetedHistogramFlag)->Add(1);

  GetClient()->keyAdded(WebString::fromUTF8(key_system),
                        WebString::fromUTF8(session_id));
}

void WebMediaPlayerImpl::OnNeedKey(const std::string& key_system,
                                   const std::string& session_id,
                                   const std::string& type,
                                   scoped_array<uint8> init_data,
                                   int init_data_size) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // Do not fire NeedKey event if encrypted media is not enabled.
  if (!decryptor_)
    return;

  UMA_HISTOGRAM_COUNTS(kMediaEme + std::string("NeedKey"), 1);

  DCHECK(init_data_type_.empty() || type.empty() || type == init_data_type_);
  if (init_data_type_.empty())
    init_data_type_ = type;

  GetClient()->keyNeeded(WebString::fromUTF8(key_system),
                         WebString::fromUTF8(session_id),
                         init_data.get(),
                         init_data_size);
}

#define COMPILE_ASSERT_MATCHING_ENUM(name) \
  COMPILE_ASSERT(static_cast<int>(WebKit::WebMediaPlayerClient::name) == \
                 static_cast<int>(media::Decryptor::k ## name), \
                 mismatching_enums)
COMPILE_ASSERT_MATCHING_ENUM(UnknownError);
COMPILE_ASSERT_MATCHING_ENUM(ClientError);
COMPILE_ASSERT_MATCHING_ENUM(ServiceError);
COMPILE_ASSERT_MATCHING_ENUM(OutputError);
COMPILE_ASSERT_MATCHING_ENUM(HardwareChangeError);
COMPILE_ASSERT_MATCHING_ENUM(DomainError);
#undef COMPILE_ASSERT_MATCHING_ENUM

void WebMediaPlayerImpl::OnKeyError(const std::string& key_system,
                                    const std::string& session_id,
                                    media::Decryptor::KeyError error_code,
                                    int system_code) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  base::LinearHistogram::FactoryGet(
      kMediaEme + KeySystemNameForUMA(key_system) + ".KeyError", 1,
      media::Decryptor::kMaxKeyError, media::Decryptor::kMaxKeyError + 1,
      base::Histogram::kUmaTargetedHistogramFlag)->Add(error_code);

  GetClient()->keyError(
      WebString::fromUTF8(key_system),
      WebString::fromUTF8(session_id),
      static_cast<WebKit::WebMediaPlayerClient::MediaKeyErrorCode>(error_code),
      system_code);
}

void WebMediaPlayerImpl::OnKeyMessage(const std::string& key_system,
                                      const std::string& session_id,
                                      const std::string& message,
                                      const std::string& default_url) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  const GURL default_url_gurl(default_url);
  DLOG_IF(WARNING, !default_url.empty() && !default_url_gurl.is_valid())
      << "Invalid URL in default_url: " << default_url;

  GetClient()->keyMessage(WebString::fromUTF8(key_system),
                          WebString::fromUTF8(session_id),
                          reinterpret_cast<const uint8*>(message.data()),
                          message.size(),
                          default_url_gurl);
}

void WebMediaPlayerImpl::SetOpaque(bool opaque) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  GetClient()->setOpaque(opaque);
}

void WebMediaPlayerImpl::DataSourceInitialized(const GURL& gurl, bool success) {
  DCHECK(main_loop_->BelongsToCurrentThread());

  if (!success) {
    SetNetworkState(WebMediaPlayer::NetworkStateFormatError);
    Repaint();
    return;
  }

  StartPipeline();
}

void WebMediaPlayerImpl::NotifyDownloading(bool is_downloading) {
  if (!is_downloading && network_state_ == WebMediaPlayer::NetworkStateLoading)
    SetNetworkState(WebMediaPlayer::NetworkStateIdle);
  else if (is_downloading && network_state_ == WebMediaPlayer::NetworkStateIdle)
    SetNetworkState(WebMediaPlayer::NetworkStateLoading);
  media_log_->AddEvent(
      media_log_->CreateBooleanEvent(
          media::MediaLogEvent::NETWORK_ACTIVITY_SET,
          "is_downloading_data", is_downloading));
}

void WebMediaPlayerImpl::StartPipeline() {
  starting_ = true;
  pipeline_->Start(
      filter_collection_.Pass(),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineEnded),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineError),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineSeek),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineBufferingState),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnDurationChange));
}

void WebMediaPlayerImpl::SetNetworkState(WebMediaPlayer::NetworkState state) {
  DCHECK(main_loop_->BelongsToCurrentThread());
  DVLOG(1) << "SetNetworkState: " << state;
  network_state_ = state;
  // Always notify to ensure client has the latest value.
  GetClient()->networkStateChanged();
}

void WebMediaPlayerImpl::SetReadyState(WebMediaPlayer::ReadyState state) {
  DCHECK(main_loop_->BelongsToCurrentThread());
  DVLOG(1) << "SetReadyState: " << state;

  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing &&
      state >= WebMediaPlayer::ReadyStateHaveMetadata) {
    if (!hasVideo())
      GetClient()->disableAcceleratedCompositing();
  } else if (state == WebMediaPlayer::ReadyStateHaveEnoughData) {
    if (is_local_source_ &&
        network_state_ == WebMediaPlayer::NetworkStateLoading) {
      SetNetworkState(WebMediaPlayer::NetworkStateLoaded);
    }
  }

  ready_state_ = state;
  // Always notify to ensure client has the latest value.
  GetClient()->readyStateChanged();
}

void WebMediaPlayerImpl::Destroy() {
  DCHECK(main_loop_->BelongsToCurrentThread());

  // Abort any pending IO so stopping the pipeline doesn't get blocked.
  if (data_source_)
    data_source_->Abort();
  if (chunk_demuxer_)
    chunk_demuxer_->Shutdown();

  // Make sure to kill the pipeline so there's no more media threads running.
  // Note: stopping the pipeline might block for a long time.
  base::WaitableEvent waiter(false, false);
  pipeline_->Stop(base::Bind(
      &base::WaitableEvent::Signal, base::Unretained(&waiter)));
  waiter.Wait();

  // Let V8 know we are not using extra resources anymore.
  if (incremented_externally_allocated_memory_) {
    v8::V8::AdjustAmountOfExternalAllocatedMemory(-kPlayerExtraMemory);
    incremented_externally_allocated_memory_ = false;
  }

  media_thread_.Stop();

  // Release any final references now that everything has stopped.
  data_source_ = NULL;
  chunk_demuxer_ = NULL;
}

WebKit::WebMediaPlayerClient* WebMediaPlayerImpl::GetClient() {
  DCHECK(main_loop_->BelongsToCurrentThread());
  DCHECK(client_);
  return client_;
}

WebKit::WebAudioSourceProvider* WebMediaPlayerImpl::audioSourceProvider() {
  return audio_source_provider_;
}

void WebMediaPlayerImpl::IncrementExternallyAllocatedMemory() {
  DCHECK(main_loop_->BelongsToCurrentThread());
  incremented_externally_allocated_memory_ = true;
  v8::V8::AdjustAmountOfExternalAllocatedMemory(kPlayerExtraMemory);
}

double WebMediaPlayerImpl::GetPipelineDuration() const {
  base::TimeDelta duration = pipeline_->GetMediaDuration();

  // Return positive infinity if the resource is unbounded.
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/video.html#dom-media-duration
  if (duration == media::kInfiniteDuration())
    return std::numeric_limits<double>::infinity();

  return duration.InSecondsF();
}

void WebMediaPlayerImpl::OnDurationChange() {
  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing)
    return;

  GetClient()->durationChanged();
}

void WebMediaPlayerImpl::FrameReady(
    const scoped_refptr<media::VideoFrame>& frame) {
  base::AutoLock auto_lock(lock_);
  current_frame_ = frame;

  if (pending_repaint_)
    return;

  pending_repaint_ = true;
  main_loop_->PostTask(FROM_HERE, base::Bind(
      &WebMediaPlayerImpl::Repaint, AsWeakPtr()));
}

}  // namespace webkit_media
