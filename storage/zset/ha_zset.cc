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
#include "mysql/plugin.h"
#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "sql/table.h"
#include "typelib.h"

handlerton *zset_hton;

// WAL fsync policy, defined in zset_wal.cc.
extern ulong zset_wal_fsync;

// Table file extensions, for DROP/repair discovery.
static const char *zset_file_exts[] = {".zlog", nullptr};

// Build the <table>.zlog path from the table name.
static void make_wal_path(char *buf, size_t buflen, const char *name) {
  fn_format(buf, name, "", ".zlog", MY_REPLACE_EXT | MY_UNPACK_FILENAME);
}

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

static SYS_VAR *zset_system_variables[] = {MYSQL_SYSVAR(wal_fsync), nullptr};

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
    : handler(hton, table_arg), share_(nullptr), scan_pos_(nullptr) {}

// ============================================================================
// Lifecycle
// ============================================================================

int ha_zset::create(const char *name, TABLE *form, HA_CREATE_INFO *,
                    dd::Table *) {
  int rc = validate_schema(form);
  if (rc) {
    return rc;
  }

  char path[FN_REFLEN];
  make_wal_path(path, sizeof(path), name);
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
  scan_pos_ = nullptr;

  if (!share_->replayed_) {
    char path[FN_REFLEN];
    make_wal_path(path, sizeof(path), name);

    int rc = share_->wal_.open(path);
    if (rc) {
      return HA_ERR_CRASHED;
    }

    share_->wal_.replay(&share_->mem_, &share_->seq_);
    share_->replayed_ = true;
  }

  return 0;
}

int ha_zset::close(void) {
  DBUG_TRACE;
  scan_pos_ = nullptr;
  return 0;
}

int ha_zset::delete_table(const char *name, const dd::Table *) {
  DBUG_TRACE;
  char path[FN_REFLEN];
  make_wal_path(path, sizeof(path), name);

  if (my_delete(path, MYF(0))) {
    return HA_ERR_CRASHED;
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

  // Persist a CLEAR marker first, then drop the in-memory state.
  int rc = share_->wal_.append_clear();
  if (rc) {
    return rc;
  }
  share_->mem_.clear();
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
  if (share_->mem_.get(m, len, &old_score)) {
    return HA_ERR_FOUND_DUPP_KEY;
  }

  // Write-ahead: persist the PUT record first, then apply to the memtable.
  const uint64 seq = ++share_->seq_;
  if (share_->wal_.append(score, m, len, seq, Zset_wal::Type::kPut)) {
    return HA_ERR_CRASHED;
  }

  share_->mem_.put(score, m, len, seq);
  stats.records = share_->mem_.count();

  return 0;
}

int ha_zset::update_row(const uchar *old_data, uchar *new_data) {
  DBUG_TRACE;
  const uchar *old_m;
  uint old_len;
  decode_member(table, old_data, &old_m, &old_len);

  const uchar *new_m;
  uint new_len;
  decode_member(table, new_data, &new_m, &new_len);
  double score = decode_score(table, new_data);

  double old_score;
  const bool member_exists = share_->mem_.get(old_m, old_len, &old_score);
  const bool same_member =
      old_len == new_len && memcmp(old_m, new_m, new_len) == 0;

  // Write-ahead first. Tombstone the old entry when the score changes.
  const uint64 seq = ++share_->seq_;
  if (member_exists && !same_member) {
    if (share_->wal_.append(old_score, old_m, old_len, seq,
                            Zset_wal::Type::kDelete)) {
      return HA_ERR_CRASHED;
    }
    share_->mem_.tombstone(old_score, old_m, old_len, seq);
  } else if (member_exists && old_score != score) {
    if (share_->wal_.append(old_score, old_m, old_len, seq,
                            Zset_wal::Type::kDelete)) {
      return HA_ERR_CRASHED;
    }
    share_->mem_.tombstone(old_score, old_m, old_len, seq);
  }

  if (share_->wal_.append(score, new_m, new_len, seq, Zset_wal::Type::kPut)) {
    return HA_ERR_CRASHED;
  }

  share_->mem_.put(score, new_m, new_len, seq);
  stats.records = share_->mem_.count();

  return 0;
}

int ha_zset::delete_row(const uchar *buf) {
  DBUG_TRACE;
  const uchar *m;
  uint len;
  decode_member(table, buf, &m, &len);

  double score;
  if (!share_->mem_.get(m, len, &score)) {
    return 0;  // already gone
  }

  // Write-ahead: persist the DELETE tombstone first, then apply.
  const uint64 seq = ++share_->seq_;
  if (share_->wal_.append(score, m, len, seq, Zset_wal::Type::kDelete)) {
    return HA_ERR_CRASHED;
  }
  share_->mem_.tombstone(score, m, len, seq);
  stats.records = share_->mem_.count();

  return 0;
}

int ha_zset::delete_all_rows(void) {
  DBUG_TRACE;

  int rc = share_->wal_.append_clear();
  if (rc) {
    return rc;
  }

  share_->mem_.clear();
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
    // PRIMARY: whole-key point lookup via the hash table.
    uint mlen = uint2korr(key);
    const uchar *m = key + 2;
    ZNode *node = share_->mem_.lookup(m, mlen);

    if (node == nullptr) {
      return HA_ERR_KEY_NOT_FOUND;
    }

    fill_record(buf, node);
    scan_pos_ = share_->mem_.nextLive(node);

    return 0;
  }

  // idx_score: position the cursor at the lower bound.
  double score = float8get(key);
  const uchar *m = nullptr;
  uint len = 0;
  if (keypart_map == HA_WHOLE_KEY) {
    len = uint2korr(key + 8);
    m = key + 8 + 2;
  }

  scan_pos_ = share_->mem_.seekLive(score, m, len);
  if (scan_pos_ == nullptr) {
    return HA_ERR_KEY_NOT_FOUND;
  }

  if (find_flag == HA_READ_KEY_EXACT) {
    if (scan_pos_->score != score) {
      scan_pos_ = nullptr;

      return HA_ERR_KEY_NOT_FOUND;
    }
    // For a prefix key (score only) the member is not part of the key.
    if (keypart_map == HA_WHOLE_KEY &&
        (scan_pos_->member_len != len ||
         memcmp(scan_pos_->member, m, len) != 0)) {
      scan_pos_ = nullptr;

      return HA_ERR_KEY_NOT_FOUND;
    }
  }

  fill_record(buf, scan_pos_);
  scan_pos_ = share_->mem_.nextLive(scan_pos_);

  return 0;
}

