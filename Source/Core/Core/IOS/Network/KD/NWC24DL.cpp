// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/KD/NWC24DL.h"

#include "Common/BitUtils.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::NWC24
{
constexpr const char DL_LIST_PATH[] = "/" WII_WC24CONF_DIR "/nwc24dl.bin";

NWC24Dl::NWC24Dl(std::shared_ptr<FS::FileSystem> fs) : m_fs{std::move(fs)}
{
  ReadDlList();
}

void NWC24Dl::ReadDlList()
{
  const auto file = m_fs->OpenFile(PID_KD, PID_KD, DL_LIST_PATH, FS::Mode::Read);
  if (!file || !file->Read(&m_data, 1))
    return;

  const s32 file_error = CheckNwc24DlList();
  if (file_error)
    ERROR_LOG_FMT(IOS_WC24, "There is an error in the DL list for WC24: {}", file_error);
}

s32 NWC24Dl::CheckNwc24DlList() const
{
  // 'WcDl' magic
  if (Magic() != DL_LIST_MAGIC)
  {
    ERROR_LOG_FMT(IOS_WC24, "DL list magic mismatch");
    return -1;
  }

  if (Version() != 1)
  {
    ERROR_LOG_FMT(IOS_WC24, "DL list version mismatch");
    return -1;
  }

  return 0;
}

void NWC24Dl::WriteDlList() const
{
  constexpr FS::Modes public_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};
  m_fs->CreateFullPath(PID_KD, PID_KD, DL_LIST_PATH, 0, public_modes);
  const auto file = m_fs->CreateAndOpenFile(PID_KD, PID_KD, DL_LIST_PATH, public_modes);

  if (!file || !file->Write(&m_data, 1))
    ERROR_LOG_FMT(IOS_WC24, "Failed to open or write WC24 DL list file");
}

bool NWC24Dl::DoesEntryExist(u16 entry_index) const
{
  return m_data.entries[entry_index].low_title_id != 0;
}

std::string NWC24Dl::GetDownloadURL(u16 entry_index, std::optional<u8> subtask_id) const
{
  std::string url(m_data.entries[entry_index].dl_url);

  // Determine if we need to append the subtask to the URL.
  if (subtask_id &&
      Common::ExtractBit(Common::swap32(m_data.entries[entry_index].subtask_bitmask), 1))
  {
    url.append(fmt::format(".{:02d}", *subtask_id));
  }

  return url;
}

std::string NWC24Dl::GetVFFContentName(u16 entry_index, std::optional<u8> subtask_id) const
{
  std::string content(m_data.entries[entry_index].filename);

  // Determine if we need to append the subtask to the name.
  if (subtask_id &&
      Common::ExtractBit(Common::swap32(m_data.entries[entry_index].subtask_bitmask), 0))
  {
    content.append(fmt::format(".{:02d}", *subtask_id));
  }

  return content;
}

std::string NWC24Dl::GetVFFPath(u16 entry_index) const
{
  const u32 lower_title_id = Common::swap32(m_data.entries[entry_index].low_title_id);
  const u32 high_title_id = Common::swap32(m_data.entries[entry_index].high_title_id);

  return fmt::format("/title/{0:08x}/{1:08x}/data/wc24dl.vff", lower_title_id, high_title_id);
}

std::optional<WC24PubkMod> NWC24Dl::GetWC24PubkMod(u16 entry_index) const
{
  WC24PubkMod pubk_mod;
  const u32 lower_title_id = Common::swap32(m_data.entries[entry_index].low_title_id);
  const u32 high_title_id = Common::swap32(m_data.entries[entry_index].high_title_id);

  const std::string path =
      fmt::format("/title/{0:08x}/{1:08x}/data/wc24pubk.mod", lower_title_id, high_title_id);

  const auto file = m_fs->OpenFile(PID_KD, PID_KD, path, IOS::HLE::FS::Mode::Read);
  if (!file)
    return std::nullopt;

  if (!file->Read(&pubk_mod, 1))
    return std::nullopt;

  return pubk_mod;
}

bool NWC24Dl::IsEncrypted(u16 entry_index) const
{
  return !!Common::ExtractBit(Common::swap32(m_data.entries[entry_index].flags), 3);
}

bool NWC24Dl::IsRSASigned(u16 entry_index) const
{
  return !Common::ExtractBit(Common::swap32(m_data.entries[entry_index].flags), 2);
}

bool NWC24Dl::SkipSchedulerDownload(u16 entry_index) const
{
  // Some entries can be set to not be downloaded by the scheduler.
  return !!Common::ExtractBit(Common::swap32(m_data.entries[entry_index].flags), 5);
}

bool NWC24Dl::HasSubtask(u16 entry_index) const
{
  switch (m_data.entries[entry_index].subtask_type)
  {
  case 1:
  case 2:
  case 3:
  case 4:
    return true;
  default:
    return false;
  }
}

bool NWC24Dl::IsSubtaskDownloadDisabled(u16 entry_index) const
{
  return !!Common::ExtractBit(Common::swap16(m_data.entries[entry_index].subtask_flags), 9);
}

bool NWC24Dl::IsValidSubtask(u16 entry_index, u8 subtask_id) const
{
  return !!Common::ExtractBit(m_data.entries[entry_index].subtask_bitmask, subtask_id);
}

u64 NWC24Dl::GetNextDownloadTime(u16 record_index) const
{
  // Timestamps are stored as a UNIX timestamp but in minutes. We want seconds.
  return Common::swap32(m_data.records[record_index].next_dl_timestamp) * SECONDS_PER_MINUTE;
}

u64 NWC24Dl::GetRetryTime(u16 entry_index) const
{
  const u64 retry_time = Common::swap16(m_data.entries[entry_index].retry_frequency);
  if (retry_time == 0)
  {
    return MINUTES_PER_DAY * SECONDS_PER_MINUTE;
  }
  return retry_time * SECONDS_PER_MINUTE;
}

u64 NWC24Dl::GetDownloadMargin(u16 entry_index) const
{
  return Common::swap16(m_data.entries[entry_index].dl_margin) * SECONDS_PER_MINUTE;
}

void NWC24Dl::SetNextDownloadTime(u16 record_index, u64 value, std::optional<u8> subtask_id)
{
  if (subtask_id)
  {
    m_data.entries[record_index].subtask_timestamps[*subtask_id] =
        Common::swap32(static_cast<u32>(value / SECONDS_PER_MINUTE));
  }

  m_data.records[record_index].next_dl_timestamp =
      Common::swap32(static_cast<u32>(value / SECONDS_PER_MINUTE));
}

u64 NWC24Dl::GetLastSubtaskDownloadTime(u16 entry_index, u8 subtask_id) const
{
  return Common::swap32(m_data.entries[entry_index].subtask_timestamps[subtask_id]) *
             SECONDS_PER_MINUTE +
         Common::swap32(m_data.entries[entry_index].server_dl_interval) * SECONDS_PER_MINUTE;
}

u32 NWC24Dl::Magic() const
{
  return Common::swap32(m_data.header.magic);
}

void NWC24Dl::SetMagic(u32 magic)
{
  m_data.header.magic = Common::swap32(magic);
}

u32 NWC24Dl::Version() const
{
  return Common::swap32(m_data.header.version);
}

void NWC24Dl::SetVersion(u32 version)
{
  m_data.header.version = Common::swap32(version);
}
}  // namespace IOS::HLE::NWC24
