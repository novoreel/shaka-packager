// Copyright 2016 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packager/media/base/audio_stream_info.h"
#include "packager/media/base/media_sample.h"
#include "packager/media/base/text_stream_info.h"
#include "packager/media/base/video_stream_info.h"
#include "packager/media/codecs/aac_audio_specific_config.h"
#include "packager/media/codecs/nal_unit_to_byte_stream_converter.h"
#include "packager/media/formats/mp2t/pes_packet.h"
#include "packager/media/formats/mp2t/pes_packet_generator.h"

namespace shaka {
namespace media {
namespace mp2t {

using ::testing::_;
using ::testing::DoAll;
using ::testing::SetArgPointee;
using ::testing::Return;

namespace {

// Bogus data for testing.
const uint8_t kAnyData[] = {
  0x56, 0x87, 0x88, 0x33, 0x98, 0xAF, 0xE5,
};

const bool kIsKeyFrame = true;

// Only {Audio,Video}Codec and extra data matter for this test. Other values are
// bogus.
const VideoCodec kH264VideoCodec = VideoCodec::kCodecH264;
const AudioCodec kAacAudioCodec = AudioCodec::kCodecAAC;

// TODO(rkuroiwa): It might make sense to inject factory functions to create
// NalUnitToByteStreamConverter and AACAudioSpecificConfig so that these
// extra data don't need to be copy pasted from other tests.
const uint8_t kVideoExtraData[] = {
    0x01,        // configuration version (must be 1)
    0x00,        // AVCProfileIndication (bogus)
    0x00,        // profile_compatibility (bogus)
    0x00,        // AVCLevelIndication (bogus)
    0xFF,        // Length size minus 1 == 3
    0xE1,        // 1 sps.
    0x00, 0x1D,  // SPS length == 29
    0x67, 0x64, 0x00, 0x1E, 0xAC, 0xD9, 0x40, 0xB4,
    0x2F, 0xF9, 0x7F, 0xF0, 0x00, 0x80, 0x00, 0x91,
    0x00, 0x00, 0x03, 0x03, 0xE9, 0x00, 0x00, 0xEA,
    0x60, 0x0F, 0x16, 0x2D, 0x96,
    0x01,        // 1 pps.
    0x00, 0x0A,  // PPS length == 10
    0x68, 0xFE, 0xFD, 0xFC, 0xFB, 0x11, 0x12, 0x13, 0x14, 0x15,
};

// Basic profile.
const uint8_t kAudioExtraData[] = {0x12, 0x10};

const int kTrackId = 0;
const uint32_t kTimeScale = 90000;
const uint64_t kDuration = 180000;
const char kCodecString[] = "avc1";
const char kLanguage[] = "eng";
const uint32_t kWidth = 1280;
const uint32_t kHeight = 720;
const uint32_t kPixelWidth = 1;
const uint32_t kPixelHeight = 1;
const uint16_t kTrickPlayRate = 1;
const uint8_t kNaluLengthSize = 1;
const bool kIsEncrypted = false;

const uint8_t kSampleBits = 16;
const uint8_t kNumChannels = 2;
const uint32_t kSamplingFrequency = 44100;
const uint64_t kSeekPreroll = 0;
const uint64_t kCodecDelay = 0;
const uint32_t kMaxBitrate = 320000;
const uint32_t kAverageBitrate = 256000;

class MockNalUnitToByteStreamConverter : public NalUnitToByteStreamConverter {
 public:
  MOCK_METHOD3(Initialize,
               bool(const uint8_t* decoder_configuration_data,
                    size_t decoder_configuration_data_size,
                    bool escape_data));
  MOCK_METHOD4(ConvertUnitToByteStream,
               bool(const uint8_t* sample,
                    size_t sample_size,
                    bool is_key_frame,
                    std::vector<uint8_t>* output));
};

class MockAACAudioSpecificConfig : public AACAudioSpecificConfig {
 public:
  MOCK_METHOD1(Parse, bool(const std::vector<uint8_t>& data));
  MOCK_CONST_METHOD1(ConvertToADTS, bool(std::vector<uint8_t>* buffer));
};

scoped_refptr<VideoStreamInfo> CreateVideoStreamInfo(VideoCodec codec) {
  scoped_refptr<VideoStreamInfo> stream_info(new VideoStreamInfo(
      kTrackId, kTimeScale, kDuration, codec, kCodecString, kLanguage,
      kWidth, kHeight, kPixelWidth, kPixelHeight, kTrickPlayRate,
      kNaluLengthSize, kVideoExtraData, arraysize(kVideoExtraData),
      kIsEncrypted));
  return stream_info;
}

scoped_refptr<AudioStreamInfo> CreateAudioStreamInfo(AudioCodec codec) {
  scoped_refptr<AudioStreamInfo> stream_info(new AudioStreamInfo(
      kTrackId, kTimeScale, kDuration, codec, kCodecString, kLanguage,
      kSampleBits, kNumChannels, kSamplingFrequency, kSeekPreroll, kCodecDelay,
      kMaxBitrate, kAverageBitrate, kAudioExtraData, arraysize(kAudioExtraData),
      kIsEncrypted));
  return stream_info;
}

}  // namespace

class PesPacketGeneratorTest : public ::testing::Test {
 protected:
  void UseMockNalUnitToByteStreamConverter(
      scoped_ptr<MockNalUnitToByteStreamConverter>
          mock_nal_unit_to_byte_stream_converter) {
    generator_.converter_ = mock_nal_unit_to_byte_stream_converter.Pass();
  }

