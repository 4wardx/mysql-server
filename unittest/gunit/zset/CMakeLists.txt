ADD_DEFINITIONS(-DMYSQL_SERVER)

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/storage/zset
)

SET(ZSET_UNIT_SOURCES
  zset-t.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_skiplist.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_hashtable.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_memtable.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_wal.cc
)

MYSQL_ADD_EXECUTABLE(zset-t
  ${ZSET_UNIT_SOURCES}
  ADD_TEST zset
  COMPILE_DEFINITIONS ${DISABLE_PSI_DEFINITIONS}
  ENABLE_EXPORTS
  EXCLUDE_FROM_ALL
  LINK_LIBRARIES sqlgunitlib gunit_small
  SKIP_INSTALL
  ADD_GCOV_TEST zset
)

SET(ZSET_BENCH_SOURCES
  zset-bench.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_skiplist.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_hashtable.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_memtable.cc
  ${CMAKE_SOURCE_DIR}/storage/zset/zset_wal.cc
)

MYSQL_ADD_EXECUTABLE(zset-bench
  ${ZSET_BENCH_SOURCES}
  COMPILE_DEFINITIONS ${DISABLE_PSI_DEFINITIONS}
  ENABLE_EXPORTS
  EXCLUDE_FROM_ALL
  LINK_LIBRARIES sqlgunitlib gunit_small
  SKIP_INSTALL
)
