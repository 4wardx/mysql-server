disable_missing_profile_warning()
add_definitions(-DMYSQL_SERVER)

if(WITH_ZSET_STORAGE_ENGINE AND NOT WITHOUT_ZSET_STORAGE_ENGINE)
  mysql_add_plugin(
    zset
    ha_zset.cc
    STORAGE_ENGINE
    DEFAULT
    LINK_LIBRARIES
    extra::rapidjson
    ext::zlib)
elseif(NOT WITHOUT_ZSET_STORAGE_ENGINE)
  mysql_add_plugin(
    zset
    ha_zset.cc
    STORAGE_ENGINE
    MODULE_ONLY
    LINK_LIBRARIES
    extra::rapidjson
    ext::zlib)
endif()
