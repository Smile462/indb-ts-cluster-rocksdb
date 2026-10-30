//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "rocksdb/stats_props.h"

#include <algorithm>
#include <cstring>

namespace indb {
namespace {

constexpr size_t kPointKeySize = 12;

void PutU32(std::string* out, uint32_t value) {
  for (int i = 3; i >= 0; --i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}
uint32_t GetU32(const char* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value = (value << 8) | static_cast<uint8_t>(p[i]);
  return value;
}
uint64_t GetU64(const char* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | static_cast<uint8_t>(p[i]);
  return value;
}

// Keys are dataset (u32 big endian) followed by the timestamp with its sign bit
// flipped, both big endian, so byte order matches numeric order.
bool DecodePointKey(const rocksdb::Slice& key, uint32_t* ds, int64_t* ts) {
  if (key.size() < kPointKeySize) return false;
  *ds = GetU32(key.data());
  *ts = static_cast<int64_t>(GetU64(key.data() + 4) ^ 0x8000000000000000ULL);
  return true;
}

}  // namespace

const char* const kPageStatsProperty = "indb.page_stats";

void PageMetaRegistry::Put(uint32_t ds, int64_t page_no, int64_t t_start, int64_t t_end,
                           const std::string& encoded) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& pages = by_dataset_[ds];
  for (PageMetaEntry& entry : pages) {
    if (entry.page_no == page_no) {
      entry.t_start = t_start;
      entry.t_end = t_end;
      entry.encoded = encoded;
      return;
    }
  }
  PageMetaEntry entry;
  entry.page_no = page_no;
  entry.t_start = t_start;
  entry.t_end = t_end;
  entry.encoded = encoded;
  pages.push_back(std::move(entry));
  std::sort(pages.begin(), pages.end(),
            [](const PageMetaEntry& lhs, const PageMetaEntry& rhs) { return lhs.page_no < rhs.page_no; });
}

void PageMetaRegistry::Clear(uint32_t ds) {
  std::lock_guard<std::mutex> lock(mutex_);
  by_dataset_.erase(ds);
}

size_t PageMetaRegistry::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (const auto& item : by_dataset_) total += item.second.size();
  return total;
}

std::string PageMetaRegistry::Range(uint32_t ds, int64_t t0, int64_t t1) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = by_dataset_.find(ds);
  if (found == by_dataset_.end()) return std::string();

  std::vector<const PageMetaEntry*> picked;
  for (const PageMetaEntry& entry : found->second) {
    if (entry.t_end < t0 || entry.t_start > t1) continue;
    picked.push_back(&entry);
  }
  if (picked.empty()) return std::string();
  return EncodePayload(picked);
}

std::string EncodePayload(const std::vector<const PageMetaEntry*>& entries) {
  std::string out;
  size_t total = 4;
  for (const PageMetaEntry* entry : entries) total += 4 + entry->encoded.size();
  out.reserve(total);
  PutU32(&out, static_cast<uint32_t>(entries.size()));
  for (const PageMetaEntry* entry : entries) {
    PutU32(&out, static_cast<uint32_t>(entry->encoded.size()));
    out.append(entry->encoded);
  }
  return out;
}

bool DecodePayload(const std::string& payload, std::vector<PageMeta>* pages, std::string* error) {
  if (payload.size() < 4) {
    if (error) *error = "page stats payload is truncated";
    return false;
  }
  const uint32_t count = GetU32(payload.data());
  size_t offset = 4;
  pages->clear();
  pages->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (offset + 4 > payload.size()) {
      if (error) *error = "page stats payload is truncated at record header";
      return false;
    }
    const uint32_t length = GetU32(payload.data() + offset);
    offset += 4;
    if (offset + length > payload.size()) {
      if (error) *error = "page stats payload is truncated at record body";
      return false;
    }
    try {
      pages->push_back(decode_meta(std::string(payload.data() + offset, length)));
    } catch (const std::exception& failure) {
      if (error) *error = std::string("page stats decode failed: ") + failure.what();
      return false;
    }
    offset += length;
  }
  return true;
}

rocksdb::Status IndbStatsCollector::AddUserKey(const rocksdb::Slice& key, const rocksdb::Slice& value,
                                               rocksdb::EntryType entry_type,
                                               rocksdb::SequenceNumber sequence, uint64_t file_size) {
  (void)value;
  (void)entry_type;
  (void)sequence;
  (void)file_size;

  uint32_t ds = 0;
  int64_t ts = 0;
  if (!DecodePointKey(key, &ds, &ts)) return rocksdb::Status::OK();
  if (!seen_) {
    seen_ = true;
    ds_ = ds;
    min_ts_ = ts;
    max_ts_ = ts;
    return rocksdb::Status::OK();
  }
  // A single file normally holds one series; keep the first dataset and
  // widen the range regardless.
  if (ts < min_ts_) min_ts_ = ts;
  if (ts > max_ts_) max_ts_ = ts;
  return rocksdb::Status::OK();
}

rocksdb::Status IndbStatsCollector::Finish(rocksdb::UserCollectedProperties* properties) {
  if (!seen_ || registry_ == nullptr) return rocksdb::Status::OK();
  const std::string payload = registry_->Range(ds_, min_ts_, max_ts_);
  if (payload.empty()) return rocksdb::Status::OK();
  (*properties)[kPageStatsProperty] = payload;
  return rocksdb::Status::OK();
}

bool DecodeTableProperties(const rocksdb::TableProperties& properties,
                           std::vector<PageMeta>* pages) {
  const auto found = properties.user_collected_properties.find(kPageStatsProperty);
  if (found == properties.user_collected_properties.end()) return false;
  std::string error;
  return DecodePayload(found->second, pages, &error);
}

}  // namespace indb
