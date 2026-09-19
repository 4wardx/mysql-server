/**
  @file ha_zset.cc

  @brief
  Implementation of the ZSET storage engine handler.

  @details
  Each table is a Redis-style sorted set (member -> score) held in a
  dual-indexed memtable: a skiplist ordered by (score, member) for
  range scans and a hash table member -> ZNode for O(1) point lookups.
*/

#include "storage/zset/ha_zset.h"

#include <cstring>

#include "my_byteorder.h"
#include "my_dbug.h"
#include "my_dir.h"
#include "mysql/plugin.h"
#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "sql/table.h"
#include "typelib.h"

handlerton *zset_hton;

// WAL fsync policy, defined in zset_wal.cc.
extern ulong zset_wal_fsync;

// Flush the memtable when it holds more internal keys than this.
// Registered as zset_memtable_limit; the default matches the historical
// hard-coded limit.
static ulong zset_memtable_limit = 100'000;

// Table file extensions, for DROP/repair discovery.
static const char *zset_file_exts[] = {".zlog", nullptr};

// ZSET owns no system tables, so this always returns false.
static bool zset_is_supported_system_table(const char *, const char *, bool) {
  return false;
}

// Handler factory: create one handler instance per open.
static handler *zset_create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                                    MEM_ROOT *mem_root) {
  return new (mem_root) ha_zset(hton, table);
}

// Plugin init: register the handlerton.
static int zset_init_func(void *p) {
  DBUG_TRACE;

  zset_hton = (handlerton *)p;
  zset_hton->state = SHOW_OPTION_YES;
  zset_hton->create = zset_create_handler;
  // No HTON_CAN_RECREATE: TRUNCATE must go through handler::truncate()
  // so the memtable and the CLEAR marker stay consistent.
  zset_hton->file_extensions = zset_file_exts;
  zset_hton->is_supported_system_table = zset_is_supported_system_table;

  return 0;
}

// Plugin deinit.
static int zset_deinit_func(void *p [[maybe_unused]]) {
  DBUG_TRACE;

  assert(p);

  return 0;
}

// ============================================================================
// Plugin registration
// ============================================================================

static MYSQL_SYSVAR_ULONG(wal_fsync, zset_wal_fsync, PLUGIN_VAR_RQCMDARG,
                          "WAL fsync policy: 0=every write, 1=batched", nullptr,
                          nullptr, 0, 0, 1, 0);

// Flush the memtable to an sstable once it holds more than this many
// internal keys. Exposed so tests (and tuning) can trigger a flush
// without loading 100K rows.
static MYSQL_SYSVAR_ULONG(memtable_limit, zset_memtable_limit,
                          PLUGIN_VAR_RQCMDARG,
                          "Flush the memtable above this many internal keys",
                          nullptr, nullptr, 100000, 1, 1000000000, 0);

static SYS_VAR *zset_system_variables[] = {
    MYSQL_SYSVAR(wal_fsync), MYSQL_SYSVAR(memtable_limit), nullptr};

struct st_mysql_storage_engine zset_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(zset){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &zset_storage_engine,
    "ZSET",
    PLUGIN_AUTHOR_ORACLE,
    "ZSET storage engine",
    PLUGIN_LICENSE_GPL,
    zset_init_func,   /* Plugin Init */
    nullptr,          /* Plugin check uninstall */
    zset_deinit_func, /* Plugin Deinit */
    0x0001 /* 0.1 */,
    nullptr,               /* status variables */
    zset_system_variables, /* system variables */
    nullptr,               /* config options */
    0,                     /* flags */
} mysql_declare_plugin_end;

// ============================================================================
// Constructors
// ============================================================================

Zset_share::Zset_share() { thr_lock_init(&lock_); }

ha_zset::ha_zset(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg), share_(nullptr) {}

// ============================================================================
// Lifecycle
// ============================================================================

