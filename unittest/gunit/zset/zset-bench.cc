/*
  Performance benchmark for the ZSET core structures.

  Covers the hot paths of an OLTP-style workload on the versioned
  memtable and the write-ahead log:
    - insert           memtable.put, new members
    - update           score change: tombstone the old key, put the new
    - point lookup     hashtable lookup by member
    - ordered scan     firstLive/nextLive walk
    - seek             seekLive to a random target
    - wal append       append-only, no per-record fsync
    - wal append+fsync same as above with zset_wal_fsync=0
    - wal replay       rebuild the memtable from the log

  Build:  cmake --build build --target zset-bench
  Run:    ./build/runtime_output_directory/zset-bench [scale]
          scale = number of rows (default 1000000)
*/

#include "my_config.h"
#include "my_sys.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "storage/zset/zset_memtable.h"
#include "storage/zset/zset_wal.h"

using std::string;
using std::vector;

// Toggled by the WAL benchmarks; declared in zset_wal.cc.
extern ulong zset_wal_fsync;

namespace {

using Clock = std::chrono::steady_clock;

// Nanoseconds spent running fn once. A warm-up pass runs first so the
// measured pass sees warm caches and settled allocators.
template <typename Fn>
double time_ns(Fn &&fn) {
  fn();
  auto start = Clock::now();
  fn();
  auto end = Clock::now();
  return std::chrono::duration<double, std::nano>(end - start).count();
}

void report(const char *name, double ns, size_t ops) {
  std::printf("%-22s %12.0f ops/sec  (%8.1f ns/op)\n", name, 1e9 / (ns / ops),
              ns / ops);
}

// Sink so the compiler cannot elide the measured loops.
volatile size_t g_sink = 0;

// Random keys and scores, generated once and reused by every case.
struct Workload {
  size_t scale;
  vector<string> keys;
  vector<double> scores;

  explicit Workload(size_t scale) : scale(scale) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-1e6, 1e6);
    std::uniform_int_distribution<int> len_dist(1, 32);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    keys.reserve(scale);
    scores.reserve(scale);
    for (size_t i = 0; i < scale; i++) {
      const int len = len_dist(rng);
      std::string k;
      k.reserve(len);
      for (int j = 0; j < len; j++) {
        k.push_back(static_cast<char>(byte_dist(rng)));
      }
      keys.push_back(std::move(k));
      scores.push_back(dist(rng));
    }
  }

  void fill(ZsetMemTable *mem) const {
    uint64 seq = 1;
    for (size_t i = 0; i < scale; i++) {
      mem->put(scores[i], reinterpret_cast<const uchar *>(keys[i].data()),
               static_cast<uint>(keys[i].size()), seq++);
    }
  }
};

// Scenario: bulk-loading a fresh table, one new member per insert.
// Measures the full append path (skiplist insert + live-view hash
// update) with a cold allocator and growing table.
void bench_insert(const Workload &w) {
  size_t rows = 0;
  double ns = time_ns([&] {
    ZsetMemTable mem;
    uint64 seq = 1;
    for (size_t i = 0; i < w.scale; i++) {
      mem.put(w.scores[i], reinterpret_cast<const uchar *>(w.keys[i].data()),
              static_cast<uint>(w.keys[i].size()), seq++);
    }
    rows += mem.count();
  });
  report("insert", ns, w.scale);
  g_sink += rows;
}

// Scenario: ZINCRBY-style workload, every member changes its score.
// Each update tombstones the old (score, member) and writes the new
// one, so the table grows by two internal keys per update.
void bench_update(const Workload &w) {
  ZsetMemTable mem;
  w.fill(&mem);
  uint64 seq = w.scale + 1;
  double ns = time_ns([&] {
    for (size_t i = 0; i < w.scale; i++) {
      const uchar *m = reinterpret_cast<const uchar *>(w.keys[i].data());
      uint len = static_cast<uint>(w.keys[i].size());
      // Score change: tombstone the old key, then put the new one.
      mem.tombstone(w.scores[i], m, len, seq++);
      mem.put(w.scores[i] + 1.0, m, len, seq++);
    }
  });
  report("update (score change)", ns, w.scale);
  g_sink += mem.count();
}

// Scenario: ZSCORE-style point reads over a settled table. Members are
// picked at random, so every hit is a cold bucket/node access.
void bench_lookup(const Workload &w) {
  ZsetMemTable mem;
  w.fill(&mem);
  std::mt19937 rng(7);
  size_t hits = 0;
  double ns = time_ns([&] {
    for (size_t i = 0; i < w.scale; i++) {
      const string &k = w.keys[rng() % w.scale];
      if (mem.lookup(reinterpret_cast<const uchar *>(k.data()),
                     static_cast<uint>(k.size())) != nullptr) {
        hits++;
      }
    }
  });
  report("point lookup", ns, w.scale);
  g_sink += hits;
}