  void UseMockAACAudioSpecificConfig(
      scoped_ptr<MockAACAudioSpecificConfig> mock) {
    generator_.adts_converter_ = mock.Pass();
  }

  void H264EncryptionTest(const uint8_t* input,
                          size_t input_size,
                          const uint8_t* expected_output,
                          size_t expected_output_size) {
    scoped_refptr<VideoStreamInfo> stream_info(
        CreateVideoStreamInfo(kH264VideoCodec));
    EXPECT_TRUE(generator_.Initialize(*stream_info));
    EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

    scoped_refptr<MediaSample> sample =
        MediaSample::CopyFrom(input, input_size, kIsKeyFrame);
    const uint32_t kPts = 12345;
    const uint32_t kDts = 12300;
    sample->set_pts(kPts);
    sample->set_dts(kDts);

    scoped_ptr<MockNalUnitToByteStreamConverter> mock(
        new MockNalUnitToByteStreamConverter());

    // Returning only the input data so that it doesn't have all the unnecessary
    // NALUs to test encryption.
    std::vector<uint8_t> clear_data(input, input + input_size);
    EXPECT_CALL(*mock, ConvertUnitToByteStream(_, input_size, kIsKeyFrame, _))
        .WillOnce(DoAll(SetArgPointee<3>(clear_data), Return(true)));

    UseMockNalUnitToByteStreamConverter(mock.Pass());

    const std::vector<uint8_t> all_zero(16, 0);
    scoped_ptr<EncryptionKey> encryption_key(new EncryptionKey());
    encryption_key->key = all_zero;
    encryption_key->iv = all_zero;
    EXPECT_TRUE(generator_.SetEncryptionKey(encryption_key.Pass()));

    EXPECT_TRUE(generator_.PushSample(sample));
    EXPECT_EQ(1u, generator_.NumberOfReadyPesPackets());
    scoped_ptr<PesPacket> pes_packet = generator_.GetNextPesPacket();
    ASSERT_TRUE(pes_packet);

    std::vector<uint8_t> expected(expected_output,
                                  expected_output + expected_output_size);
    ASSERT_EQ(expected.size(), pes_packet->data().size());
    for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(expected[i], pes_packet->data()[i]) << " mismatch at " << i;
    }
    //EXPECT_EQ(expected, pes_packet->data());
  }

