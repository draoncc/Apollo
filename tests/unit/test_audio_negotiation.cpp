/**
 * @file tests/unit/test_audio_negotiation.cpp
 * @brief Test audio codec negotiation from a client's ANNOUNCE attributes.
 */
#include "../tests_common.h"

#include <src/audio.h>

using namespace audio;
using namespace std::literals;

namespace {
  using args_t = std::unordered_map<std::string_view, std::string_view>;

  /// ANNOUNCE attributes as Moonlight sends them for a given channel layout.
  args_t announce(std::string_view channels, std::string_view mask, std::string_view packetDuration) {
    return {
      {"x-nv-audio.surround.numChannels"sv, channels},
      {"x-nv-audio.surround.channelMask"sv, mask},
      {"x-nv-audio.surround.AudioQuality"sv, "0"sv},
      {"x-nv-aqos.packetDuration"sv, packetDuration},
    };
  }
}  // namespace

struct layout_t {
  std::string_view channels;
  std::string_view mask;
  int expectedChannels;
  int expectedMask;
};

struct AudioNegotiationLayoutTest: testing::TestWithParam<layout_t> {};

INSTANTIATE_TEST_SUITE_P(
  ChannelLayouts,
  AudioNegotiationLayoutTest,
  testing::Values(
    layout_t {"2"sv, "3"sv, 2, 0x3},
    layout_t {"6"sv, "63"sv, 6, 0x3F},
    layout_t {"8"sv, "1599"sv, 8, 0x63F}
  ),
  [](const auto &info) {
    return "Channels" + std::string {info.param.channels};
  }
);

TEST_P(AudioNegotiationLayoutTest, PcmAcceptGivesPcmAudioIn5msPackets) {
  auto layout = GetParam();
  auto args = announce(layout.channels, layout.mask, "5"sv);
  args.emplace("x-ss-audio-codec-accept"sv, "PCM"sv);

  auto config = negotiate(args, std::nullopt, ""sv);

  EXPECT_EQ(config.codec, codec_e::pcm);
  EXPECT_EQ(config.channels, layout.expectedChannels);
  EXPECT_EQ(config.mask, layout.expectedMask);
  EXPECT_EQ(config.packetDuration, 5);
}

TEST_P(AudioNegotiationLayoutTest, PcmAcceptOverridesClientPacketDurationTo5ms) {
  auto layout = GetParam();
  // 1 and 2 ms are the shortened durations earlier Artemis builds sent for PCM surround audio
  for (auto packetDuration : {"1"sv, "2"sv, "10"sv, "20"sv}) {
    auto args = announce(layout.channels, layout.mask, packetDuration);
    args.emplace("x-ss-audio-codec-accept"sv, "PCM"sv);

    auto config = negotiate(args, std::nullopt, ""sv);

    EXPECT_EQ(config.codec, codec_e::pcm) << "x-nv-aqos.packetDuration:" << packetDuration;
    EXPECT_EQ(config.packetDuration, 5) << "x-nv-aqos.packetDuration:" << packetDuration;
  }
}

TEST_P(AudioNegotiationLayoutTest, MissingAcceptGivesOpusAudio) {
  auto layout = GetParam();
  auto args = announce(layout.channels, layout.mask, "5"sv);

  auto config = negotiate(args, std::nullopt, ""sv);

  EXPECT_EQ(config.codec, codec_e::opus);
  EXPECT_EQ(config.channels, layout.expectedChannels);
  EXPECT_EQ(config.mask, layout.expectedMask);
  EXPECT_EQ(config.packetDuration, 5);
}

TEST(AudioNegotiationTest, UnrecognizedAcceptGivesOpusAudio) {
  for (auto value : {"pcm"sv, "Pcm"sv, "Opus"sv, "AC3"sv, "EAC3"sv, ""sv}) {
    auto args = announce("2"sv, "3"sv, "5"sv);
    args.emplace("x-ss-audio-codec-accept"sv, value);

    EXPECT_EQ(negotiate(args, std::nullopt, ""sv).codec, codec_e::opus) << "x-ss-audio-codec-accept:" << value;
  }
}

TEST(AudioNegotiationTest, RemovedApolloCodecAttributeGivesOpusAudio) {
  auto args = announce("2"sv, "3"sv, "5"sv);
  args.emplace("x-apollo-audio.codec"sv, "pcm"sv);

  EXPECT_EQ(negotiate(args, std::nullopt, ""sv).codec, codec_e::opus);
}

TEST(AudioNegotiationTest, OpusAudioQualityAttributeSetsHighQuality) {
  auto args = announce("6"sv, "63"sv, "5"sv);
  args.insert_or_assign("x-nv-audio.surround.AudioQuality"sv, "1"sv);

  EXPECT_TRUE(negotiate(args, std::nullopt, ""sv).flags[config_t::HIGH_QUALITY]);
}

TEST(AudioNegotiationTest, OpusStereoHighQualityFollowsHostHeader) {
  auto args = announce("2"sv, "3"sv, "5"sv);

  EXPECT_TRUE(negotiate(args, "192.168.1.10"sv, ""sv).flags[config_t::HIGH_QUALITY]);
  EXPECT_FALSE(negotiate(args, "0.0.0.0"sv, ""sv).flags[config_t::HIGH_QUALITY]);
}

