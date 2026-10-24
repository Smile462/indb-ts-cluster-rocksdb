//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Command line front end: import, query and inspect a RocksDB dataset.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "indb/core.h"
#include "rocksdb/store.h"

namespace {

using indb::Config;
using indb::Level;
using indb::QueryOptions;
using indb::QueryResult;
using indb::Store;

void PrintUsage() {
  std::printf(
      "indb_cli <command> [options]\n"
      "\n"
      "Commands:\n"
      "  import   --db PATH --ds N --csv FILE --k K --l L [--edk N] [--r X]\n"
      "           [--page-points N] [--chunk-pages N] [--iter-build N] [--header]\n"
      "  info     --db PATH\n"
      "  query    --db PATH --ds N --t0 T --t1 T --method M [--level page|chunk]\n"
      "           [--k K] [--l L] [--repeat N] [--out FILE]\n"
      "  selftest\n"
      "\n"
      "Methods: kshape, kshape_raw, mshape, mshape_raw\n");
}

bool NextValue(int argc, char** argv, int* index, std::string* out) {
  if (*index + 1 >= argc) return false;
  *out = argv[++(*index)];
  return true;
}

int ParseInt(const std::string& text) { return static_cast<int>(std::stol(text)); }
long long ParseLong(const std::string& text) { return std::stoll(text); }

void PrintCenters(const QueryResult& result, const std::string& path) {
  FILE* out = stdout;
  if (!path.empty()) {
    out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", path.c_str());
      return;
    }
  }
  std::fprintf(out, "%zu %zu\n", result.centers.size(),
               result.centers.empty() ? 0 : result.centers[0].size());
  for (const auto& row : result.centers) {
    for (size_t i = 0; i < row.size(); ++i) {
      std::fprintf(out, "%.17g%s", row[i], i + 1 == row.size() ? "\n" : " ");
    }
  }
  if (out != stdout) std::fclose(out);
}

void PrintStats(const QueryResult& result) {
  const auto& s = result.stats;
  std::fprintf(stderr,
               "stats pages_considered=%lld pages_meta=%lld pages_corrected=%lld "
               "chunks_meta=%lld chunks_descended=%lld units_merged=%lld "
               "raw_points=%lld meta_bytes=%lld\n",
               static_cast<long long>(s.pages_considered), static_cast<long long>(s.pages_meta_used),
               static_cast<long long>(s.pages_corrected), static_cast<long long>(s.chunks_meta_used),
               static_cast<long long>(s.chunks_descended), static_cast<long long>(s.units_merged),
               static_cast<long long>(s.raw_points_read),
               static_cast<long long>(s.meta_bytes_read));
}

int RunImport(int argc, char** argv) {
  std::string db_path;
  std::string csv;
  Config config;
  int ds = 0;
  bool header = false;

  for (int i = 2; i < argc; ++i) {
    const std::string flag = argv[i];
    std::string value;
    if (flag == "--db" && NextValue(argc, argv, &i, &value)) db_path = value;
    else if (flag == "--ds" && NextValue(argc, argv, &i, &value)) ds = ParseInt(value);
    else if (flag == "--csv" && NextValue(argc, argv, &i, &value)) csv = value;
    else if (flag == "--k" && NextValue(argc, argv, &i, &value)) config.k = ParseInt(value);
    else if (flag == "--l" && NextValue(argc, argv, &i, &value)) config.l = ParseInt(value);
    else if (flag == "--edk" && NextValue(argc, argv, &i, &value)) config.edk = ParseInt(value);
    else if (flag == "--r" && NextValue(argc, argv, &i, &value)) config.r = std::stod(value);
    else if (flag == "--page-points" && NextValue(argc, argv, &i, &value)) config.page_points = ParseInt(value);
    else if (flag == "--chunk-pages" && NextValue(argc, argv, &i, &value)) config.chunk_pages = ParseInt(value);
    else if (flag == "--iter-build" && NextValue(argc, argv, &i, &value)) config.iter_build = ParseInt(value);
    else if (flag == "--seed" && NextValue(argc, argv, &i, &value)) config.seed = std::stoull(value);
    else if (flag == "--header") header = true;
    else {
      std::fprintf(stderr, "unknown option: %s\n", flag.c_str());
      return 2;
    }
  }
  if (db_path.empty() || csv.empty() || config.l < 2 || config.k <= 0) {
    PrintUsage();
    return 2;
  }

  const auto begin = std::chrono::steady_clock::now();
  std::unique_ptr<Store> store = Store::Create(db_path);
  const int64_t points = store->ImportCsv(static_cast<uint32_t>(ds), csv, config, header);
  store->Flush();
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - begin).count();
  std::printf("imported %lld points in %.3f s\n", static_cast<long long>(points), seconds);
  return 0;
}

