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
// explain it.
//
// DecideBackuptool() extends the LineageOS rule (mount count at superblock
// offset 0x34 > 0). It also runs backuptool when the source system has
// third-party addon.d scripts, because the on-disk mount count was observed at
// 0 on a system that held GApps (YRRPs/project#12). Without addon.d scripts,
// it skips partitions that are not ext4: their 0x434 bytes are not a mount
// count, and odm and vendor never carry the backuptool script.
//
// The helpers depend only on libc and the C++ standard library so they can be
// unit tested on a host without the Android tree. mount_history.cc parses the
// same fields but only logs them; it is left unchanged.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

// addon.d scripts on the source system that the target build does not ship.
struct AddonScripts {
  // True when the directory was listed, or does not exist.
  bool listed = false;
  // strerror() text when listing failed; empty otherwise.
  std::string error;
  // Sorted *.sh names other than LineageOS's own 50-lineage.sh.
  std::vector<std::string> names;
};

// The source system's addon.d directory. The running slot is the source slot.
constexpr char kSourceAddonDir[] = "/system/addon.d";

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

// Lists the *.sh files in |dir| except 50-lineage.sh.
AddonScripts ListThirdPartyAddonScripts(const std::string& dir);

// Decides whether backuptool runs and names the reason. |addons| is the
// source system's listing for the system partition, and default (unlisted,
// no error) for every other partition.
BackuptoolDecision DecideBackuptool(bool have_source_device,
                                    const Ext4SuperblockInfo& source,
                                    const AddonScripts& addons = {});

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
// far to |on_line|, and stores its size in |output_bytes| when not null. Output from background processes the command leaves
// running is not waited for. Returns the wait status, or -1 if the shell could
// not be started. Unlike system(), it does not ignore SIGINT and SIGQUIT in the
// caller.
int RunCapturingOutput(const std::string& command,
                       const std::function<void(const std::string&)>& on_line,
                       size_t* output_bytes = nullptr);

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_PAYLOAD_CONSUMER_BACKUPTOOL_GATE_H_