  // The input data should be the size of an aac frame, i.e. should not be the
  // size of an ADTS frame.
  void AacEncryptionTest(const uint8_t* input,
                         size_t input_size,
                         const uint8_t* expected_output,
                         size_t expected_output_size) {
    scoped_refptr<AudioStreamInfo> stream_info(
        CreateAudioStreamInfo(kAacAudioCodec));
    EXPECT_TRUE(generator_.Initialize(*stream_info));
    EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

    // For aac, the input from MediaSample is used. Then ADTS header is added,
    // so EXPECT_CALL does not return the |input| data.
    scoped_refptr<MediaSample> sample = MediaSample::CopyFrom(
        input, input_size, kIsKeyFrame);

    scoped_ptr<MockAACAudioSpecificConfig> mock(
        new MockAACAudioSpecificConfig());
    EXPECT_CALL(*mock, ConvertToADTS(_)).WillOnce(Return(true));

    UseMockAACAudioSpecificConfig(mock.Pass());

    const std::vector<uint8_t> all_zero(16, 0);
    scoped_ptr<EncryptionKey> encryption_key(new EncryptionKey());
    encryption_key->key = all_zero;
    encryption_key->iv = all_zero;
    EXPECT_TRUE(generator_.SetEncryptionKey(encryption_key.Pass()));

    EXPECT_TRUE(generator_.PushSample(sample));
    EXPECT_EQ(1u, generator_.NumberOfReadyPesPackets());
    scoped_ptr<PesPacket> pes_packet = generator_.GetNextPesPacket();
    ASSERT_TRUE(pes_packet);

    std::vector<uint8_t> expected(expected_output,
                                  expected_output + expected_output_size);
    EXPECT_EQ(expected, pes_packet->data());
  }

