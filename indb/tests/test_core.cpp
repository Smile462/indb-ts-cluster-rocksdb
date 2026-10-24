//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// End to end tests for the kernel and the RocksDB adapter.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "indb/core.h"
#include "rocksdb/store.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL %s\n", what.c_str());
    ++g_failures;
  }
}

void CheckClose(double actual, double expected, double tolerance, const std::string& what) {
  if (!(std::fabs(actual - expected) <= tolerance)) {
    std::printf("FAIL %s: %.17g vs %.17g\n", what.c_str(), actual, expected);
    ++g_failures;
  }
}

indb::Vec Make(std::initializer_list<double> values) { return indb::Vec(values); }

void TestZScore() {
  const indb::Vec x = Make({1, 2, 3, 4});
  const indb::Vec z = indb::zscore(x);
  const double sd = std::sqrt(5.0 / 3.0);
  CheckClose(z[0], (1.0 - 2.5) / sd, 1e-12, "zscore[0]");
  CheckClose(z[3], (4.0 - 2.5) / sd, 1e-12, "zscore[3]");
}

void TestFft() {
  const int n = 64;
  std::vector<double> re(n), im(n, 0.0), fr(n), fi(n), br(n), bi(n);
  for (int i = 0; i < n; ++i) re[i] = std::sin(0.3 * i) + 0.5 * std::cos(0.11 * i);
  indb::fft(re.data(), im.data(), n, fr.data(), fi.data());
  indb::ifft(fr.data(), fi.data(), n, br.data(), bi.data());
  double error = 0.0;
  for (int i = 0; i < n; ++i) {
    error = std::max(error, std::fabs(br[i] - re[i]));
    error = std::max(error, std::fabs(bi[i]));
  }
  Check(error < 1e-12, "fft roundtrip");
  Check(indb::fft_size_for(166) == 512, "fft size for l=166");
  Check(indb::fft_size_for(96) == 256, "fft size for l=96");
}

void TestEigen() {
  indb::Mat m(2, 2);
  m.at(0, 0) = 2.0;
  m.at(0, 1) = 1.0;
  m.at(1, 0) = 1.0;
  m.at(1, 1) = 2.0;
  const indb::EigenResult e = indb::eigen_symmetric(m);
  CheckClose(e.values[0], 1.0, 1e-12, "eigen small");
  CheckClose(e.values[1], 3.0, 1e-12, "eigen large");
  CheckClose(std::fabs(e.vectors.at(0, 1)), std::sqrt(0.5), 1e-12, "eigenvector");
}

void TestMetaRoundTrip() {
  indb::Points points;
  for (int i = 0; i < 300; ++i) {
    points.ts.push_back(i);
    points.values.push_back(std::sin(0.1 * i) + (i / 20) * 0.01);
  }
  indb::Config config;
  config.k = 2;
  config.l = 10;
  config.iter_build = 5;
  const indb::PageMeta meta = indb::build_page_meta(points, config, 12345);
  Check(meta.n_seq > 0, "page has subsequences");

  const std::string encoded = indb::encode_meta(meta);
  const indb::PageMeta decoded = indb::decode_meta(encoded);
  Check(decoded.k == meta.k && decoded.l == meta.l, "roundtrip header");
  Check(decoded.sum_matrices == meta.sum_matrices, "roundtrip matrices");
  Check(decoded.centroids == meta.centroids, "roundtrip centroids");

  indb::Config wrong = config;
  wrong.l = 11;
  bool threw = false;
  try {
    indb::validate_meta(decoded, wrong);
  } catch (const indb::AlgorithmError&) {
    threw = true;
  }
  Check(threw, "mismatched parameters are rejected");
}

indb::Points MakeSeries(int points, int l) {
  indb::Points out;
  for (int i = 0; i < points; ++i) {
    out.ts.push_back(i);
    const int phase = (i / l) % 2;
    const double t = static_cast<double>(i % l) / l;
    out.values.push_back(phase == 0 ? t : 1.0 - t);
  }
  return out;
}

