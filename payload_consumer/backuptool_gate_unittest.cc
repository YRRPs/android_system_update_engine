//
// Copyright (C) 2026 The YRRP Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/payload_consumer/backuptool_gate.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <climits>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace chromeos_update_engine {

namespace {

constexpr size_t kSbOffset = 0x400;

void PutLe16(std::vector<uint8_t>* image, size_t offset, uint16_t value) {
  (*image)[offset] = value & 0xff;
  (*image)[offset + 1] = value >> 8;
}

void PutLe32(std::vector<uint8_t>* image, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; i++) {
    (*image)[offset + i] = (value >> (8 * i)) & 0xff;
  }
}

// Builds the first 4 KiB of a partition with the given superblock fields.
std::vector<uint8_t> MakeImage(uint16_t magic,
                               uint16_t mount_count,
                               uint32_t mount_time,
                               uint32_t write_time) {
  std::vector<uint8_t> image(kBackuptoolGateReadSize, 0);
  PutLe32(&image, kSbOffset + 0x2C, mount_time);
  PutLe32(&image, kSbOffset + 0x30, write_time);
  PutLe16(&image, kSbOffset + 0x34, mount_count);
  PutLe16(&image, kSbOffset + 0x38, magic);
  return image;
}

std::string WriteTempImage(const std::vector<uint8_t>& image) {
  // O_DIRECT is unsupported on tmpfs, so avoid /tmp on hosts.
#ifdef __ANDROID__
  char path[] = "/data/local/tmp/backuptool_gate_test_XXXXXX";
#else
  char path[] = "./backuptool_gate_test_XXXXXX";
#endif
  int fd = mkstemp(path);
  if (fd < 0) {
    ADD_FAILURE() << "mkstemp failed";
    return path;
  }
  EXPECT_EQ(write(fd, image.data(), image.size()),
            static_cast<ssize_t>(image.size()));
  close(fd);
  return path;
}

}  // namespace

TEST(BackuptoolGateTest, ParsesExt4Superblock) {
  auto image = MakeImage(0xEF53, 1, 65555575, 65555576);
  Ext4SuperblockInfo info = ParseExt4Superblock(image.data(), image.size());
  EXPECT_TRUE(info.read_ok);
  EXPECT_TRUE(info.is_ext4);
  EXPECT_EQ(info.magic, 0xEF53);
  EXPECT_EQ(info.mount_count, 1);
  EXPECT_EQ(info.mount_time, 65555575u);
  EXPECT_EQ(info.write_time, 65555576u);
}

TEST(BackuptoolGateTest, FlagsNonExt4Superblock) {
  auto image = MakeImage(0x1234, 2750, 0, 0);
  Ext4SuperblockInfo info = ParseExt4Superblock(image.data(), image.size());
  EXPECT_TRUE(info.read_ok);
  EXPECT_FALSE(info.is_ext4);
  EXPECT_EQ(info.mount_count, 2750);
}

TEST(BackuptoolGateTest, ShortBufferIsReadFailure) {
  auto image = MakeImage(0xEF53, 1, 0, 0);
  Ext4SuperblockInfo info = ParseExt4Superblock(image.data(), kSbOffset + 0x38);
  EXPECT_FALSE(info.read_ok);
  EXPECT_FALSE(info.is_ext4);
}

TEST(BackuptoolGateTest, FormatsExt4Fields) {
  auto image = MakeImage(0xEF53, 1, 65555575, 65555576);
  EXPECT_EQ(FormatSuperblock(ParseExt4Superblock(image.data(), image.size())),
            "magic=0xef53 mount_count=1 mount_time=65555575 "
            "write_time=65555576");
}

TEST(BackuptoolGateTest, FormatsNonExt4AsNotExt4) {
  auto image = MakeImage(0x1234, 2750, 0, 0);
  EXPECT_EQ(FormatSuperblock(ParseExt4Superblock(image.data(), image.size())),
            "magic=0x1234 mount_count=not ext4 raw_count=2750");
}

TEST(BackuptoolGateTest, FormatsReadFailure) {
  EXPECT_EQ(FormatSuperblock(Ext4SuperblockInfo{}), "read=failed");
}

TEST(BackuptoolGateTest, RunsWhenExt4MountCountPositive) {
  auto image = MakeImage(0xEF53, 1, 0, 0);
  auto decision =
      DecideBackuptool(true, ParseExt4Superblock(image.data(), image.size()));
  EXPECT_TRUE(decision.run);
  EXPECT_EQ(decision.reason, "mount count 1");
}

TEST(BackuptoolGateTest, SkipsWhenExt4MountCountZero) {
  auto image = MakeImage(0xEF53, 0, 0, 0);
  auto decision =
      DecideBackuptool(true, ParseExt4Superblock(image.data(), image.size()));
  EXPECT_FALSE(decision.run);
  EXPECT_EQ(decision.reason, "mount count 0");
}

// LineageOS gates on the raw bytes without checking the magic. Keep that.
TEST(BackuptoolGateTest, NonExt4KeepsLegacyRawCountDecision) {
  auto garbage = MakeImage(0x1234, 2750, 0, 0);
  auto run = DecideBackuptool(
      true, ParseExt4Superblock(garbage.data(), garbage.size()));
  EXPECT_TRUE(run.run);
  EXPECT_EQ(run.reason, "not ext4, legacy raw count 2750");

  auto zero = MakeImage(0x1234, 0, 0, 0);
  auto skip =
      DecideBackuptool(true, ParseExt4Superblock(zero.data(), zero.size()));
  EXPECT_FALSE(skip.run);
  EXPECT_EQ(skip.reason, "not ext4, legacy raw count 0");
}