  PesPacketGenerator generator_;
};

TEST_F(PesPacketGeneratorTest, InitializeVideo) {
  scoped_refptr<VideoStreamInfo> stream_info(
      CreateVideoStreamInfo(kH264VideoCodec));
  EXPECT_TRUE(generator_.Initialize(*stream_info));
}

TEST_F(PesPacketGeneratorTest, InitializeVideoNonH264) {
  scoped_refptr<VideoStreamInfo> stream_info(
      CreateVideoStreamInfo(VideoCodec::kCodecVP9));
  EXPECT_FALSE(generator_.Initialize(*stream_info));
}

TEST_F(PesPacketGeneratorTest, InitializeAudio) {
  scoped_refptr<AudioStreamInfo> stream_info(
      CreateAudioStreamInfo(kAacAudioCodec));
  EXPECT_TRUE(generator_.Initialize(*stream_info));
}

TEST_F(PesPacketGeneratorTest, InitializeAudioNonAac) {
  scoped_refptr<AudioStreamInfo> stream_info(
      CreateAudioStreamInfo(AudioCodec::kCodecOpus));
  EXPECT_FALSE(generator_.Initialize(*stream_info));
}

// Text is not supported yet.
TEST_F(PesPacketGeneratorTest, InitializeTextInfo) {
  scoped_refptr<TextStreamInfo> stream_info(
      new TextStreamInfo(kTrackId, kTimeScale, kDuration, kCodecString,
                         kLanguage, std::string(), kWidth, kHeight));
  EXPECT_FALSE(generator_.Initialize(*stream_info));
}

TEST_F(PesPacketGeneratorTest, AddVideoSample) {
  scoped_refptr<VideoStreamInfo> stream_info(
      CreateVideoStreamInfo(kH264VideoCodec));
  EXPECT_TRUE(generator_.Initialize(*stream_info));
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  scoped_refptr<MediaSample> sample =
      MediaSample::CopyFrom(kAnyData, arraysize(kAnyData), kIsKeyFrame);
  const uint32_t kPts = 12345;
  const uint32_t kDts = 12300;
  sample->set_pts(kPts);
  sample->set_dts(kDts);

  std::vector<uint8_t> expected_data(kAnyData, kAnyData + arraysize(kAnyData));

  scoped_ptr<MockNalUnitToByteStreamConverter> mock(
      new MockNalUnitToByteStreamConverter());
  EXPECT_CALL(*mock,
              ConvertUnitToByteStream(_, arraysize(kAnyData), kIsKeyFrame, _))
      .WillOnce(DoAll(SetArgPointee<3>(expected_data), Return(true)));

  UseMockNalUnitToByteStreamConverter(mock.Pass());

  EXPECT_TRUE(generator_.PushSample(sample));
  EXPECT_EQ(1u, generator_.NumberOfReadyPesPackets());
  scoped_ptr<PesPacket> pes_packet = generator_.GetNextPesPacket();
  ASSERT_TRUE(pes_packet);
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  EXPECT_EQ(0xe0, pes_packet->stream_id());
  EXPECT_EQ(kPts, pes_packet->pts());
  EXPECT_EQ(kDts, pes_packet->dts());
  EXPECT_EQ(expected_data, pes_packet->data());

  EXPECT_TRUE(generator_.Flush());
}

TEST_F(PesPacketGeneratorTest, AddVideoSampleFailedToConvert) {
  scoped_refptr<VideoStreamInfo> stream_info(
      CreateVideoStreamInfo(kH264VideoCodec));
  EXPECT_TRUE(generator_.Initialize(*stream_info));
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  scoped_refptr<MediaSample> sample =
      MediaSample::CopyFrom(kAnyData, arraysize(kAnyData), kIsKeyFrame);

  std::vector<uint8_t> expected_data(kAnyData, kAnyData + arraysize(kAnyData));
  scoped_ptr<MockNalUnitToByteStreamConverter> mock(
      new MockNalUnitToByteStreamConverter());
  EXPECT_CALL(*mock,
              ConvertUnitToByteStream(_, arraysize(kAnyData), kIsKeyFrame, _))
      .WillOnce(Return(false));

  UseMockNalUnitToByteStreamConverter(mock.Pass());

  EXPECT_FALSE(generator_.PushSample(sample));
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());
  EXPECT_TRUE(generator_.Flush());
}

TEST_F(PesPacketGeneratorTest, AddAudioSample) {
  scoped_refptr<AudioStreamInfo> stream_info(
      CreateAudioStreamInfo(kAacAudioCodec));
  EXPECT_TRUE(generator_.Initialize(*stream_info));
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  scoped_refptr<MediaSample> sample =
      MediaSample::CopyFrom(kAnyData, arraysize(kAnyData), kIsKeyFrame);

  std::vector<uint8_t> expected_data(kAnyData, kAnyData + arraysize(kAnyData));

  scoped_ptr<MockAACAudioSpecificConfig> mock(new MockAACAudioSpecificConfig());
  EXPECT_CALL(*mock, ConvertToADTS(_))
      .WillOnce(DoAll(SetArgPointee<0>(expected_data), Return(true)));

  UseMockAACAudioSpecificConfig(mock.Pass());

  EXPECT_TRUE(generator_.PushSample(sample));
  EXPECT_EQ(1u, generator_.NumberOfReadyPesPackets());
  scoped_ptr<PesPacket> pes_packet = generator_.GetNextPesPacket();
  ASSERT_TRUE(pes_packet);
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  EXPECT_EQ(0xc0, pes_packet->stream_id());
  EXPECT_EQ(expected_data, pes_packet->data());

  EXPECT_TRUE(generator_.Flush());
}

TEST_F(PesPacketGeneratorTest, AddAudioSampleFailedToConvert) {
  scoped_refptr<AudioStreamInfo> stream_info(
      CreateAudioStreamInfo(kAacAudioCodec));
  EXPECT_TRUE(generator_.Initialize(*stream_info));
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  scoped_refptr<MediaSample> sample =
      MediaSample::CopyFrom(kAnyData, arraysize(kAnyData), kIsKeyFrame);

  scoped_ptr<MockAACAudioSpecificConfig> mock(new MockAACAudioSpecificConfig());
  EXPECT_CALL(*mock, ConvertToADTS(_)).WillOnce(Return(false));

  UseMockAACAudioSpecificConfig(mock.Pass());

  EXPECT_FALSE(generator_.PushSample(sample));
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());
  EXPECT_TRUE(generator_.Flush());
}

