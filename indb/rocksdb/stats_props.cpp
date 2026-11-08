//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "rocksdb/stats_props.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

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

void PageMetaRegistry::PutPage(uint32_t ds, int64_t page_no, int64_t t_start, int64_t t_end,
                               const std::string& encoded) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& pages = pages_[ds];
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
  std::sort(pages.begin(), pages.end(), [](const PageMetaEntry& lhs, const PageMetaEntry& rhs) {
    return lhs.page_no < rhs.page_no;
  });
}

void PageMetaRegistry::PutChunk(uint32_t ds, int64_t t_start, int64_t t_end,
                                const std::string& encoded) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& chunks = chunks_[ds];
  for (ChunkEntry& entry : chunks) {
    if (entry.t_start == t_start && entry.t_end == t_end) {
      entry.encoded = encoded;
      return;
    }
  }
  ChunkEntry entry;
  entry.t_start = t_start;
  entry.t_end = t_end;
  entry.encoded = encoded;
  chunks.push_back(std::move(entry));
}

void PageMetaRegistry::Clear(uint32_t ds) {
  std::lock_guard<std::mutex> lock(mutex_);
  pages_.erase(ds);
  chunks_.erase(ds);
}

size_t PageMetaRegistry::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (const auto& item : pages_) total += item.second.size();
  return total;
}

std::string PageMetaRegistry::BuildPayload(uint32_t ds, int64_t t0, int64_t t1) const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<const PageMetaEntry*> picked;
  const auto page_it = pages_.find(ds);
  if (page_it != pages_.end()) {
    for (const PageMetaEntry& entry : page_it->second) {
      if (entry.t_end < t0 || entry.t_start > t1) continue;
      picked.push_back(&entry);
    }
  }
  if (picked.empty()) return std::string();

  // The chunk record belonging to this file: match on the range the importer
  // registered. A file holds exactly one chunk, so this is unambiguous.
  std::string chunk_encoded;
  const auto chunk_it = chunks_.find(ds);
  if (chunk_it != chunks_.end()) {
    for (const ChunkEntry& entry : chunk_it->second) {
      if (entry.t_start == t0 && entry.t_end == t1) {
        chunk_encoded = entry.encoded;
        break;
      }
    }
    if (chunk_encoded.empty()) {
      for (const ChunkEntry& entry : chunk_it->second) {
        if (entry.t_start >= t0 && entry.t_end <= t1) {
          chunk_encoded = entry.encoded;
          break;
        }
      }
    }
  }

  return EncodePayload(picked, chunk_encoded.empty() ? nullptr : &chunk_encoded);
}

std::string EncodePayload(const std::vector<const PageMetaEntry*>& pages,
                          const std::string* chunk_encoded) {
  std::string out;
  size_t total = 1 + 4;
  for (const PageMetaEntry* entry : pages) total += 4 + entry->encoded.size();
  total += 4 + (chunk_encoded == nullptr ? 0 : chunk_encoded->size());
  out.reserve(total);

  out.push_back(static_cast<char>(kPayloadVersion));
  PutU32(&out, static_cast<uint32_t>(pages.size()));
  for (const PageMetaEntry* entry : pages) {
    PutU32(&out, static_cast<uint32_t>(entry->encoded.size()));
    out.append(entry->encoded);
  }
  if (chunk_encoded == nullptr) {
    PutU32(&out, 0);
  } else {
    PutU32(&out, static_cast<uint32_t>(chunk_encoded->size()));
    out.append(*chunk_encoded);
  }
  return out;
}

