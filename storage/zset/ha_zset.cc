/**
  @file ha_zset.cc

  @brief
  Skeleton implementation of the ZSET storage engine handler.

  @details
  Defines the handler structure and interfaces. The core components are added
  incrementally: in-memory skiplist + hashtable (Redis-style dual structure),
  WAL for crash recovery, sorted SSTable files and multi-level compaction.
*/

#include "storage/zset/ha_zset.h"

#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "typelib.h"

static handler *zset_create_handler(handlerton *hton, TABLE_SHARE *table,
                                    bool partitioned, MEM_ROOT *mem_root);

handlerton *zset_hton;

/* Interface to mysqld, to check system tables supported by SE */
static bool zset_is_supported_system_table(const char *db,
                                           const char *table_name,
                                           bool is_sql_layer_system_table);

Zset_share::Zset_share() { thr_lock_init(&lock); }

static int zset_init_func(void *p) {
  DBUG_TRACE;

  zset_hton = (handlerton *)p;
  zset_hton->state = SHOW_OPTION_YES;
  zset_hton->create = zset_create_handler;
  zset_hton->flags = HTON_CAN_RECREATE;
  zset_hton->is_supported_system_table = zset_is_supported_system_table;

  return 0;
}

static int zset_deinit_func(void *p [[maybe_unused]]) {
  DBUG_TRACE;

  assert(p);

  return 0;
}

Zset_share *ha_zset::get_share() {
  Zset_share *tmp_share;

  DBUG_TRACE;

  lock_shared_ha_data();
  if (!(tmp_share = static_cast<Zset_share *>(get_ha_share_ptr()))) {
    tmp_share = new Zset_share;
    if (!tmp_share) goto err;

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}

static handler *zset_create_handler(handlerton *hton, TABLE_SHARE *table, bool,
                                    MEM_ROOT *mem_root) {
  return new (mem_root) ha_zset(hton, table);
}

ha_zset::ha_zset(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {}

/*
  List of all system tables specific to the SE.
  Array element would look like below,
     { "<database_name>", "<system table name>" },
  The last element MUST be,
     { (const char*)NULL, (const char*)NULL }

  This array is optional, so every SE need not implement it.
*/
static bool zset_is_supported_system_table(const char *, const char *, bool) {
  return false;
}

// ============================================================================
// Lifecycle
// ============================================================================

int ha_zset::create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) {
  DBUG_TRACE;
  return 0;
}

int ha_zset::open(const char *, int, uint, const dd::Table *) {
  DBUG_TRACE;
  if (!(share = get_share())) return HA_ERR_OUT_OF_MEM;
  thr_lock_data_init(&share->lock, &lock, nullptr);
  return 0;
}

int ha_zset::close(void) {
  DBUG_TRACE;
  scan_pos = nullptr;
  return 0;
}

int ha_zset::delete_table(const char *, const dd::Table *) {
  DBUG_TRACE;
  return 0;
}

int ha_zset::rename_table(const char *, const char *, const dd::Table *,
                          dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::truncate(dd::Table *) {
  DBUG_TRACE;
  return 0;
}

// ============================================================================
// DML
// ============================================================================

int ha_zset::write_row(uchar *) {
  DBUG_TRACE;
  return 0;
}

int ha_zset::update_row(const uchar *, uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::delete_row(const uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::delete_all_rows(void) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

// ============================================================================
// Table scan
// ============================================================================

int ha_zset::rnd_init(bool) {
  DBUG_TRACE;
  return 0;
}

int ha_zset::rnd_end() {
  DBUG_TRACE;
  return 0;
}

int ha_zset::rnd_next(uchar *) {
  DBUG_TRACE;
  return HA_ERR_END_OF_FILE;
}

int ha_zset::rnd_pos(uchar *, uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

void ha_zset::position(const uchar *) { DBUG_TRACE; }

// ============================================================================
// Index access
// ============================================================================

int ha_zset::index_read_map(uchar *, const uchar *, key_part_map,
                            enum ha_rkey_function) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::index_next(uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::index_prev(uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::index_first(uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_zset::index_last(uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

// ============================================================================
// Metadata / locking
// ============================================================================

int ha_zset::info(uint) {
  DBUG_TRACE;
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

ha_rows ha_zset::records_in_range(uint, key_range *, key_range *) {
  DBUG_TRACE;
  return 10;  // low number to force index usage
}

THR_LOCK_DATA **ha_zset::store_lock(THD *, THR_LOCK_DATA **to,
                                    enum thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK) lock.type = lock_type;
  *to++ = &lock;
  return to;
}

// ============================================================================
// Schema validation / field encode-decode
// ============================================================================

int ha_zset::validate_schema(const TABLE *) const {
  DBUG_TRACE;
  return 0;
}

// ============================================================================
// Plugin registration
// ============================================================================

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
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    0,       /* flags */
} mysql_declare_plugin_end;
