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

#ifndef UPDATE_ENGINE_PAYLOAD_CONSUMER_BACKUPTOOL_GATE_H_
#define UPDATE_ENGINE_PAYLOAD_CONSUMER_BACKUPTOOL_GATE_H_

// Observability for the LineageOS backuptool postinstall gate.
//
// The gate decides whether addon.d scripts run during A/B postinstall by
// reading the ext4 mount count of the source slot's partition. These helpers
// read and describe every input to that decision so the update_engine log can
// explain it. They do not change the decision: DecideBackuptool() reproduces
// the LineageOS rule (raw 16-bit value at superblock offset 0x34 > 0).
//
// The helpers depend only on libc and the C++ standard library so they can be
// unit tested on a host without the Android tree. mount_history.cc parses the
// same fields but only logs them; it is left unchanged.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace chromeos_update_engine {

// Bytes read from the start of a partition. Covers the superblock at 0x400 and
// satisfies O_DIRECT alignment for 4 KiB logical blocks.
constexpr size_t kBackuptoolGateReadSize = 4096;

struct Ext4SuperblockInfo {
  bool read_ok = false;
  bool is_ext4 = false;
  uint16_t magic = 0;
  // Raw value at superblock offset 0x34; a mount count only when is_ext4.
  uint16_t mount_count = 0;
  uint32_t mount_time = 0;
  uint32_t write_time = 0;
};

// kBuffered goes through the block device page cache, as the gate does.
// kDirect bypasses it with O_DIRECT and shows what is on disk.
enum class ReadMode { kBuffered, kDirect };

struct BackuptoolDecision {
  bool run = false;
  std::string reason;
};

struct BackuptoolGateInputs {
  std::string partition;
  unsigned int source_slot = 0;
  unsigned int target_slot = 0;
  std::string source_path;
  std::string source_real;
  std::string mapper_path;
  std::string mapper_real;
  Ext4SuperblockInfo source;
};

// Parses superblock fields from the first |size| bytes of a partition.
Ext4SuperblockInfo ParseExt4Superblock(const uint8_t* data, size_t size);

// Reads the first kBackuptoolGateReadSize bytes of |path| and parses them.
Ext4SuperblockInfo ReadExt4Superblock(const std::string& path, ReadMode mode);

// Applies the unchanged LineageOS gate and names the reason.
BackuptoolDecision DecideBackuptool(bool have_source_device,
                                    const Ext4SuperblockInfo& source);

// "_a" for slot 0, "_b" for slot 1; empty for an invalid slot.
std::string SlotSuffix(unsigned int slot);

// "/dev/block/mapper/<partition><suffix>", or empty for an invalid slot.
std::string MapperPathFor(const std::string& partition, unsigned int slot);

// realpath() of |path|, or "unresolved".
std::string ResolvePath(const std::string& path);

std::string FormatSuperblock(const Ext4SuperblockInfo& info);
std::string FormatGateLine(const BackuptoolGateInputs& inputs);
std::string FormatReadLine(const std::string& partition,
                           const std::string& source,
                           ReadMode mode,
                           const std::string& path,
                           const Ext4SuperblockInfo& info);
std::string FormatDecisionLine(const std::string& partition,
                               const BackuptoolDecision& decision);

// Runs |command| through the shell and waits for the shell to exit, like
// system(). Then passes each line of the combined stdout and stderr written so
// far to |on_line|. Output from background processes the command leaves
// running is not waited for. Returns the wait status, or -1 if the shell could
// not be started. Unlike system(), it does not ignore SIGINT and SIGQUIT in the
// caller.
int RunCapturingOutput(const std::string& command,
                       const std::function<void(const std::string&)>& on_line);

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_PAYLOAD_CONSUMER_BACKUPTOOL_GATE_H_