int ha_zset::index_next(uchar *buf) {
  DBUG_TRACE;
  if (scan_pos_ == nullptr) {
    return HA_ERR_END_OF_FILE;
  }

  fill_record(buf, scan_pos_);
  scan_pos_ = share_->mem_.nextLive(scan_pos_);

  return 0;
}

int ha_zset::index_prev(uchar *buf) {
  DBUG_TRACE;
  if (scan_pos_ == nullptr) {
    return HA_ERR_END_OF_FILE;
  }

  fill_record(buf, scan_pos_);
  scan_pos_ = share_->mem_.prevLive(scan_pos_);

  return 0;
}

int ha_zset::index_first(uchar *buf) {
  DBUG_TRACE;
  scan_pos_ = share_->mem_.firstLive();
  if (scan_pos_ == nullptr) {
    return HA_ERR_END_OF_FILE;
  }
  fill_record(buf, scan_pos_);
  scan_pos_ = share_->mem_.nextLive(scan_pos_);

  return 0;
}

int ha_zset::index_last(uchar *buf) {
  DBUG_TRACE;
  scan_pos_ = share_->mem_.lastLive();
  if (scan_pos_ == nullptr) {
    return HA_ERR_END_OF_FILE;
  }
  fill_record(buf, scan_pos_);
  scan_pos_ = share_->mem_.prevLive(scan_pos_);

  return 0;
}

// ============================================================================
// Table scan
// ============================================================================

int ha_zset::rnd_init(bool) {
  DBUG_TRACE;
  scan_pos_ = share_->mem_.firstLive();
  return 0;
}

int ha_zset::rnd_end() {
  DBUG_TRACE;
  return 0;
}

int ha_zset::rnd_next(uchar *buf) {
  DBUG_TRACE;
  if (scan_pos_ == nullptr) {
    return HA_ERR_END_OF_FILE;
  }

  fill_record(buf, scan_pos_);
  scan_pos_ = share_->mem_.nextLive(scan_pos_);

  return 0;
}

int ha_zset::rnd_pos(uchar *buf, uchar *pos) {
  DBUG_TRACE;
  uint len = uint2korr(pos);
  const uchar *m = pos + 2;
  ZNode *node = share_->mem_.lookup(m, len);

  if (node == nullptr) {
    return HA_ERR_KEY_NOT_FOUND;
  }

  fill_record(buf, node);

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
    stats.records = share_->mem_.count();
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

    return static_cast<ha_rows>(share_->mem_.countLiveInRange(min_s, max_s));
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

void ha_zset::fill_record(uchar *buf, ZNode *node) {
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
  table->field[0]->store(reinterpret_cast<const char *>(node->member),
                         node->member_len, &my_charset_bin);
  table->field[1]->set_notnull();
  table->field[1]->store(node->score);
  if (delta != 0) {
    for (uint i = 0; i < table->s->fields; i++) {
      table->field[i]->move_field_offset(-delta);
    }
  }

  dbug_tmp_restore_column_map(table->write_set, org_bitmap);
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