// Scenario: ZRANGE with no LIMIT, the full merged view emitted in key
// order. Each row costs a pointer chase plus the shadowed-version check.
void bench_scan(const Workload &w) {
  ZsetMemTable mem;
  w.fill(&mem);
  size_t rows = 0;
  double ns = time_ns([&] {
    for (ZNode *n = mem.firstLive(); n != nullptr; n = mem.nextLive(n)) {
      rows++;
    }
  });
  report("ordered scan", ns, rows);
  g_sink += rows;
}

// Range scan: seek to a random start, then walk a limited number of
// rows, like ORDER BY ... LIMIT.
// Scenario: ZRANGEBYSCORE ... LIMIT, the common paged query. Seek to a
// random (score, member) then walk a fixed window of rows.
void bench_range_scan(const Workload &w) {
  const size_t kLimit = 100;
  ZsetMemTable mem;
  w.fill(&mem);
  std::mt19937 rng(7);
  size_t rows = 0;
  double ns = time_ns([&] {
    for (size_t i = 0; i < w.scale; i++) {
      const string &k = w.keys[rng() % w.scale];
      const uchar *m = reinterpret_cast<const uchar *>(k.data());
      ZNode *n = mem.seekLive(w.scores[rng() % w.scale], m,
                              static_cast<uint>(k.size()));
      for (size_t j = 0; n != nullptr && j < kLimit; j++, n = mem.nextLive(n)) {
        rows++;
      }
    }
  });
  report("range scan (100 rows)", ns, w.scale);
  g_sink += rows;
}

// Scenario: ZRANK-style reverse-position lookup, a skiplist descent to
// a random target key without any subsequent traversal.
void bench_seek(const Workload &w) {
  ZsetMemTable mem;
  w.fill(&mem);
  std::mt19937 rng(7);
  size_t hits = 0;
  double ns = time_ns([&] {
    for (size_t i = 0; i < w.scale; i++) {
      size_t j = rng() % w.scale;
      const string &k = w.keys[j];
      const uchar *m = reinterpret_cast<const uchar *>(k.data());
      if (mem.seekLive(w.scores[j], m, static_cast<uint>(k.size())) !=
          nullptr) {
        hits++;
      }
    }
  });
  report("seek", ns, w.scale);
  g_sink += hits;
}

// Scenario: the durability cost of a write. Appends one mutation record
// per member to the log, with and without the per-record fsync that
// zset_wal_fsync=0 implies.
void bench_wal_append(const Workload &w, bool fsync) {
  const char *path = "/tmp/zset_bench.log";
  unlink(path);
  Zset_wal wal;
  if (wal.open(path) != 0) {
    std::fprintf(stderr, "failed to open %s\n", path);
    return;
  }
  ulong saved = zset_wal_fsync;
  zset_wal_fsync = fsync ? 0 : 1;  // 0 = fsync every record
  uint64 seq = 1;
  double ns = time_ns([&] {
    for (size_t i = 0; i < w.scale; i++) {
      const uchar *m = reinterpret_cast<const uchar *>(w.keys[i].data());
      wal.append(w.scores[i], m, static_cast<uint>(w.keys[i].size()), seq++,
                 Zset_wal::Type::kPut);
    }
  });
  zset_wal_fsync = saved;
  wal.close();
  report(fsync ? "wal append + fsync" : "wal append", ns, w.scale);
  unlink(path);
}

// Scenario: crash recovery. Re-reads a full log and rebuilds the
// memtable plus the sequence counter, the cost paid once at open().
void bench_wal_replay(const Workload &w) {
  const char *path = "/tmp/zset_bench.log";
  unlink(path);
  {
    Zset_wal wal;
    if (wal.open(path) != 0) {
      std::fprintf(stderr, "failed to open %s\n", path);
      return;
    }
    uint64 seq = 1;
    for (size_t i = 0; i < w.scale; i++) {
      const uchar *m = reinterpret_cast<const uchar *>(w.keys[i].data());
      wal.append(w.scores[i], m, static_cast<uint>(w.keys[i].size()), seq++,
                 Zset_wal::Type::kPut);
    }
  }
  Zset_wal wal;
  if (wal.open(path) != 0) {
    std::fprintf(stderr, "failed to open %s\n", path);
    return;
  }
  ZsetMemTable mem;
  uint64 next_seq = 0;
  double ns = time_ns([&] { wal.replay(&mem, &next_seq); });
  wal.close();
  report("wal replay", ns, w.scale);
  g_sink += mem.count() + next_seq;
  unlink(path);
}

}  // namespace

int main(int argc, char **argv) {
  my_init();
  size_t scale = 1000000;
  if (argc > 1) {
    scale = std::strtoull(argv[1], nullptr, 10);
  }

  Workload w(scale);
  std::printf("ZSET core benchmark, scale=%zu rows\n\n", scale);
  bench_insert(w);
  bench_update(w);
  bench_lookup(w);
  bench_scan(w);
  bench_range_scan(w);
  bench_seek(w);
  bench_wal_append(w, /*fsync=*/false);
  bench_wal_append(w, /*fsync=*/true);
  bench_wal_replay(w);
  std::printf("\nsink=%zu\n", static_cast<size_t>(g_sink));
  my_end(0);
  return 0;
}