TEST(AudioNegotiationTest, OpusSurroundUsesSessionSurroundParams) {
  auto args = announce("6"sv, "63"sv, "5"sv);

  // 6 channels, 4 streams, 2 coupled streams, mapping 0 4 1 5 2 3
  auto config = negotiate(args, std::nullopt, "642041523"sv);

  ASSERT_TRUE(config.flags[config_t::CUSTOM_SURROUND_PARAMS]);
  EXPECT_EQ(config.customStreamParams.channelCount, 6);
  EXPECT_EQ(config.customStreamParams.streams, 4);
  EXPECT_EQ(config.customStreamParams.coupledStreams, 2);
  std::vector<std::uint8_t> mapping(config.customStreamParams.mapping, config.customStreamParams.mapping + 6);
  EXPECT_EQ(mapping, (std::vector<std::uint8_t> {0, 4, 1, 5, 2, 3}));
}

TEST(AudioNegotiationTest, OpusSurroundIgnoresInvalidSurroundParams) {
  auto args = announce("6"sv, "63"sv, "5"sv);

  // mapping refers to channel 7 of 6
  EXPECT_FALSE(negotiate(args, std::nullopt, "642041527"sv).flags[config_t::CUSTOM_SURROUND_PARAMS]);
}

TEST(AudioNegotiationTest, PcmStereoIgnoresHighQuality) {
  auto args = announce("2"sv, "3"sv, "5"sv);
  args.insert_or_assign("x-nv-audio.surround.AudioQuality"sv, "1"sv);
  args.emplace("x-ss-audio-codec-accept"sv, "PCM"sv);

  auto config = negotiate(args, "192.168.1.10"sv, ""sv);

  EXPECT_EQ(config.codec, codec_e::pcm);
  EXPECT_FALSE(config.flags[config_t::HIGH_QUALITY]);
}

TEST(AudioNegotiationTest, PcmSurroundIgnoresHighQualityAndSurroundParams) {
  auto args = announce("6"sv, "63"sv, "5"sv);
  args.insert_or_assign("x-nv-audio.surround.AudioQuality"sv, "1"sv);
  args.emplace("x-ss-audio-codec-accept"sv, "PCM"sv);

  auto config = negotiate(args, std::nullopt, "642041523"sv);

  EXPECT_EQ(config.codec, codec_e::pcm);
  EXPECT_FALSE(config.flags[config_t::HIGH_QUALITY]);
  EXPECT_FALSE(config.flags[config_t::CUSTOM_SURROUND_PARAMS]);
}

TEST(AudioNegotiationTest, UnrecognizedAcceptKeepsOpusSettings) {
  // The accept is matched case-sensitively, so this session gets Opus audio with its usual settings
  auto args = announce("6"sv, "63"sv, "5"sv);
  args.insert_or_assign("x-nv-audio.surround.AudioQuality"sv, "1"sv);
  args.emplace("x-ss-audio-codec-accept"sv, "pcm"sv);

  auto config = negotiate(args, std::nullopt, "642041523"sv);

  EXPECT_EQ(config.codec, codec_e::opus);
  EXPECT_TRUE(config.flags[config_t::HIGH_QUALITY]);
  EXPECT_TRUE(config.flags[config_t::CUSTOM_SURROUND_PARAMS]);
}

TEST(AudioNegotiationTest, MissingRequiredAttributeThrows) {
  auto args = announce("2"sv, "3"sv, "5"sv);
  args.erase("x-nv-audio.surround.numChannels"sv);

  EXPECT_THROW(negotiate(args, std::nullopt, ""sv), std::out_of_range);
}

TEST(AudioNegotiationTest, PcmBitrateIs16BitAt48kHzPerChannel) {
  for (auto [channels, mask, kbps] : {
         std::tuple {"2"sv, "3"sv, 1536},
         std::tuple {"6"sv, "63"sv, 4608},
         std::tuple {"8"sv, "1599"sv, 6144},
       }) {
    auto args = announce(channels, mask, "5"sv);
    args.emplace("x-ss-audio-codec-accept"sv, "PCM"sv);

    EXPECT_EQ(bitrate_kbps(negotiate(args, std::nullopt, ""sv)), kbps) << channels << " channels";
  }
}

TEST(AudioNegotiationTest, OpusBitrateEstimateIsUnchanged) {
  auto args = announce("6"sv, "63"sv, "5"sv);
  EXPECT_EQ(bitrate_kbps(negotiate(args, std::nullopt, ""sv)), 96 * 6);

  args.insert_or_assign("x-nv-audio.surround.AudioQuality"sv, "1"sv);
  EXPECT_EQ(bitrate_kbps(negotiate(args, std::nullopt, ""sv)), 256 * 6);
}

TEST(AudioNegotiationTest, NegotiatedCodecIsReportedByName) {
  auto args = announce("8"sv, "1599"sv, "5"sv);
  EXPECT_EQ(codec_name(negotiate(args, std::nullopt, ""sv).codec), "Opus"sv);

  args.emplace("x-ss-audio-codec-accept"sv, "PCM"sv);
  EXPECT_EQ(codec_name(negotiate(args, std::nullopt, ""sv).codec), "PCM"sv);
}