int ha_zset::create(const char *name, TABLE *form, HA_CREATE_INFO *,
                    dd::Table *) {
  int rc = validate_schema(form);
  if (rc) {
    return rc;
  }

  // Create the empty log file so DROP discovers the table's files.
  char path[FN_REFLEN];
  fn_format(path, name, "", ".zlog", MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  File fd = my_open(path, O_CREAT | O_RDWR, MYF(MY_WME));
  if (fd < 0) {
    return HA_ERR_CRASHED;
  }

  my_close(fd, MYF(0));
  return 0;
}

int ha_zset::open(const char *name, int, uint, const dd::Table *) {
  DBUG_TRACE;
  if (!(share_ = get_share())) {
    return HA_ERR_OUT_OF_MEM;
  }

  thr_lock_data_init(&share_->lock_, &lock_, nullptr);
  scan_sequence_ = ~0ULL;

  if (share_->lsm_.open(name) != 0) {
    return HA_ERR_CRASHED;
  }

  return 0;
}

int ha_zset::close(void) {
  DBUG_TRACE;
  return 0;
}

int ha_zset::delete_table(const char *name, const dd::Table *) {
  DBUG_TRACE;
  // Remove the log and any flushed sstable files.
  char path[FN_REFLEN];
  fn_format(path, name, "", ".zlog", MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  my_delete(path, MYF(0));

  char dir[FN_REFLEN];
  size_t dir_len = 0;
  dirname_part(dir, name, &dir_len);
  MY_DIR *d = my_dir(dir, MYF(MY_WME));
  if (d != nullptr) {
    const char *base = base_name(name);
    const std::string prefix = std::string(base) + "-";

    for (size_t i = 0; i < d->number_off_files; i++) {
      const char *fn = d->dir_entry[i].name;
      const size_t flen = strlen(fn);

      if (flen > prefix.size() + 4 &&
          strncmp(fn, prefix.c_str(), prefix.size()) == 0 &&
          strcmp(fn + flen - 4, ".sst") == 0) {
        snprintf(path, sizeof(path), "%s/%s", dir, fn);
        my_delete(path, MYF(0));
      }
    }

    my_dirend(d);
  }

  return 0;
}

int ha_zset::rename_table(const char *, const char *, const dd::Table *,
                          dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::truncate(dd::Table *) {
  DBUG_TRACE;
  if (share_->lsm_.clear() != 0) {
    return HA_ERR_CRASHED;
  }

  stats.records = 0;

  return 0;
}

// ============================================================================
// DML
// ============================================================================

int ha_zset::write_row(uchar *buf) {
  DBUG_TRACE;
  const uchar *m;
  uint len;
  decode_member(table, buf, &m, &len);
  double score = decode_score(table, buf);

  // Member is the primary key and must be unique.
  double old_score;
  if (share_->lsm_.get(m, len, &old_score)) {
    return HA_ERR_FOUND_DUPP_KEY;
  }

  share_->lsm_.put(score, m, len);
  maybe_flush();
  return 0;
}

int ha_zset::update_row(const uchar *old_data, uchar *new_data) {
  DBUG_TRACE;
  const uchar *old_m;
  uint old_len;
  decode_member(table, old_data, &old_m, &old_len);
  const double old_score = decode_score(table, old_data);

  const uchar *new_m;
  uint new_len;
  decode_member(table, new_data, &new_m, &new_len);
  double score = decode_score(table, new_data);

  const bool same_member =
      old_len == new_len && memcmp(old_m, new_m, new_len) == 0;

  // Tombstone the old entry when the key or the score changes, then write
  // the new version. The old score comes from the record the handler was
  // asked to replace, so no point lookup is needed: an UPDATE always
  // operates on a row that already exists, and a point lookup on flushed
  // data would fall back to a full merged scan per row.
  if (!same_member || old_score != score) {
    share_->lsm_.del(old_score, old_m, old_len);
  }
  share_->lsm_.put(score, new_m, new_len);
  maybe_flush();

  return 0;
}

int ha_zset::delete_row(const uchar *buf) {
  DBUG_TRACE;
  const uchar *m;
  uint len;
  decode_member(table, buf, &m, &len);

  double score;
  if (!share_->lsm_.get(m, len, &score)) {
    return 0;  // already gone
  }

  share_->lsm_.del(score, m, len);
  maybe_flush();

  return 0;
}

int ha_zset::delete_all_rows(void) {
  DBUG_TRACE;
  if (share_->lsm_.clear() != 0) {
    return HA_ERR_CRASHED;
  }

  stats.records = 0;

  return 0;
}

// ============================================================================
// Index access
// ============================================================================

int ha_zset::index_read_map(uchar *buf, const uchar *key,
                            key_part_map keypart_map,
                            enum ha_rkey_function find_flag) {
  DBUG_TRACE;
  if (active_index == 0) {
    // PRIMARY: whole-key point lookup via the engine.
    uint mlen = uint2korr(key);
    const uchar *m = key + 2;
    double score;
    if (!share_->lsm_.get(m, mlen, &score)) {
      return HA_ERR_KEY_NOT_FOUND;
    }

    fill_record(buf, m, mlen, score);
    return 0;
  }

  // idx_score: position the merged cursor at the lower bound.
  double score = float8get(key);
  const uchar *m = nullptr;
  uint len = 0;
  if (keypart_map == HA_WHOLE_KEY) {
    len = uint2korr(key + 8);
    m = key + 8 + 2;
  }

  scan_.seek(&share_->lsm_, score, m, len, scan_sequence_);
  if (!scan_.valid()) {
    return HA_ERR_KEY_NOT_FOUND;
  }

  if (find_flag == HA_READ_KEY_EXACT) {
    if (scan_.key().score != score) {
      return HA_ERR_KEY_NOT_FOUND;
    }

    // For a prefix key (score only) the member is not part of the key.
    if (keypart_map == HA_WHOLE_KEY &&
        (scan_.key().member.size() != len ||
         memcmp(scan_.key().member.data(), m, len) != 0)) {
      return HA_ERR_KEY_NOT_FOUND;
    }
  }

  fill_record(buf, scan_.key());
  scan_.next();
  return 0;
}

int ha_zset::index_next(uchar *buf) {
  DBUG_TRACE;
  if (!scan_.valid()) {
    return HA_ERR_END_OF_FILE;
  }

  fill_record(buf, scan_.key());
  scan_.next();

  return 0;
}

int ha_zset::index_prev(uchar *buf) {
  DBUG_TRACE;
  if (rev_pos_ == static_cast<size_t>(-1)) {
    return HA_ERR_END_OF_FILE;
  }

  fill_record(buf, rev_buf_[rev_pos_]);
  rev_pos_--;

  return 0;
}

int ha_zset::index_first(uchar *buf) {
  DBUG_TRACE;
  scan_.seekToFirst(&share_->lsm_, scan_sequence_);
  if (!scan_.valid()) {
    return HA_ERR_END_OF_FILE;
  }

  fill_record(buf, scan_.key());
  scan_.next();

  return 0;
}

int ha_zset::index_last(uchar *buf) {
  DBUG_TRACE;
  rev_buf_.clear();
  ZsetLSM::Iterator it;

  it.seekToFirst(&share_->lsm_, scan_sequence_);
  while (it.valid()) {
    rev_buf_.push_back(it.key());
    it.next();
  }

  if (rev_buf_.empty()) {
    return HA_ERR_END_OF_FILE;
  }

  rev_pos_ = rev_buf_.size() - 1;
  fill_record(buf, rev_buf_[rev_pos_]);
  rev_pos_--;

  return 0;
}

// ============================================================================
// Table scan
// ============================================================================

int ha_zset::rnd_init(bool) {
  DBUG_TRACE;
  scan_sequence_ = share_->lsm_.sequence();
  scan_.seekToFirst(&share_->lsm_, scan_sequence_);
  return 0;
}

int ha_zset::index_init(uint idx, bool) {
  DBUG_TRACE;
  active_index = idx;
  scan_sequence_ = share_->lsm_.sequence();
  return 0;
}

int ha_zset::rnd_end() {
  DBUG_TRACE;
  return 0;
}

int ha_zset::rnd_next(uchar *buf) {
  DBUG_TRACE;
  if (!scan_.valid()) {
    return HA_ERR_END_OF_FILE;
  }

  fill_record(buf, scan_.key());
  scan_.next();

  return 0;
}

int ha_zset::rnd_pos(uchar *buf, uchar *pos) {
  DBUG_TRACE;
  uint len = uint2korr(pos);
  const uchar *m = pos + 2;
  double score;

  if (!share_->lsm_.get(m, len, &score)) {
    return HA_ERR_KEY_NOT_FOUND;
  }

  fill_record(buf, m, len, score);

  return 0;
}

void ha_zset::position(const uchar *record) {
  DBUG_TRACE;
  const uchar *m;
  uint len;
  decode_member(table, record, &m, &len);
  int2store(ref, len);
  memcpy(ref + 2, m, len);
  ref_length = len + 2;
}

// ============================================================================
// Metadata / locking
// ============================================================================

int ha_zset::info(uint flag) {
  DBUG_TRACE;
  if (share_) {
    stats.records = share_->lsm_.count();
  }
  if (share_) {
    stats.data_file_length = stats.records * 265;
  }

  if (flag & HA_STATUS_ERRKEY) {
    errkey = 0;  // Duplicates can only happen on the PRIMARY KEY.
  }

  return 0;
}

int ha_zset::extra(enum ha_extra_function) {
  DBUG_TRACE;
  return 0;
}

int ha_zset::external_lock(THD *, int) {
  DBUG_TRACE;
  return 0;
}

ha_rows ha_zset::records_in_range(uint inx, key_range *min_key,
                                  key_range *max_key) {
  DBUG_TRACE;
  if (inx == 1 && min_key != nullptr && max_key != nullptr &&
      min_key->key != nullptr && max_key->key != nullptr) {
    double min_s = float8get(min_key->key);
    double max_s = float8get(max_key->key);

    // Count live rows in the score range across the whole LSM.
    ha_rows n = 0;
    ZsetLSM::Iterator it;
    it.seekToFirst(&share_->lsm_, ~0ULL);

    while (it.valid()) {
      if (it.key().score >= min_s && it.key().score <= max_s) {
        n++;
      }
      it.next();
    }

    return n;
  }

  return 10;  // low number to force index usage
}

THR_LOCK_DATA **ha_zset::store_lock(THD *, THR_LOCK_DATA **to,
                                    enum thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && lock_.type == TL_UNLOCK) {
    lock_.type = lock_type;
  }
  *to++ = &lock_;
  return to;
}

// ============================================================================
// Internal functions
// ============================================================================

// Flush the memtable to an sstable once it holds more internal keys than
// the limit.

Zset_share *ha_zset::get_share() {
  Zset_share *tmp_share;

  DBUG_TRACE;

  lock_shared_ha_data();
  if (!(tmp_share = static_cast<Zset_share *>(get_ha_share_ptr()))) {
    tmp_share = new Zset_share;
    if (!tmp_share) {
      goto err;
    }

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();

  return tmp_share;
}

int ha_zset::validate_schema(const TABLE *table) const {
  DBUG_TRACE;
  if (table->s->keys != 2) {
    return HA_ERR_WRONG_COMMAND;
  }

  KEY *key0 = &table->key_info[0];
  KEY *key1 = &table->key_info[1];

  if (key0->user_defined_key_parts != 1 || key1->user_defined_key_parts != 2) {
    return HA_ERR_WRONG_COMMAND;
  }

  if (table->field[1]->type() != MYSQL_TYPE_DOUBLE) {
    return HA_ERR_WRONG_COMMAND;
  }

  return 0;
}

void ha_zset::fill_record(uchar *buf, const ZsetLSM::Key &key) {
  fill_record(buf, reinterpret_cast<const uchar *>(key.member.data()),
              key.member.size(), key.score);
}

void ha_zset::fill_record(uchar *buf, const uchar *member, uint len,
                          double score) {
  // store() asserts that the field is in table->write_set; reads must
  // temporarily mark all columns (same pattern as ha_tina).
  my_bitmap_map *org_bitmap = dbug_tmp_use_all_columns(table, table->write_set);

  // The fields point at table->record[0]; when buf is a different buffer
  // (e.g. REPLACE reads the duplicate row into record[1]), shift them so
  // the row is written directly into buf without clobbering record[0].
  const ptrdiff_t delta = buf - table->record[0];
  if (delta != 0) {
    for (uint i = 0; i < table->s->fields; i++) {
      table->field[i]->move_field_offset(delta);
    }
  }

  table->field[0]->set_notnull();
  table->field[0]->store(reinterpret_cast<const char *>(member), len,
                         &my_charset_bin);
  table->field[1]->set_notnull();
  table->field[1]->store(score);
  if (delta != 0) {
    for (uint i = 0; i < table->s->fields; i++) {
      table->field[i]->move_field_offset(-delta);
    }
  }

  dbug_tmp_restore_column_map(table->write_set, org_bitmap);
}

void ha_zset::maybe_flush() {
  if (share_->lsm_.mem_size() > static_cast<size_t>(zset_memtable_limit)) {
    share_->lsm_.flush();
  }
}

void ha_zset::decode_member(const TABLE *table, const uchar *buf,
                            const uchar **m, uint *len) {
  // The fields point at table->record[0]; shift them so buf is read when
  // it is a different buffer (e.g. update_row's old image in record[1]).
  const ptrdiff_t delta = buf - table->record[0];
  if (delta != 0) {
    for (uint i = 0; i < table->s->fields; i++) {
      table->field[i]->move_field_offset(delta);
    }
  }

  String tmp;
  String *s = table->field[0]->val_str(&tmp);
  *m = pointer_cast<const uchar *>(s->ptr());
  *len = s->length();
  if (delta != 0) {
    for (uint i = 0; i < table->s->fields; i++) {
      table->field[i]->move_field_offset(-delta);
    }
  }
}

double ha_zset::decode_score(const TABLE *table, const uchar *buf) {
  const ptrdiff_t delta = buf - table->record[0];
  if (delta != 0) {
    for (uint i = 0; i < table->s->fields; i++) {
      table->field[i]->move_field_offset(delta);
    }
  }

  double score = table->field[1]->val_real();
  if (delta != 0) {
    for (uint i = 0; i < table->s->fields; i++) {
      table->field[i]->move_field_offset(-delta);
    }
  }
  return score;
}

void ha_zset::decode_index_key(const TABLE *, uint idx, const uchar *key,
                               double *score, const uchar **m, uint *len) {
  if (idx == 1) {
    *score = float8get(key);
    *len = uint2korr(key + 8);
    *m = key + 8 + 2;
  } else {
    *score = 0;
    *len = uint2korr(key);
    *m = key + 2;
  }
}
