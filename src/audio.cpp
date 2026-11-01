/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;
  /// Largest audio packet payload the stream can send (see MAX_AUDIO_PACKET_SIZE in stream.cpp)
  constexpr std::int64_t MAX_PACKET_SIZE = 1400;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // Opus audio estimate in bitrate_kbps()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  /**
   * @brief Get the stream config for a session's audio config.
   * The Opus-only settings are ignored for PCM audio.
   */
  static opus_stream_config_t stream_config(const config_t &config) {
    if (config.codec == codec_e::pcm) {
      return stream_configs[map_stream(config.channels, false)];
    }

    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }
    return stream;
  }

  /**
   * @brief Convert a float sample to int16 by scaling, rounding and clamping (no dither).
   */
  static std::int16_t float_to_int16(float sample) {
    // Same scale as Opus' own float-to-int16 conversion: [-1, 1) maps onto the full int16 range
    const auto scaled = std::lround(sample * 32768.0f);
    return static_cast<std::int16_t>(std::clamp<long>(scaled, INT16_MIN, INT16_MAX));
  }

  static void encodePcm(const sample_queue_t &samples, const safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data) {
    while (auto sample = samples->pop()) {
      buffer_t packet {sample->size() * sizeof(std::int16_t)};

      auto out = std::begin(packet);
      for (const auto value : *sample) {
        const auto pcm = static_cast<std::uint16_t>(float_to_int16(value));
        *out++ = static_cast<std::uint8_t>(pcm & 0xFF);
        *out++ = static_cast<std::uint8_t>(pcm >> 8);
      }

      packets->raise(channel_data, std::move(packet));
    }
  }

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_config(config);

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    if (config.codec == codec_e::pcm) {
      BOOST_LOG(info) << "PCM audio: "sv << stream.sampleRate / 1000 << " kHz, "sv
                      << stream.channelCount << " channels, "sv
                      << config.packetDuration << " ms packets"sv;
      encodePcm(samples, packets, channel_data);
      return;
    }

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream || config.input_only) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_config(config);

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
      return;
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    BOOST_LOG(info) << "Selected audio sink: "sv << *sink;

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          if (config::audio.auto_capture) {
            BOOST_LOG(info) << "Reinitializing audio capture"sv;
            mic.reset();
            do {
              mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
              if (!mic) {
                BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
              }
            } while (!mic && !shutdown_event->view(5s));
          }

          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }

  config_t negotiate(const std::unordered_map<std::string_view, std::string_view> &args, std::optional<std::string_view> host, std::string_view surround_params) {
    config_t config {};
    config.channels = util::from_view(args.at("x-nv-audio.surround.numChannels"));
    config.mask = util::from_view(args.at("x-nv-audio.surround.channelMask"));
    config.packetDuration = util::from_view(args.at("x-nv-aqos.packetDuration"));
    config.flags[config_t::HIGH_QUALITY] = util::from_view(args.at("x-nv-audio.surround.AudioQuality"));

    auto codec = args.find("x-apollo-audio.codec");
    if (codec != std::end(args) && codec->second == "pcm") {
      // 16-bit samples at 48 kHz: 48 samples per channel per millisecond
      auto packet_size = (std::int64_t) config.channels * sizeof(std::int16_t) * 48 * config.packetDuration;
      if (packet_size <= MAX_PACKET_SIZE) {
        // The Opus-only settings don't apply to PCM audio
        config.codec = codec_e::pcm;
        config.flags[config_t::HIGH_QUALITY] = false;
        return config;
      }

      BOOST_LOG(error) << "Client requested PCM audio with "sv << config.channels << " channels at "sv
                       << config.packetDuration << " ms packets ("sv << packet_size << " B, limit is "sv
                       << MAX_PACKET_SIZE << " B); falling back to Opus audio"sv;
    }

    // When using stereo audio, the audio quality is (strangely) indicated by whether the Host field
    // in the RTSP message matches a local interface's IP address. Fortunately, Moonlight always sends
    // 0.0.0.0 when it wants low quality, so it is easy to check without enumerating interfaces.
    if (config.channels == 2) {
      if (host) {
        BOOST_LOG(debug) << "Found Host: "sv << *host;
        config.flags[config_t::HIGH_QUALITY] = (host->find("0.0.0.0"sv) == std::string_view::npos);
      }
    } else if (surround_params.length() > 3) {
      // Channels
      std::uint8_t c = surround_params[0] - '0';
      // Streams
      std::uint8_t n = surround_params[1] - '0';
      // Coupled streams
      std::uint8_t m = surround_params[2] - '0';
      auto valid = false;
      if ((c == 6 || c == 8) && c == config.channels && n + m == c && surround_params.length() == c + 3) {
        config.customStreamParams.channelCount = c;
        config.customStreamParams.streams = n;
        config.customStreamParams.coupledStreams = m;
        valid = true;
        for (std::uint8_t i = 0; i < c; i++) {
          config.customStreamParams.mapping[i] = surround_params[i + 3] - '0';
          if (config.customStreamParams.mapping[i] >= c) {
            valid = false;
            break;
          }
        }
      }
      config.flags[config_t::CUSTOM_SURROUND_PARAMS] = valid;
    }

    return config;
  }

  int bitrate_kbps(const config_t &config) {
    if (config.codec == codec_e::pcm) {
      return SAMPLE_RATE * 16 * config.channels / 1000;
    }

    // Opus audio's per-channel estimate; keep in sync with stream_configs above
    return (config.flags[config_t::HIGH_QUALITY] ? 256 : 96) * config.channels;
  }
}  // namespace audio
