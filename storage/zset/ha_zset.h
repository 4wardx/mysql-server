/** @file ha_zset.h

    @brief
  The ZSET storage engine: each table is a Redis-style sorted set
  (member -> score) backed by a hash table and an LSM store.

    @see
  /sql/handler.h and /storage/zset/ha_zset.cc
*/

#include <sys/types.h>

#include "my_base.h" /* ha_rows */
#include "my_inttypes.h"
#include "sql/handler.h" /* handler */
#include "thr_lock.h"    /* THR_LOCK, THR_LOCK_DATA */

#include "storage/zset/zset_memtable.h"
#include "storage/zset/zset_wal.h"

/** @brief
  Zset_share is a class that will be shared among all open handlers.
  It owns the per-table state:
    - ZsetMemTable mem: skiplist ordered by (score, member) plus the
      member -> ZNode hash table, kept consistent on every mutation.
    - THR_LOCK lock: the MySQL table lock.
*/
class Zset_share : public Handler_share {
  friend class ha_zset;

 public:
  Zset_share();
  ~Zset_share() override { thr_lock_delete(&lock_); }

 private:
  ZsetMemTable mem_;       ///< Versioned memtable
  Zset_wal wal_;           ///< Write-ahead log
  uint64 seq_ = 0;         ///< Sequence counter for internal keys
  bool replayed_ = false;  ///< True once the WAL has been replayed
  THR_LOCK lock_;
};