void TestStoreEndToEnd() {
  const std::filesystem::path db_path = "E:/tmp/indb_test_db";
  std::filesystem::remove_all(db_path);

  indb::Config config;
  config.k = 2;
  config.l = 16;
  config.iter_build = 10;
  config.iter_raw = 10;
  config.page_points = 240;
  config.chunk_pages = 3;

  const indb::Points points = MakeSeries(1920, config.l);
  {
    std::unique_ptr<indb::Store> store = indb::Store::Create(db_path.string());
    const int64_t written = store->ImportPoints(7, points, config);
    Check(written == 1920, "imported point count");
    store->Flush();
  }

  {
    std::unique_ptr<indb::Store> store = indb::Store::Open(db_path.string());
    const indb::DatasetInfo info = store->Describe(7);
    Check(info.n_points == 1920, "catalog point count");
    Check(info.n_pages == 8, "page count");
    Check(info.n_chunks == 3, "chunk count");

    indb::QueryOptions options;
    options.ds = 7;
    options.t0 = 0;
    options.t1 = 1919;

    const indb::QueryResult page = store->KShape(options);
    Check(static_cast<int>(page.centers.size()) == 2, "kshape page center count");
    for (const auto& row : page.centers) Check(indb::all_finite(row), "kshape page finite");
    Check(page.stats.pages_corrected == 0, "full range needs no correction");
    Check(page.stats.pages_meta_used > 0, "metadata was used");

    options.level = indb::Level::Chunk;
    const indb::QueryResult chunk = store->KShape(options);
    Check(static_cast<int>(chunk.centers.size()) == 2, "kshape chunk center count");
    Check(chunk.stats.chunks_meta_used > 0, "chunk metadata was used");

    options.level = indb::Level::Page;
    const indb::QueryResult raw = store->KShapeRaw(options);
    Check(static_cast<int>(raw.centers.size()) == 2, "raw center count");

    const indb::QueryResult medoid = store->MShape(options);
    Check(static_cast<int>(medoid.centers.size()) == 2, "mshape center count");
    for (const auto& row : medoid.centers) Check(indb::all_finite(row), "mshape finite");

    // A partially covered range must fall back to the raw points of the edge
    // pages only.
    options.t0 = 100;
    options.t1 = 900;
    const indb::QueryResult partial = store->KShape(options);
    Check(static_cast<int>(partial.centers.size()) == 2, "partial center count");
    Check(partial.stats.pages_corrected >= 1, "partial range corrected");
  }

  {
    // Reopening must read metadata from disk, not from memory.
    std::unique_ptr<indb::Store> store = indb::Store::Open(db_path.string());
    indb::QueryOptions options;
    options.ds = 7;
    options.t0 = 0;
    options.t1 = 1919;
    const indb::QueryResult again = store->KShape(options);
    Check(static_cast<int>(again.centers.size()) == 2, "reopen center count");
    Check(again.stats.meta_bytes_read > 0, "reopen read metadata from disk");
  }

  bool threw = false;
  {
    std::unique_ptr<indb::Store> store = indb::Store::Open(db_path.string());
    indb::QueryOptions options;
    options.ds = 7;
    options.l = 99;
    try {
      store->KShape(options);
    } catch (const indb::AlgorithmError&) {
      threw = true;
    }
  }
  Check(threw, "parameter mismatch rejected at query time");

  std::filesystem::remove_all(db_path);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  struct Case {
    const char* name;
    void (*fn)();
  };
  const Case cases[] = {
      {"zscore", TestZScore}, {"fft", TestFft},   {"eigen", TestEigen},
      {"meta", TestMetaRoundTrip}, {"store", TestStoreEndToEnd},
  };
  for (const Case& item : cases) {
    std::printf("[ RUN  ] %s\n", item.name);
    try {
      item.fn();
    } catch (const std::exception& error) {
      std::printf("FAIL %s threw: %s\n", item.name, error.what());
      ++g_failures;
    }
    std::printf("[ DONE ] %s\n", item.name);
  }

  if (g_failures == 0) {
    std::printf("ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_failures);
  return 1;
}