// Because TS has to use 90000 as its timescale, make sure that the timestamps
// are scaled.
TEST_F(PesPacketGeneratorTest, TimeStampScaling) {
  const uint32_t kTestTimescale = 1000;
  scoped_refptr<VideoStreamInfo> stream_info(new VideoStreamInfo(
      kTrackId, kTestTimescale, kDuration, kH264VideoCodec, kCodecString,
      kLanguage, kWidth, kHeight, kPixelWidth, kPixelHeight, kTrickPlayRate,
      kNaluLengthSize, kVideoExtraData, arraysize(kVideoExtraData),
      kIsEncrypted));
  EXPECT_TRUE(generator_.Initialize(*stream_info));

  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  scoped_refptr<MediaSample> sample =
      MediaSample::CopyFrom(kAnyData, arraysize(kAnyData), kIsKeyFrame);
  const uint32_t kPts = 5000;
  const uint32_t kDts = 4000;
  sample->set_pts(kPts);
  sample->set_dts(kDts);

  scoped_ptr<MockNalUnitToByteStreamConverter> mock(
      new MockNalUnitToByteStreamConverter());
  EXPECT_CALL(*mock,
              ConvertUnitToByteStream(_, arraysize(kAnyData), kIsKeyFrame, _))
      .WillOnce(Return(true));

  UseMockNalUnitToByteStreamConverter(mock.Pass());

  EXPECT_TRUE(generator_.PushSample(sample));
  EXPECT_EQ(1u, generator_.NumberOfReadyPesPackets());
  scoped_ptr<PesPacket> pes_packet = generator_.GetNextPesPacket();
  ASSERT_TRUE(pes_packet);
  EXPECT_EQ(0u, generator_.NumberOfReadyPesPackets());

  // Since 90000 (MPEG2 timescale) / 1000 (input timescale) is 90, the
  // timestamps should be multipled by 90.
  EXPECT_EQ(kPts * 90, pes_packet->pts());
  EXPECT_EQ(kDts * 90, pes_packet->dts());

  EXPECT_TRUE(generator_.Flush());
}

// The nalu is too small for it to be encrypted. Verify it is not modified.
TEST_F(PesPacketGeneratorTest, H264SampleEncryptionSmallNalu) {
  const uint8_t kNaluData[] = {
      0x00, 0x00, 0x00, 0x01, 0x61, 0xbb, 0xcc, 0xdd,
  };

  ASSERT_NO_FATAL_FAILURE(H264EncryptionTest(kNaluData, arraysize(kNaluData),
                                             kNaluData, arraysize(kNaluData)));
}

// Verify that sample encryption works.
TEST_F(PesPacketGeneratorTest, H264SampleEncryption) {
  // Use the following command to encrypt data.
  // openssl aes-128-cbc -nopad -e -in input -K
  // "00000000000000000000000000000000" -iv "00000000000000000000000000000000" >
  // enc
  const uint8_t kNaluData[] = {
      0x00, 0x00, 0x00, 0x01,  // Start code.
      0x61,                    // nalu type 1; this type should get encrypted.
      // Bogus data but should not be encrypted.
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,

      // Next 16 bytes should be encrypted.
      0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A,
      0x2B, 0x2C, 0x2D, 0x2E,

      // Next 144 bytes should be in the clear.
      0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
      0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
      0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
      0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
      0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
      0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
      0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82,
      0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
      0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
      0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
      0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
      0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,

      // Next 16 bytes should be encrypted.
      0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
      0xCB, 0xCC, 0xCD, 0xCE,

      // This last bytes should not be encrypted.
      0xCF,
  };

  const uint8_t kEncryptedNaluData[] = {
      0x00, 0x00, 0x00, 0x01,  // Start code.
      0x61,                    // nalu type 1; should get encrypted.
      // Bogus data but should sample encrypted.
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,

      // Encrypted 16 bytes.
      0x93, 0x3A, 0x2C, 0x38, 0x86, 0x4B, 0x64, 0xE2, 0x62, 0x7E, 0xCC, 0x75,
      0x71, 0xFB, 0x60, 0x7C,

      // Next 144 bytes should be in the clear.
      0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
      0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
      0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
      0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
      0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
      0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
      0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82,
      0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
      0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
      0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
      0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
      0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,

      // Encrypted 16 bytes.
      0xB7, 0x1C, 0x64, 0xAE, 0x90, 0xA4, 0x35, 0x88, 0x4F, 0xD1, 0x30, 0xC2,
      0x06, 0x2E, 0xF8, 0xA5,

      // This last bytes should not be encrypted.
      0xCF,
  };
  ASSERT_NO_FATAL_FAILURE(H264EncryptionTest(kNaluData, arraysize(kNaluData),
                                             kEncryptedNaluData,
                                             arraysize(kEncryptedNaluData)));
}

