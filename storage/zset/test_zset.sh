#!/bin/bash
# ZSET storage engine end-to-end test.
#
# Initializes a scratch data directory, starts mysqld with the ZSET
# plugin, creates a ZSET table and runs a set of SQL checks against it.
# Exits 0 if all checks pass, 1 otherwise.
#
# Usage: test_zset.sh [--port=33306] [--keep]
#   --keep   do not shut down mysqld after the test.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/bin"
PORT=33306
DATADIR=/tmp/zset-test-db
SOCK=/tmp/zset-test.sock
PLUGIN="$ROOT/build/plugin_output_directory/ha_zset.so"
KEEP=0

# Parse options.
for arg in "$@"; do
	case "$arg" in
	--port=*) PORT="${arg#*=}" ;;
	--keep) KEEP=1 ;;
	*)
		echo "unknown option: $arg" >&2
		exit 2
		;;
	esac
done

failures=0

# Run a SQL command; report any error.
sql() {
	local desc="$1"
	shift
	"$BIN/mysql" --socket="$SOCK" -uroot --binary-as-hex=0 "$@" \
		</dev/null 2>&1 || {
		echo "FAIL: $desc" >&2
		failures=$((failures + 1))
	}
}

check() {
	local desc="$1"
	shift
	local expected="$1"
	shift
	local got
	got="$("$BIN/mysql" --socket="$SOCK" -uroot --batch --skip-column-names "$@" 2>&1)"
	if [ "$got" != "$expected" ]; then
		echo "FAIL: $desc" >&2
		echo "  expected: $expected" >&2
		echo "  got:      $got" >&2
		failures=$((failures + 1))
	fi
}

echo "== ZSET engine test =="
echo "build dir: $ROOT/build   port: $PORT"

# 1. Initialize a scratch data directory.
rm -rf "$DATADIR"
if ! "$BIN/mysqld" --initialize-insecure --datadir="$DATADIR" >/tmp/zset-test-init.log 2>&1; then
	echo "FAIL: mysqld --initialize-insecure" >&2
	exit 1
fi

# 2. Start mysqld with the ZSET plugin. Socket-only, so no TCP port conflict.
"$BIN/mysqld" --datadir="$DATADIR" --socket="$SOCK" --port="$PORT" \
	--skip-networking --mysqlx=0 \
	--plugin-dir="$ROOT/build/plugin_output_directory" \
	--plugin-load-add=ha_zset.so \
	>/tmp/zset-test-mysqld.log 2>&1 &
MYSQLD_PID=$!

# Wait for the server to accept connections.
for _ in $(seq 1 60); do
	if "$BIN/mysqladmin" --socket="$SOCK" -uroot ping >/dev/null 2>&1; then
		break
	fi
	sleep 0.5
done

if ! "$BIN/mysqladmin" --socket="$SOCK" -uroot ping >/dev/null 2>&1; then
	echo "FAIL: mysqld did not start (see /tmp/zset-test-mysqld.log)" >&2
	exit 1
fi

echo "-- server started (pid $MYSQLD_PID) --"

# 3. Check the engine is registered.
check "SHOW ENGINES shows ZSET" "ZSET" \
	-e "SELECT ENGINE FROM information_schema.ENGINES WHERE ENGINE='ZSET';"

# 4. Create the ZSET table.
sql "CREATE DATABASE ztest" -e "CREATE DATABASE ztest;"
sql "CREATE TABLE ztest.t" -e \
	"CREATE TABLE ztest.t (member VARBINARY(255) NOT NULL, score DOUBLE NOT NULL, PRIMARY KEY(member), KEY idx_score(score, member)) ENGINE=ZSET;"

# 5. Insert rows and check ordering / lookups.
sql "INSERT rows" -e \
	"INSERT INTO ztest.t VALUES ('apple',3),('banana',1),('cherry',1),('egg',1),('durian',2.5);"

check "row count" "5" -e "SELECT COUNT(*) FROM ztest.t;"
check "ordered scan (ORDER BY score,member)" $'banana\ncherry\negg\ndurian\napple' \
	-e "SELECT member FROM ztest.t ORDER BY score, member;"
check "PK point lookup (ZSCORE)" "3" -e "SELECT score FROM ztest.t WHERE member='apple';"
check "score range (ZRANGEBYSCORE)" $'banana\ncherry\negg' \
	-e "SELECT member FROM ztest.t WHERE score BETWEEN 1 AND 1 ORDER BY score, member;"
check "ORDER BY score DESC (ZREVRANGE)" "apple" -e "SELECT member FROM ztest.t ORDER BY score DESC LIMIT 1;"

# 6. Update and delete.
sql "UPDATE score" -e "UPDATE ztest.t SET score=0.5 WHERE member='apple';"
check "after update" "0.5" -e "SELECT score FROM ztest.t WHERE member='apple';"
sql "DELETE row" -e "DELETE FROM ztest.t WHERE member='banana';"
check "after delete" "4" -e "SELECT COUNT(*) FROM ztest.t;"

# 7. Duplicate key must be rejected.
"$BIN/mysql" --socket="$SOCK" -uroot -e "INSERT INTO ztest.t VALUES ('cherry',9);" >/dev/null 2>&1
if [ $? -eq 0 ]; then
	echo "FAIL: duplicate member should be rejected" >&2
	failures=$((failures + 1))
fi

# 8. Illegal schema must be rejected.
"$BIN/mysql" --socket="$SOCK" -uroot -e "CREATE TABLE ztest.bad (a INT) ENGINE=ZSET;" >/dev/null 2>&1
if [ $? -eq 0 ]; then
	echo "FAIL: illegal ZSET schema should be rejected" >&2
	failures=$((failures + 1))
fi

# 9. Truncate / drop.
sql "TRUNCATE" -e "TRUNCATE TABLE ztest.t;"
check "after truncate" "0" -e "SELECT COUNT(*) FROM ztest.t;"
sql "DROP TABLE" -e "DROP TABLE ztest.t;"

# Cleanup.
if [ "$KEEP" -eq 0 ]; then
	"$BIN/mysqladmin" --socket="$SOCK" -uroot shutdown >/dev/null 2>&1
	wait "$MYSQLD_PID" 2>/dev/null
	rm -rf "$DATADIR"
	echo "-- server shut down --"
else
	echo "-- keeping server running (pid $MYSQLD_PID) --"
fi

if [ "$failures" -eq 0 ]; then
	echo "All ZSET engine tests passed."
	exit 0
fi
echo "$failures ZSET engine test(s) failed."
exit 1