/** @brief
  Class definition for the storage engine
*/
class ha_zset : public handler {
 public:
  ha_zset(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_zset() override = default;

  /** @brief
    Engine name shown in SHOW ENGINES / SHOW CREATE TABLE.
   */
  const char *table_type() const override { return "ZSET"; }

  /** @brief
    LSM is ordered, so BTREE is the default.
   */
  enum ha_key_alg get_default_index_algorithm() const override {
    return HA_KEY_ALG_BTREE;
  }

  /** @brief
    Only BTREE (ordered) is supported; HASH would hide ordering.
   */
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override {
    return key_alg == HA_KEY_ALG_BTREE;
  }

  /** @brief
    Engine capabilities: row/binlog capable, non-transactional, reports exact
    row count, stores position as primary key, and keeps primary key columns in
    every index for covering reads.
   */
  ulonglong table_flags() const override {
    return HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE | HA_NO_TRANSACTIONS |
           HA_STATS_RECORDS_IS_EXACT | HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
           HA_PRIMARY_KEY_IN_READ_INDEX;
  }

  /** @brief
    The physical (clustered) order of a ZSET table is idx_score (score, member),
    not the primary key (member). The PK is served by the hash table. Keep the
    base-class default (false) so the optimizer never assumes PK order.
   */
  bool primary_key_is_clustered() const override { return false; }

  /** @brief
    Index capabilities. idx_score (index 1) is the LSM ordered store:
    it supports forward/backward/range/ordered scans along (score, member).
    The primary key (index 0, member) is served by the hash table, so it
    supports only whole-key point lookups (no member-range iteration; such
    queries fall back to a full LSM scan filtered by member).
   */
  ulong index_flags(uint inx [[maybe_unused]], uint part [[maybe_unused]],
                    bool all_parts [[maybe_unused]]) const override {
    return (inx == 1)
               ? (HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE)
               : (HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR);
  }

  /** @brief
    Maximum row length MySQL allows.
   */
  uint max_supported_record_length() const override {
    return HA_MAX_REC_LENGTH;
  }

  /** @brief
    ZSET has exactly two indexes: PRIMARY(member) and idx_score(score,member).
   */
  uint max_supported_keys() const override { return 2; }

  /** @brief
    idx_score has two parts (score, member); PRIMARY has one.
   */
  uint max_supported_key_parts() const override { return 2; }

  /** @brief
    idx_score key = score(8) + VARBINARY member(255 + 2 length bytes) = 265.
   */
  uint max_supported_key_length() const override { return 265; }

  /** @brief
    Full scan cost. LSM scans grow linearly with row count, so return
    records/100+1 to keep the optimizer from treating scans as free.
   */
  double scan_time() override { return stats.records / 100.0 + 1; }

  /** @brief
    Index range scan cost (rows/20+1).
   */
  double read_time(uint, uint, ha_rows rows) override {
    return (double)rows / 20.0 + 1;
  }

  /*
    Handler lifecycle and data access. The comments below describe the LSM
    behavior; the base class handler.h documents the generic contract.
   */

  /** @brief
     Create the backing store. Validates the table definition and creates the
     WAL file.
   */
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;  // required

  /** @brief
    Open a table. Obtains the per-table share (LSM), replays the WAL and
    rebuilds the state.
   */
  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;  // required

  /** @brief
    Close a table. Releases the scan cursor and closes the WAL.
   */
  int close(void) override;  // required

  /** @brief
    Delete the backing store and WAL file for the table.
   */
  int delete_table(const char *from, const dd::Table *table_def) override;

  /** @brief
    Rename the backing store / WAL file.
   */
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;

  /** @brief
    Truncate the table: clear the LSM memtable and truncate the WAL.
   */
  int truncate(dd::Table *table_def) override;

  /** @brief
    Insert a row. Encodes the primary key as the LSM internal key, inserts into
    the memtable and appends the WAL.
   */
  int write_row(uchar *buf) override;

  /** @brief
    Update a row. Tombstones the old internal key in the LSM, inserts the new
    one and appends the WAL.
   */
  int update_row(const uchar *old_data, uchar *new_data) override;

  /** @brief
    Delete a row. Writes a DELETE tombstone for the internal key in the LSM/WAL.
   */
  int delete_row(const uchar *buf) override;

  /** @brief
    Remove all rows and truncate the WAL.
   */
  int delete_all_rows(void) override;

  /** @brief
    Index lookup. Seeks the LSM to the first entry satisfying the key range.
   */
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;

  /** @brief
    Next entry along the current index scan (LSM ordered iteration).
   */
  int index_next(uchar *buf) override;

  /** @brief
    Previous entry along the current index scan.
   */
  int index_prev(uchar *buf) override;

  /** @brief
    First entry of the current index (smallest key).
   */
  int index_first(uchar *buf) override;

  /** @brief
    Last entry of the current index (largest key).
   */
  int index_last(uchar *buf) override;

  /** @brief
    Start a sequential scan. Positions the cursor at the first LSM entry.
   */
  int rnd_init(bool scan) override;  // required

  /** @brief
    Start an index scan; snapshots the sequence number so the scan is a
    stable view even when rows are updated while it runs.
   */
  int index_init(uint idx, bool sorted) override;

  /** @brief
    End a sequential scan.
   */
  int rnd_end() override;

  /** @brief
    Next row in sequential scan order (LSM ordered iteration).
   */
  int rnd_next(uchar *buf) override;  ///< required

  /** @brief
    Fetch a row by position reference.
   */
  int rnd_pos(uchar *buf, uchar *pos) override;  ///< required

  /** @brief
    Save the current row's position into ref for rnd_pos.
   */
  void position(const uchar *record) override;  ///< required

  /** @brief
    Fill handler statistics for the optimizer cost model.
   */
  int info(uint) override;  ///< required

  /** @brief
    Handle extra hints from the server.
   */
  int extra(enum ha_extra_function operation) override;

  /** @brief
    Table-level lock handling (MySQL table locks).
   */
  int external_lock(THD *thd, int lock_type) override;  ///< required

  /** @brief
    Estimate rows in a key interval for the optimizer.
   */
  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key) override;

  THR_LOCK_DATA **store_lock(
      THD *thd, THR_LOCK_DATA **to,
      enum thr_lock_type lock_type) override;  ///< required

  Zset_share *get_share();  ///< Get the share

  int validate_schema(
      const TABLE *table) const;  ///< Verify the fixed ZSET table definition

  // Pack member + score from a node back into the record buffer.
  void fill_record(uchar *buf, ZNode *node);

  // Extract member bytes from the record buffer.
  static void decode_member(const TABLE *table, const uchar *buf,
                            const uchar **m, uint *len);

  // Extract score from the record buffer.
  static double decode_score(const TABLE *table, const uchar *buf);

  // Decode an index key image into (score, member).
  static void decode_index_key(const TABLE *table, uint idx, const uchar *key,
                               double *score, const uchar **m, uint *len);

  THR_LOCK_DATA lock_;  ///< MySQL table lock
  Zset_share *share_;   ///< Shared per-table state (memtable + lock)
  ZNode *scan_pos_;     ///< rnd_next / index_next cursor
  uint64 scan_seq_;     ///< Scan snapshot watermark (~0ULL = no filtering)
};