int RunInfo(int argc, char** argv) {
  std::string db_path;
  for (int i = 2; i < argc; ++i) {
    std::string value;
    if (std::string(argv[i]) == "--db" && NextValue(argc, argv, &i, &value)) db_path = value;
  }
  if (db_path.empty()) {
    PrintUsage();
    return 2;
  }
  std::unique_ptr<Store> store = Store::Open(db_path);
  for (const auto& info : store->ListDatasets()) {
    std::printf("ds=%u k=%d l=%d edk=%d points=%lld pages=%lld chunks=%lld range=[%lld,%lld]\n",
                info.ds, info.config.k, info.config.l, info.config.edk_effective(),
                static_cast<long long>(info.n_points), static_cast<long long>(info.n_pages),
                static_cast<long long>(info.n_chunks), static_cast<long long>(info.first_ts),
                static_cast<long long>(info.last_ts));
  }
  return 0;
}

int RunQuery(int argc, char** argv) {
  std::string db_path;
  std::string method;
  std::string level = "page";
  std::string out;
  QueryOptions options;
  int repeat = 1;

  for (int i = 2; i < argc; ++i) {
    const std::string flag = argv[i];
    std::string value;
    if (flag == "--db" && NextValue(argc, argv, &i, &value)) db_path = value;
    else if (flag == "--ds" && NextValue(argc, argv, &i, &value)) options.ds = static_cast<uint32_t>(ParseInt(value));
    else if (flag == "--t0" && NextValue(argc, argv, &i, &value)) options.t0 = ParseLong(value);
    else if (flag == "--t1" && NextValue(argc, argv, &i, &value)) options.t1 = ParseLong(value);
    else if (flag == "--method" && NextValue(argc, argv, &i, &value)) method = value;
    else if (flag == "--level" && NextValue(argc, argv, &i, &value)) level = value;
    else if (flag == "--k" && NextValue(argc, argv, &i, &value)) options.k = ParseInt(value);
    else if (flag == "--l" && NextValue(argc, argv, &i, &value)) options.l = ParseInt(value);
    else if (flag == "--edk" && NextValue(argc, argv, &i, &value)) options.edk = ParseInt(value);
    else if (flag == "--repeat" && NextValue(argc, argv, &i, &value)) repeat = ParseInt(value);
    else if (flag == "--out" && NextValue(argc, argv, &i, &value)) out = value;
    else {
      std::fprintf(stderr, "unknown option: %s\n", flag.c_str());
      return 2;
    }
  }
  if (db_path.empty() || method.empty()) {
    PrintUsage();
    return 2;
  }
  options.level = level == "chunk" ? Level::Chunk : Level::Page;

  std::unique_ptr<Store> store = Store::Open(db_path);
  QueryResult result;
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < repeat; ++i) {
    if (method == "kshape_raw") {
      result = store->KShapeRaw(options);
    } else if (method == "mshape_raw") {
      result = store->MShapeRaw(options);
    } else if (method == "kshape") {
      result = store->KShape(options);
    } else if (method == "mshape") {
      result = store->MShape(options);
    } else {
      std::fprintf(stderr, "unknown method: %s\n", method.c_str());
      return 2;
    }
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - begin).count() / repeat;

  std::fprintf(stderr, "method=%s level=%s centers=%zu elapsed=%.6f s\n", method.c_str(),
               level.c_str(), result.centers.size(), seconds);
  PrintStats(result);
  PrintCenters(result, out);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  const std::string command = argv[1];
  try {
    if (command == "import") return RunImport(argc, argv);
    if (command == "info") return RunInfo(argc, argv);
    if (command == "query") return RunQuery(argc, argv);
    if (command == "selftest") {
      // Implemented in tests; the CLI keeps a pointer so the binary always
      // exposes the same entry points.
      std::printf("selftest moved to indb_tests\n");
      return 0;
    }
    PrintUsage();
    return 2;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