// If any block is encrypted, then the whole nal unit must be re-escaped.
TEST_F(PesPacketGeneratorTest, H264SampleEncryptionVerifyReescape) {
  const uint8_t kNaluData[] = {
      0x00, 0x00, 0x00, 0x01,  // Start code.
      0x61,                    // nalu type 1; this type should get encrypted.
      // Bogus data but should not be encrypted.
      // But 0x00 0x00 0x03 should be re-escaped.
      0x00, 0x00, 0x03, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,

      // Next 16 bytes should be encrypted.
      // Note that there is 0x00 0x00 0x03 sequence that will be reescaped.
      0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A,
      0x2B, 0x2C, 0x2D, 0x2E,

      // Next 144 bytes should be in the clear.
      0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
      0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
      0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
      0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
      0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
      0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
      0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82,
      0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
      0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
      // Still part of clear data, but this line includes 0x00 0x00 0x03
      // which should be re-escaped.
      0x9B, 0x9C, 0x9D, 0x00, 0x00, 0x03, 0x01, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
      0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
      0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,

      // Next 16 bytes should be encrypted.
      0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
      0xCB, 0xCC, 0xCD, 0xCE,

      // This last bytes should not be encrypted.
      0xCF,
  };

  const uint8_t kEncryptedNaluData[] = {
      0x00, 0x00, 0x00, 0x01,  // Start code.
      0x61,                    // nalu type 1; should get encrypted.
      // Bogus data but should not be encrypted.
      0x00, 0x00, 0x03, 0x03, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
      0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,

      // Encrypted 16 bytes.
      0x93, 0x3A, 0x2C, 0x38, 0x86, 0x4B, 0x64, 0xE2, 0x62, 0x7E, 0xCC, 0x75,
      0x71, 0xFB, 0x60, 0x7C,

      // Next 144 bytes should be in the clear.
      0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
      0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
      0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
      0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
      0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
      0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
      0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82,
      0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
      0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
      // Extra 0x03 is added.
      0x9B, 0x9C, 0x9D, 0x00, 0x00, 0x03, 0x03, 0x01, 0xA2, 0xA3, 0xA4, 0xA5,
      0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1,
      0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD,
      0xBE,

      // Encrypted 16 bytes.
      0xB7, 0x1C, 0x64, 0xAE, 0x90, 0xA4, 0x35, 0x88, 0x4F, 0xD1, 0x30, 0xC2,
      0x06, 0x2E, 0xF8, 0xA5,

      // This last bytes should not be encrypted.
      0xCF,
  };
  ASSERT_NO_FATAL_FAILURE(H264EncryptionTest(kNaluData, arraysize(kNaluData),
                                             kEncryptedNaluData,
                                             arraysize(kEncryptedNaluData)));
}

// Verify that if the last there are only 16 bytes left, then it doesn't get
// encrypted.
TEST_F(PesPacketGeneratorTest, H264SampleEncryptionLast16ByteNotEncrypted) {
  const uint8_t kNaluData[] = {
      0x00, 0x00, 0x00, 0x01,  // Start code.
      0x61,                    // nalu type 1; should get encrypted.
      // Bogus data but should not be encrypted.
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,

      // Next 16 bytes should be encrypted.
      0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A,
      0x2B, 0x2C, 0x2D, 0x2E,

      // Next 144 bytes should be in the clear.
      0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
      0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
      0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
      0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
      0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
      0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
      0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82,
      0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
      0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
      0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
      0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
      0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,

      // These 16 bytes should not be encrypted.
      0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
      0xCB, 0xCC, 0xCD, 0xCE,
  };

  const uint8_t kEncryptedNaluData[] = {
      0x00, 0x00, 0x00, 0x01,  // Start code.
      0x61,                    // nalu type 1; should get encrypted.
      // Bogus data but should not be encrypted.
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,

      // Encrypted 16 bytes.
      0x93, 0x3A, 0x2C, 0x38, 0x86, 0x4B, 0x64, 0xE2, 0x62, 0x7E, 0xCC, 0x75,
      0x71, 0xFB, 0x60, 0x7C,

      // Next 144 bytes should be in the clear.
      0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
      0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
      0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
      0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
      0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
      0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
      0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82,
      0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
      0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
      0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
      0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
      0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,

      // These 16 bytes should not be encrypted.
      0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
      0xCB, 0xCC, 0xCD, 0xCE,
  };
  ASSERT_NO_FATAL_FAILURE(H264EncryptionTest(kNaluData, arraysize(kNaluData),
                                             kEncryptedNaluData,
                                             arraysize(kEncryptedNaluData)));
}