namespace {
template <typename T>
T HeaderValue(std::string_view bytes, size_t offset) {
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

MetaRecordView IndexRecord(std::string_view bytes) {
  // encode_meta has six native-endian i32s, three i64s and one flag byte,
  // followed by fixed-size arrays whose dimensions are in that header.
  constexpr size_t header_size = 6 * sizeof(int32_t) + 3 * sizeof(int64_t) + 1;
  if (bytes.size() < header_size) throw AlgorithmError("truncated metadata header");
  if (HeaderValue<uint32_t>(bytes, 0) != 0x494E444DU) throw AlgorithmError("metadata magic mismatch");
  const int32_t version = HeaderValue<int32_t>(bytes, 4);
  const int32_t k = HeaderValue<int32_t>(bytes, 8), l = HeaderValue<int32_t>(bytes, 12);
  const int32_t edk = HeaderValue<int32_t>(bytes, 16), n = HeaderValue<int32_t>(bytes, 20);
  if (version != kMetaVersion) throw AlgorithmError("unsupported metadata version " + std::to_string(version));
  if (k <= 0 || l < 2 || edk <= 0 || n < 0) throw AlgorithmError("metadata header has invalid dimensions");
  size_t offset = header_size;
  // Validate every array's extent without reading its values or allocating it.
  // Bound products before multiplying, including adversarial dimensions.
  auto skip = [&](size_t width, std::initializer_list<size_t> dimensions) {
    size_t elements = 1;
    for (size_t dimension : dimensions) {
      if (dimension != 0 && elements > (bytes.size() - offset) / width / dimension)
        throw AlgorithmError("truncated metadata record");
      elements *= dimension;
    }
    offset += elements * width;
  };
  const size_t ks = k, ls = l, es = edk, ns = n;
  skip(8, {ks, ls, ls}); skip(8, {ks, ls}); skip(8, {ks}); skip(4, {ns});
  skip(8, {es, ls}); skip(8, {es}); skip(4, {es}); skip(4, {ns});
  skip(8, {ls}); skip(8, {ls});
  MetaRecordView result;
  result.t_start = HeaderValue<int64_t>(bytes, 24);
  result.t_end = HeaderValue<int64_t>(bytes, 32);
  result.count = HeaderValue<int64_t>(bytes, 40);
  if (result.t_end < result.t_start || result.count < 0) throw AlgorithmError("invalid metadata time range or count");
  result.encoded = bytes;
  return result;
}
}  // namespace

bool IndexPayload(const std::string& payload, PayloadView* view, std::string* error) {
  *view = PayloadView();
  try {
    if (payload.size() < 5) throw AlgorithmError("page stats payload is truncated");
    const uint8_t version = static_cast<uint8_t>(payload[0]);
    if (version != 1 && version != kPayloadVersion) throw AlgorithmError("unsupported page stats payload version " + std::to_string(version));
    const uint32_t count = GetU32(payload.data() + 1);
    if (count > (payload.size() - 5) / 4) throw AlgorithmError("invalid page stats record count");
    size_t offset = 5;
    auto record = [&]() {
      if (payload.size() - offset < 4) throw AlgorithmError("page stats payload is truncated at record header");
      const uint32_t length = GetU32(payload.data() + offset);
      offset += 4;
      if (length > payload.size() - offset) throw AlgorithmError("page stats payload is truncated at record body");
      const std::string_view bytes(payload.data() + offset, length);
      offset += length;
      return bytes;
    };
    view->pages.reserve(count);
    for (uint32_t i = 0; i < count; ++i) view->pages.push_back(IndexRecord(record()));
    if (version == 1) return true;
    const std::string_view chunk = record();
    if (!chunk.empty()) {
      view->chunk = IndexRecord(chunk);
      view->has_chunk = true;
    }
    return true;
  } catch (const std::exception& failure) {
    if (error) *error = failure.what();
    *view = PayloadView();
    return false;
  }
}

bool DecodePayload(const std::string& payload, std::vector<PageMeta>* pages, PageMeta* chunk,
                   bool* has_chunk, std::string* error) {
  if (has_chunk != nullptr) *has_chunk = false;
  if (payload.size() < 5) {
    if (error) *error = "page stats payload is truncated";
    return false;
  }
  const uint8_t version = static_cast<uint8_t>(payload[0]);
  if (version != 1 && version != kPayloadVersion) {
    if (error) *error = "unsupported page stats payload version " + std::to_string(version);
    return false;
  }

  const uint32_t count = GetU32(payload.data() + 1);
  size_t offset = 5;
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

  // Version 1 payloads carry page records only.
  if (version == 1) return true;

  if (offset + 4 > payload.size()) {
    if (error) *error = "page stats payload is truncated at chunk header";
    return false;
  }
  const uint32_t chunk_length = GetU32(payload.data() + offset);
  offset += 4;
  if (chunk_length == 0) return true;
  if (offset + chunk_length > payload.size()) {
    if (error) *error = "page stats payload is truncated at chunk body";
    return false;
  }
  try {
    *chunk = decode_meta(std::string(payload.data() + offset, chunk_length));
  } catch (const std::exception& failure) {
    if (error) *error = std::string("chunk stats decode failed: ") + failure.what();
    return false;
  }
  if (has_chunk != nullptr) *has_chunk = true;
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
  if (ts < min_ts_) min_ts_ = ts;
  if (ts > max_ts_) max_ts_ = ts;
  return rocksdb::Status::OK();
}

rocksdb::Status IndbStatsCollector::Finish(rocksdb::UserCollectedProperties* properties) {
  if (!seen_ || registry_ == nullptr) return rocksdb::Status::OK();
  const std::string payload = registry_->BuildPayload(ds_, min_ts_, max_ts_);
  if (payload.empty()) return rocksdb::Status::OK();
  (*properties)[kPageStatsProperty] = payload;
  return rocksdb::Status::OK();
}

bool DecodeTableProperties(const rocksdb::TableProperties& properties, std::vector<PageMeta>* pages,
                           PageMeta* chunk, bool* has_chunk) {
  const auto found = properties.user_collected_properties.find(kPageStatsProperty);
  if (found == properties.user_collected_properties.end()) return false;
  std::string error;
  return DecodePayload(found->second, pages, chunk, has_chunk, &error);
}

}  // namespace indb