TEST(BackuptoolGateTest, SkipsWithoutSourceDevice) {
  auto decision = DecideBackuptool(false, Ext4SuperblockInfo{});
  EXPECT_FALSE(decision.run);
  EXPECT_EQ(decision.reason, "no source device");
}

TEST(BackuptoolGateTest, SkipsWhenSuperblockUnreadable) {
  auto decision = DecideBackuptool(true, Ext4SuperblockInfo{});
  EXPECT_FALSE(decision.run);
  EXPECT_EQ(decision.reason, "superblock read failed");
}

TEST(BackuptoolGateTest, SlotSuffixes) {
  EXPECT_EQ(SlotSuffix(0), "_a");
  EXPECT_EQ(SlotSuffix(1), "_b");
  EXPECT_EQ(SlotSuffix(UINT_MAX), "");
}

TEST(BackuptoolGateTest, MapperPathUsesSlotSuffix) {
  EXPECT_EQ(MapperPathFor("system", 1), "/dev/block/mapper/system_b");
  EXPECT_EQ(MapperPathFor("system", UINT_MAX), "");
}

TEST(BackuptoolGateTest, GateLineNamesEveryInput) {
  auto image = MakeImage(0xEF53, 0, 0, 1230768000);
  BackuptoolGateInputs inputs;
  inputs.partition = "system";
  inputs.source_slot = 1;
  inputs.target_slot = 0;
  inputs.source_path = "/dev/block/dm-17";
  inputs.source_real = "/dev/block/dm-17";
  inputs.mapper_path = "/dev/block/mapper/system_b";
  inputs.mapper_real = "/dev/block/dm-2";
  inputs.source = ParseExt4Superblock(image.data(), image.size());
  EXPECT_EQ(FormatGateLine(inputs),
            "BackuptoolGate: partition=system source_slot=_b target_slot=_a "
            "source_path=/dev/block/dm-17 source_real=/dev/block/dm-17 "
            "mapper_path=/dev/block/mapper/system_b mapper_real=/dev/block/dm-2 "
            "magic=0xef53 mount_count=0 mount_time=0 write_time=1230768000");
}

TEST(BackuptoolGateTest, ReadLineNamesPathAndMode) {
  auto image = MakeImage(0xEF53, 1, 2, 3);
  EXPECT_EQ(FormatReadLine("system",
                           "mapper",
                           ReadMode::kDirect,
                           "/dev/block/mapper/system_b",
                           ParseExt4Superblock(image.data(), image.size())),
            "BackuptoolGate: partition=system read=mapper mode=direct "
            "path=/dev/block/mapper/system_b magic=0xef53 mount_count=1 "
            "mount_time=2 write_time=3");
}

TEST(BackuptoolGateTest, DecisionLineStatesDecisionAndReason) {
  EXPECT_EQ(FormatDecisionLine("system", {false, "mount count 0"}),
            "BackuptoolGate: partition=system decision=skip "
            "reason=mount count 0");
  EXPECT_EQ(FormatDecisionLine("odm", {true, "mount count 1"}),
            "BackuptoolGate: partition=odm decision=run reason=mount count 1");
}

TEST(BackuptoolGateTest, ReadsSuperblockBufferedAndDirect) {
  std::string path = WriteTempImage(MakeImage(0xEF53, 1, 5, 6));
  Ext4SuperblockInfo buffered = ReadExt4Superblock(path, ReadMode::kBuffered);
  Ext4SuperblockInfo direct = ReadExt4Superblock(path, ReadMode::kDirect);
  unlink(path.c_str());
  for (const Ext4SuperblockInfo& info : {buffered, direct}) {
    EXPECT_TRUE(info.read_ok);
    EXPECT_TRUE(info.is_ext4);
    EXPECT_EQ(info.mount_count, 1);
    EXPECT_EQ(info.write_time, 6u);
  }
}

TEST(BackuptoolGateTest, MissingDeviceIsReadFailure) {
  EXPECT_FALSE(ReadExt4Superblock("/nonexistent/dev", ReadMode::kBuffered)
                   .read_ok);
  EXPECT_FALSE(
      ReadExt4Superblock("/nonexistent/dev", ReadMode::kDirect).read_ok);
}

TEST(BackuptoolGateTest, ResolvesRealPathOrMarksUnresolved) {
  EXPECT_EQ(ResolvePath("/"), "/");
  EXPECT_EQ(ResolvePath("/nonexistent/dev"), "unresolved");
  EXPECT_EQ(ResolvePath(""), "unresolved");
}

TEST(BackuptoolGateTest, CapturesStdoutStderrAndStatus) {
  std::vector<std::string> lines;
  int status = RunCapturingOutput(
      "echo out; echo err >&2; printf 'no newline'; exit 3",
      [&lines](const std::string& line) { lines.push_back(line); });
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 3);
  EXPECT_EQ(lines,
            (std::vector<std::string>{"out", "err", "no newline"}));
}

// system() returns when the shell exits. A helper an addon.d script leaves
// running in the background must not hold postinstall open.
TEST(BackuptoolGateTest, BackgroundChildDoesNotBlockReturn) {
  std::vector<std::string> lines;
  auto start = std::chrono::steady_clock::now();
  int status = RunCapturingOutput(
      "(sleep 5; echo late) & echo done",
      [&lines](const std::string& line) { lines.push_back(line); });
  auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
  EXPECT_EQ(lines, (std::vector<std::string>{"done"}));
}

TEST(BackuptoolGateTest, MissingCommandReportsShellStatus) {
  int status = RunCapturingOutput("/nonexistent/backuptool_postinstall.sh",
                                  [](const std::string&) {});
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 127);  // ret=32512 in the LineageOS log.
}

}  // namespace chromeos_update_engine