// The sample is too small and it doesn't need to be encrypted.
TEST_F(PesPacketGeneratorTest, AacSampleEncryptionSmallSample) {
  const uint8_t kClearData[] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
      0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
  };

  ASSERT_NO_FATAL_FAILURE(AacEncryptionTest(kClearData, arraysize(kClearData),
                                            kClearData, arraysize(kClearData)));
}

// Verify that AAC can be encrypted.
TEST_F(PesPacketGeneratorTest, AacSampleEncryption) {
  // The data is long enough so that 2 blocks (32 bytes) are encrypted.
  const uint8_t kClearData[] = {
      // First 16 bytes are always clear.
      0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
      0x13, 0x14, 0x15, 0x16,

      // Next 32 bytes (2 blocks) are encrypted.
      0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22,
      0x23, 0x24, 0x25, 0x26,
      0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,
      0x33, 0x34, 0x35, 0x36,

      // The last 2 bytes are in the clear.
      0x37, 0x38,
  };

  const uint8_t kExpectedOutputData[] = {
      // First 16 bytes are always clear.
      0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
      0x13, 0x14, 0x15, 0x16,

      // Encrypted bytes.
      0xE3, 0x42, 0x9B, 0x27, 0x33, 0x67, 0x68, 0x08, 0xA5, 0xB3, 0x3E, 0xB1,
      0xEE, 0xFC, 0x9E, 0x0A, 0x8E, 0x0C, 0x73, 0xC5, 0x57, 0xEE, 0x58, 0xC7,
      0x48, 0x74, 0x2A, 0x12, 0x38, 0x4F, 0x4E, 0xAC,

      // The last 2 bytes are in the clear.
      0x37, 0x38,
  };

  ASSERT_NO_FATAL_FAILURE(AacEncryptionTest(kClearData, arraysize(kClearData),
                                            kExpectedOutputData,
                                            arraysize(kExpectedOutputData)));
}

// Verify that all the bytes after the leading few bytes are encrypted.
// Note that this is different from h264 encryption where it doesn't encrypt the
// last 16.
TEST_F(PesPacketGeneratorTest, AacSampleEncryptionLastBytesAreEncrypted) {
  const uint8_t kClearData[] = {
      // First 16 bytes are always clear.
      0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
      0x13, 0x14, 0x15, 0x16,

      // Next 32 bytes (2 blocks) are encrypted.
      0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22,
      0x23, 0x24, 0x25, 0x26,
      0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,
      0x33, 0x34, 0x35, 0x36,
  };

  const uint8_t kExpectedOutputData[] = {
      // First 16 bytes are always clear.
      0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
      0x13, 0x14, 0x15, 0x16,

      // Encrypted bytes.
      0xE3, 0x42, 0x9B, 0x27, 0x33, 0x67, 0x68, 0x08, 0xA5, 0xB3, 0x3E, 0xB1,
      0xEE, 0xFC, 0x9E, 0x0A, 0x8E, 0x0C, 0x73, 0xC5, 0x57, 0xEE, 0x58, 0xC7,
      0x48, 0x74, 0x2A, 0x12, 0x38, 0x4F, 0x4E, 0xAC,
  };
  ASSERT_NO_FATAL_FAILURE(AacEncryptionTest(kClearData, arraysize(kClearData),
                                            kExpectedOutputData,
                                            arraysize(kExpectedOutputData)));
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka
