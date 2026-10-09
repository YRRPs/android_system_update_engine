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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <memory>

namespace chromeos_update_engine {

namespace {

// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
// Super block starts from block 0, offset 0x400.
constexpr size_t kSuperblockOffset = 0x400;
constexpr size_t kMountTimeOffset = kSuperblockOffset + 0x2C;
constexpr size_t kWriteTimeOffset = kSuperblockOffset + 0x30;
constexpr size_t kMountCountOffset = kSuperblockOffset + 0x34;
constexpr size_t kMagicOffset = kSuperblockOffset + 0x38;
constexpr uint16_t kExt4Magic = 0xEF53;
constexpr unsigned int kMaxSlots = 2;
constexpr char kLinePrefix[] = "BackuptoolGate: partition=";
constexpr char kLineageAddonScript[] = "50-lineage.sh";
constexpr char kAddonScriptSuffix[] = ".sh";

uint16_t LoadLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t LoadLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

std::string Hex16(uint16_t value) {
  char buf[8];
  snprintf(buf, sizeof(buf), "0x%04x", value);
  return buf;
}

const char* ModeName(ReadMode mode) {
  return mode == ReadMode::kDirect ? "direct" : "buffered";
}

// Reads exactly |size| bytes at offset 0, retrying short reads.
bool ReadPrefix(int fd, uint8_t* buffer, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = pread(fd, buffer + done, size - done, done);
    if (n <= 0) {
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

// Starts "sh -c |command|" with stdout and stderr on |out_fd|. The child
// inherits this thread's setexeccon() context, as with system().
pid_t SpawnShell(const std::string& command, int out_fd) {
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    return -1;
  }
  posix_spawn_file_actions_adddup2(&actions, out_fd, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, out_fd, STDERR_FILENO);
  const char* argv[] = {"sh", "-c", command.c_str(), nullptr};
  pid_t pid = -1;
  int rc = posix_spawn(&pid, _PATH_BSHELL, &actions, nullptr,
                       const_cast<char* const*>(argv), environ);
  posix_spawn_file_actions_destroy(&actions);
  return rc == 0 ? pid : -1;
}

int WaitForExit(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return status;
}

struct DirCloser {
  void operator()(DIR* dir) const { closedir(dir); }
};

bool IsThirdPartyAddonScript(const std::string& name) {
  const size_t suffix_len = sizeof(kAddonScriptSuffix) - 1;
  return name.size() > suffix_len &&
         name.compare(name.size() - suffix_len, suffix_len,
                      kAddonScriptSuffix) == 0 &&
         name != kLineageAddonScript;
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (const std::string& name : names) {
    joined += (joined.empty() ? "" : ",") + name;
  }
  return joined;
}

// Explains a mount-count decision for a system whose addon.d named nothing.
std::string AddonSuffix(const AddonScripts& addons) {
  if (!addons.error.empty()) {
    return "; addon.d unreadable: " + addons.error;
  }
  return addons.listed ? "; no third-party addon.d scripts" : "";
}

// Passes each line written to |fd| so far to |on_line|. Returns the byte count.
size_t EmitLines(int fd,
                 const std::function<void(const std::string&)>& on_line) {
  std::string line;
  char buffer[4096];
  off_t offset = 0;
  ssize_t n;
  while ((n = pread(fd, buffer, sizeof(buffer), offset)) > 0) {
    offset += n;
    for (ssize_t i = 0; i < n; i++) {
      if (buffer[i] == '\n') {
        on_line(line);
        line.clear();
      } else {
        line.push_back(buffer[i]);
      }
    }
  }
  if (!line.empty()) {
    on_line(line);
  }
  return static_cast<size_t>(offset);
}

}  // namespace

Ext4SuperblockInfo ParseExt4Superblock(const uint8_t* data, size_t size) {
  Ext4SuperblockInfo info;
  if (data == nullptr || size < kMagicOffset + sizeof(uint16_t)) {
    return info;
  }
  info.read_ok = true;
  info.magic = LoadLe16(data + kMagicOffset);
  info.is_ext4 = info.magic == kExt4Magic;
  info.mount_count = LoadLe16(data + kMountCountOffset);
  info.mount_time = LoadLe32(data + kMountTimeOffset);
  info.write_time = LoadLe32(data + kWriteTimeOffset);
  return info;
}

Ext4SuperblockInfo ReadExt4Superblock(const std::string& path, ReadMode mode) {
  int flags = O_RDONLY | O_CLOEXEC;
  if (mode == ReadMode::kDirect) {
    flags |= O_DIRECT;
  }
  int fd = open(path.c_str(), flags);
  if (fd < 0) {
    return {};
  }
  // O_DIRECT needs a buffer aligned to the logical block size.
  void* raw = nullptr;
  if (posix_memalign(&raw, kBackuptoolGateReadSize, kBackuptoolGateReadSize)) {
    close(fd);
    return {};
  }
  std::unique_ptr<uint8_t, decltype(&free)> buffer(static_cast<uint8_t*>(raw),
                                                    &free);
  bool ok = ReadPrefix(fd, buffer.get(), kBackuptoolGateReadSize);
  close(fd);
  if (!ok) {
    return {};
  }
  return ParseExt4Superblock(buffer.get(), kBackuptoolGateReadSize);
}

AddonScripts ListThirdPartyAddonScripts(const std::string& dir) {
  AddonScripts addons;
  std::unique_ptr<DIR, DirCloser> handle(opendir(dir.c_str()));
  if (!handle) {
    if (errno == ENOENT) {
      addons.listed = true;
    } else {
      addons.error = strerror(errno);
    }
    return addons;
  }
  addons.listed = true;
  while (const dirent* entry = readdir(handle.get())) {
    if (IsThirdPartyAddonScript(entry->d_name)) {
      addons.names.push_back(entry->d_name);
    }
  }
  std::sort(addons.names.begin(), addons.names.end());
  return addons;
}

BackuptoolDecision DecideBackuptool(bool have_source_device,
                                    const Ext4SuperblockInfo& source,
                                    const AddonScripts& addons) {
  if (!have_source_device) {
    return {false, "no source device"};
  }
  const std::string raw = std::to_string(source.mount_count);
  const std::string count = !source.read_ok ? "superblock read failed"
                            : source.is_ext4 ? "mount count " + raw
                                             : "not ext4, legacy raw count " + raw;
  if (!addons.names.empty()) {
    return {true, "addon.d has " + JoinNames(addons.names) + "; " + count};
  }
  if (source.read_ok && !source.is_ext4) {
    // Never carries the backuptool script, so running it only exits 127.
    return {false, count};
  }
  // The LineageOS rule: run when the source was ever mounted read-write.
  const bool run = source.read_ok && source.mount_count > 0;
  return {run, count + AddonSuffix(addons)};
}

std::string SlotSuffix(unsigned int slot) {
  if (slot >= kMaxSlots) {
    return "";
  }
  return std::string("_") + static_cast<char>('a' + slot);
}

std::string MapperPathFor(const std::string& partition, unsigned int slot) {
  std::string suffix = SlotSuffix(slot);
  if (suffix.empty()) {
    return "";
  }
  return "/dev/block/mapper/" + partition + suffix;
}

std::string ResolvePath(const std::string& path) {
  if (path.empty()) {
    return "unresolved";
  }
  char resolved[PATH_MAX];
  if (realpath(path.c_str(), resolved) == nullptr) {
    return "unresolved";
  }
  return resolved;
}

std::string FormatSuperblock(const Ext4SuperblockInfo& info) {
  if (!info.read_ok) {
    return "read=failed";
  }
  std::string magic = "magic=" + Hex16(info.magic);
  if (!info.is_ext4) {
    return magic + " mount_count=not ext4 raw_count=" +
           std::to_string(info.mount_count);
  }
  return magic + " mount_count=" + std::to_string(info.mount_count) +
         " mount_time=" + std::to_string(info.mount_time) +
         " write_time=" + std::to_string(info.write_time);
}

std::string FormatGateLine(const BackuptoolGateInputs& inputs) {
  return kLinePrefix + inputs.partition +
         " source_slot=" + SlotSuffix(inputs.source_slot) +
         " target_slot=" + SlotSuffix(inputs.target_slot) +
         " source_path=" + inputs.source_path +
         " source_real=" + inputs.source_real +
         " mapper_path=" + inputs.mapper_path +
         " mapper_real=" + inputs.mapper_real + " " +
         FormatSuperblock(inputs.source);
}

std::string FormatReadLine(const std::string& partition,
                           const std::string& source,
                           ReadMode mode,
                           const std::string& path,
                           const Ext4SuperblockInfo& info) {
  return kLinePrefix + partition + " read=" + source +
         " mode=" + ModeName(mode) + " path=" + path + " " +
         FormatSuperblock(info);
}

std::string FormatDecisionLine(const std::string& partition,
                               const BackuptoolDecision& decision) {
  return kLinePrefix + partition +
         " decision=" + (decision.run ? "run" : "skip") +
         " reason=" + decision.reason;
}

int RunCapturingOutput(
    const std::string& command,
    const std::function<void(const std::string&)>& on_line,
    size_t* output_bytes) {
  // Output goes to an anonymous file rather than a pipe, so a background
  // process that inherits stdout cannot keep this call waiting for EOF.
  int out_fd = memfd_create("backuptool_output", MFD_CLOEXEC);
  if (out_fd < 0) {
    return -1;
  }
  pid_t pid = SpawnShell(command, out_fd);
  int status = pid < 0 ? -1 : WaitForExit(pid);
  size_t bytes = EmitLines(out_fd, on_line);
  if (output_bytes != nullptr) {
    *output_bytes = bytes;
  }
  close(out_fd);
  return status;
}

}  // namespace chromeos_update_engine
