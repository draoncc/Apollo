/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */
#include "../tests_common.h"

#include <src/audio.h>

using namespace audio;

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void SetUp() override {
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  std::bitset<3> result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

config_t pcm_config(const int packetDuration, const int channels, const int mask) {
  config_t config {packetDuration, channels, mask, {0}, config_flags()};
  config.codec = codec_e::pcm;
  return config;
}

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", config_t {5, 2, 0x3, {0}, config_flags(config_t::HIGH_QUALITY)}),
    std::make_tuple("SURROUND51", config_t {5, 6, 0x3F, {0}, config_flags()}),
    std::make_tuple("SURROUND71", config_t {5, 8, 0x63F, {0}, config_flags()}),
    std::make_tuple("SURROUND51_CUSTOM", config_t {5, 6, 0x3F, {6, 4, 2, {0, 1, 4, 5, 2, 3}}, config_flags(config_t::CUSTOM_SURROUND_PARAMS)}),
    std::make_tuple("PCM_STEREO", pcm_config(5, 2, 0x3)),
    std::make_tuple("PCM_SURROUND51", pcm_config(5, 6, 0x3F)),
    std::make_tuple("PCM_SURROUND71", pcm_config(5, 8, 0x63F))
  ),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(AudioTest, TestEncode) {
  std::thread timer([&] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = mail::man->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  int pcm_packets = 0;
  std::thread capture([&] {
    // Capture raises packets on the global mail, which the stream's audio broadcast thread reads
    const auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      const auto &packet_data = packet->second;
      if (packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
      if (m_config.codec == codec_e::pcm) {
        // Captured audio is no known signal, so only the size is checked here; PcmPayloadTest checks the samples' bytes.
        // channels x 2 B x 48 x 5 ms: stereo 960 B, 5.1 2880 B, 7.1 3840 B
        const std::size_t expected_size = m_config.channels == 2 ? 960 : m_config.channels == 6 ? 2880 : 3840;
        ASSERT_EQ(packet_data.size(), expected_size);
        ++pcm_packets;
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();

  if (m_config.codec == codec_e::pcm && pcm_packets == 0) {
    // Capture only produces packets while audio is flowing through the sink
    GTEST_SKIP() << "No audio was captured on this host, so no PCM audio packets could be checked";
  }
}

namespace {
  /// The payload's bytes, for comparing against literal byte sequences independent of host endianness
  std::vector<std::uint8_t> bytes_of(const buffer_t &payload) {
    return {std::begin(payload), std::end(payload)};
  }
}  // namespace

TEST(PcmPayloadTest, SamplesAreLittleEndianInt16InOrder) {
  // 4660 = 0x1234 and -4660 = 0xEDCC, as exact fractions of 32768
  auto payload = pcm_payload({4660.0f / 32768, -4660.0f / 32768, 0.0f});

  EXPECT_EQ(bytes_of(payload), (std::vector<std::uint8_t> {0x34, 0x12, 0xCC, 0xED, 0x00, 0x00}));
}

TEST(PcmPayloadTest, SamplesRoundToNearestWithHalvesAwayFromZero) {
  // 1.4, 1.5, -1.5, -0.4 and 0.5 steps of 1/32768
  auto payload = pcm_payload({1.4f / 32768, 1.5f / 32768, -1.5f / 32768, -0.4f / 32768, 0.5f / 32768});

  // 1, 2, -2, 0, 1
  EXPECT_EQ(bytes_of(payload), (std::vector<std::uint8_t> {0x01, 0x00, 0x02, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x01, 0x00}));
}

TEST(PcmPayloadTest, FullScaleAndBeyondClampToInt16Range) {
  auto payload = pcm_payload({1.0f, -1.0f, 1.5f, -1.5f, 1000.0f, -1000.0f, 32767.0f / 32768, -32767.0f / 32768});

  // 32767, -32768, 32767, -32768, 32767, -32768, 32767, -32767
  EXPECT_EQ(bytes_of(payload), (std::vector<std::uint8_t> {0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F, 0x01, 0x80}));
}
