#!/usr/bin/env python3
# ZSET storage engine benchmark: identical workloads on ZSET vs InnoDB.
#
# Workloads covered:
#   * bulk INSERT (recursive-CTE, single statement)
#   * batched INSERT (multi-row VALUES, single vs auto-commit)
#   * sequential INSERT (continuous writes in chunks)
#   * point lookups by member (PK) - hit and miss
#   * score range scan, descending top-N scan, full count
#   * UPDATE (score change), DELETE
#   * duplicate-key rejection
#
# Fairness:
#   * The same schema, same SQL and the same server are used for both
#     engines, and each insert workload starts from a fresh table.
#   * Durability is aligned: by default both engines run WITHOUT fsync
#     (InnoDB innodb_flush_log_at_trx_commit=0, ZSET zset_wal_fsync=0).
#     Pass --durable to fsync on BOTH engines (ZSET fsyncs every write,
#     InnoDB every commit).
#   * Table setup (DROP/CREATE) happens before timing, not inside it.
#   * Lookups are batched into one IN(...) query so the mysql CLI
#     startup cost is amortized away.
#
# Usage: bench_zset.py [--port=33306] [--keep] [--total=N] [--batch=N]
#                      [--durable]
#   --total=N   number of rows for every workload (required)
#   --batch=N   rows per INSERT VALUES statement (default 10000)
#   --durable   fsync per commit/write on BOTH engines

import argparse
import os
import shutil
import subprocess
import sys
import time

try:
    import tabulate as _tab_mod

    _TABULATE = True
except ImportError:
    _tab_mod = None  # type: ignore[assignment]
    _TABULATE = False


def render_table(report, header):
    """Render the report; use tabulate when available, else a plain table."""
    if _TABULATE:
        assert _tab_mod is not None
        return _tab_mod.tabulate(
            report, headers=header, tablefmt="simple_outline", stralign="left"
        )
    w0 = max(len(h) for h in header)
    col = max(len(str(r[0])) + 4 for r in report)
    out = [
        "  ".join(h.ljust(w0) if i == 0 else h.ljust(col) for i, h in enumerate(header))
    ]
    out.append("-" * (w0 + 2 * col))
    for row in report:
        out.append(
            str(row[0]).ljust(w0)
            + "  "
            + str(row[1]).ljust(col)
            + "  "
            + str(row[2]).ljust(col)
        )
    return "\n".join(out)


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BIN = os.path.join(ROOT, "build", "bin")


class Engine:
    ZSET = "ZSET"
    INNODB = "InnoDB"


def mysql_cmd(socket, sql, timed=True):
    """Run one SQL blob via stdin; return (elapsed_seconds, rc, stderr)."""
    s = time.monotonic()
    r = subprocess.run(
        [os.path.join(BIN, "mysql"), "--socket=" + socket, "-uroot"],
        input=sql,
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    dt = time.monotonic() - s if timed else 0.0
    return dt, r.returncode, r.stderr.strip()


def fmt_rate(n, seconds):
    return "%.1fK/s" % (n / seconds / 1000.0)


def fmt_ms(seconds):
    return "%.3fms" % (seconds * 1000.0)


def run_workload(label, fn, engines, report):
    """Run fn(engine) for each engine and record a row in the report."""
    print("  [..] " + label, flush=True)
    report.append([label])
    for eng in engines:
        result = fn(eng)
        report[-1].append(result)
    print("  [ok] " + label, flush=True)


def table_sql(engine):
    return (
        "CREATE TABLE bench.t (member VARBINARY(255) NOT NULL, "
        "score DOUBLE NOT NULL, PRIMARY KEY(member), "
        f"KEY idx_score(score,member)) ENGINE={engine};"
    )


def setup_table(socket, engine):
    """DROP + CREATE the table. Untimed: setup must not skew the result."""
    dt, rc, err = mysql_cmd(
        socket, "DROP TABLE IF EXISTS bench.t; " + table_sql(engine)
    )
    return rc, err


def bench_bulk_insert(socket, n, ct_depth):
    def fn(eng):
        rc, err = setup_table(socket, eng)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        sql = (
            f"SET SESSION cte_max_recursion_depth={ct_depth}; "
            f"INSERT INTO bench.t (member,score) WITH RECURSIVE s(n) AS "
            f"(SELECT 1 UNION ALL SELECT n+1 FROM s WHERE n<{n}) "
            f"SELECT CONCAT('m_',LPAD(n,7,'0')), n FROM s;"
        )
        dt, rc, err = mysql_cmd(socket, sql)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        return fmt_rate(n, dt) + "  (" + fmt_ms(dt) + ")"

    return fn


def bench_batch_insert(socket, n, batch, in_tx):
    def fn(eng):
        rc, err = setup_table(socket, eng)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        values = []
        for i in range(n):
            values.append(f"('m_{i:07d}',{i})")
        body = []
        for i in range(0, n, batch):
            chunk = ",".join(values[i : i + batch])
            body.append(f"INSERT INTO bench.t VALUES {chunk};")
        sql = (
            ("START TRANSACTION;\n" if in_tx else "")
            + "\n".join(body)
            + ("\nCOMMIT;" if in_tx else "")
        )
        dt, rc, err = mysql_cmd(socket, sql)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        return fmt_rate(n, dt) + "  (" + fmt_ms(dt) + ")"

    return fn


def bench_sequential_write(socket, n, chunk, ct_depth):
    def fn(eng):
        rc, err = setup_table(socket, eng)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        sql = []
        for lo in range(0, n, chunk):
            hi = lo + chunk
            sql.append(
                f"SET SESSION cte_max_recursion_depth={hi + 10}; "
                f"INSERT INTO bench.t (member,score) WITH RECURSIVE "
                f"s(k) AS (SELECT 1 UNION ALL SELECT k+1 FROM s WHERE k<{hi}) "
                f"SELECT CONCAT('m_',LPAD(k,7,'0')), k FROM s "
                f"WHERE k>{lo};"
            )
        dt, rc, err = mysql_cmd(socket, "\n".join(sql))
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        return fmt_rate(n, dt) + "  (" + fmt_ms(dt) + ")"

    return fn


def bench_point_lookup(socket, n_rows, n_queries, hit=True):
    def fn(eng):
        members = []
        for i in range(n_queries):
            idx = i * 997 % n_rows if hit else n_rows + i
            members.append(f"'m_{idx:07d}'")
        sql = "SELECT COUNT(*) FROM bench.t " f"WHERE member IN ({','.join(members)});"
        dt, rc, err = mysql_cmd(socket, sql)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        return fmt_rate(n_queries, dt) + "  (" + fmt_ms(dt) + ")"

    return fn


def bench_scan(socket, kind):
    def fn(eng):
        if kind == "range":
            sql = "SELECT COUNT(*) FROM bench.t " "WHERE score BETWEEN 10000 AND 20000;"
        elif kind == "desc_top":
            sql = (
                "SELECT COUNT(*) FROM (SELECT member FROM bench.t "
                "ORDER BY score DESC LIMIT 10) x;"
            )
        elif kind == "full":
            sql = "SELECT COUNT(*) FROM bench.t;"
        else:
            raise ValueError(kind)
        dt, rc, err = mysql_cmd(socket, sql)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        return fmt_ms(dt)

    return fn


def bench_update(socket, n):
    # Single transaction: one commit instead of a per-row fsync.
    def fn(eng):
        sql = "START TRANSACTION; UPDATE bench.t SET score = score + 0.5; COMMIT;"
        dt, rc, err = mysql_cmd(socket, sql)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        return fmt_rate(n, dt) + "  (" + fmt_ms(dt) + ")"

    return fn


def bench_delete(socket, n):
    def fn(eng):
        sql = (
            f"START TRANSACTION; DELETE FROM bench.t WHERE score < {n // 2}; " "COMMIT;"
        )
        dt, rc, err = mysql_cmd(socket, sql)
        if rc != 0:
            return "FAIL: " + err.splitlines()[-1]
        return fmt_rate(n // 2, dt) + "  (" + fmt_ms(dt) + ")"

    return fn


def bench_dup_reject(socket, n):
    # The DELETE workload removed score < n/2, so probe a member that
    # survived it (score n/2 + 3) and is still a duplicate.
    member = "m_%07d" % (n // 2 + 3)

    def fn(eng):
        sql = f"INSERT INTO bench.t VALUES ('{member}', -1);"
        dt, rc, err = mysql_cmd(socket, sql)
        if rc != 0 and "1062" in err:
            return "rejected (" + fmt_ms(dt) + ")"
        return "NOT REJECTED"

    return fn


def load_for_reads(socket, n, ct_depth):
    def fn(eng):
        rc, err = setup_table(socket, eng)
        if rc != 0:
            return err
        sql = (
            f"SET SESSION cte_max_recursion_depth={ct_depth}; "
            f"INSERT INTO bench.t (member,score) WITH RECURSIVE s(n) AS "
            f"(SELECT 1 UNION ALL SELECT n+1 FROM s WHERE n<{n}) "
            f"SELECT CONCAT('m_',LPAD(n,7,'0')), n FROM s;"
        )
        return mysql_cmd(socket, sql)

    return fn


def main():
    ap = argparse.ArgumentParser(description="ZSET vs InnoDB benchmark")
    ap.add_argument("--port", type=int, default=33307)
    ap.add_argument("--socket", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument(
        "--total", type=int, required=True, help="number of rows for every workload"
    )
    ap.add_argument(
        "--batch",
        type=int,
        default=10000,
        help="rows per INSERT VALUES statement (default 10000)",
    )
    ap.add_argument(
        "--durable", action="store_true", help="fsync per commit/write on BOTH engines"
    )
    args = ap.parse_args()

    if args.total <= 0 or args.batch <= 0:
        print("FAIL: --total and --batch must be positive")
        sys.exit(1)

    if args.socket:
        sock = args.socket
        ours = False
        datadir = None
    else:
        sock = "/tmp/zset-bench.sock"
        datadir = "/tmp/zset-bench-db"
        shutil.rmtree(datadir, ignore_errors=True)
        ours = True

        init = subprocess.run(
            [
                os.path.join(BIN, "mysqld"),
                "--initialize-insecure",
                "--datadir=" + datadir,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if init.returncode != 0:
            print("FAIL: mysqld --initialize-insecure")
            sys.exit(1)

        subprocess.Popen(
            [
                os.path.join(BIN, "mysqld"),
                "--datadir=" + datadir,
                "--socket=" + sock,
                "--port=" + str(args.port),
                "--skip-networking",
                "--mysqlx=0",
                "--plugin-dir="
                + os.path.join(ROOT, "build", "plugin_output_directory"),
                "--plugin-load-add=ha_zset.so",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        ok = False
        for _ in range(60):
            time.sleep(0.5)
            r = subprocess.run(
                [os.path.join(BIN, "mysqladmin"), "--socket=" + sock, "-uroot", "ping"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if r.returncode == 0:
                ok = True
                break
        if not ok:
            print("FAIL: mysqld did not start")
            sys.exit(1)

    n = args.total
    ct_depth = max(2 * n, 200000)

    # Align durability so the comparison is apples-to-apples.
    if args.durable:
        # Both engines fsync.
        durability = "fsync on BOTH (ZSET per write, InnoDB per commit)"
        pre = (
            "SET GLOBAL zset_wal_fsync=1; "
            "SET GLOBAL innodb_flush_log_at_trx_commit=1; "
            "SET GLOBAL sync_binlog=1; "
        )
    else:
        # Neither engine fsyncs per write/commit: raw engine throughput.
        durability = "fsync off on BOTH (no per-write/per-commit fsync)"
        pre = (
            "SET GLOBAL zset_wal_fsync=0; "
            "SET GLOBAL innodb_flush_log_at_trx_commit=0; "
            "SET GLOBAL sync_binlog=0; "
        )

    dt, rc, err = mysql_cmd(
        sock, pre + "DROP DATABASE IF EXISTS bench; CREATE DATABASE bench;"
    )
    if rc != 0:
        print("FAIL: setup:", err)
        sys.exit(1)

    engines = [Engine.ZSET, Engine.INNODB]
    report = []
    header = ["Workload", "ZSET", "InnoDB"]

    print("== ZSET vs InnoDB benchmark ==")
    print(f"rows: {n}   batch: {args.batch}")
    print("durability: " + durability)
    print()

    run_workload(
        f"bulk INSERT {n} (single stmt)",
        bench_bulk_insert(sock, n, ct_depth),
        engines,
        report,
    )
    for tx in (False, True):
        run_workload(
            f"batch INSERT {n} ({'one tx' if tx else 'auto-commit'}, {args.batch}-row stmts)",
            bench_batch_insert(sock, n, args.batch, tx),
            engines,
            report,
        )
    run_workload(
        f"sequential INSERT {n} (5 chunks, {n // 5}-row stmts)",
        bench_sequential_write(sock, n, n // 5, ct_depth),
        engines,
        report,
    )

    # Reload a fixed table for the read / update / delete workloads.
    for eng in engines:
        dt, rc, err = load_for_reads(sock, n, ct_depth)(eng)
        if rc != 0:
            print(f"FAIL: loading {eng}:", err.splitlines()[-1])

    run_workload(
        "point lookup x2000 (hit)",
        bench_point_lookup(sock, n, 2000, hit=True),
        engines,
        report,
    )
    run_workload(
        "point lookup x2000 (miss)",
        bench_point_lookup(sock, n, 2000, hit=False),
        engines,
        report,
    )
    run_workload(
        "score range scan (10k..20k)", bench_scan(sock, "range"), engines, report
    )
    run_workload("desc top-10 scan", bench_scan(sock, "desc_top"), engines, report)
    run_workload("full count scan", bench_scan(sock, "full"), engines, report)
    run_workload(f"UPDATE {n} rows", bench_update(sock, n), engines, report)

    # Reload (deletes shrink the table).
    for eng in engines:
        dt, rc, err = load_for_reads(sock, n, ct_depth)(eng)
        if rc != 0:
            print(f"FAIL: reloading {eng}:", err.splitlines()[-1])
    run_workload(f"DELETE {n} rows", bench_delete(sock, n), engines, report)
    run_workload("duplicate-key rejection", bench_dup_reject(sock, n), engines, report)

    print(render_table(report, header))

    if ours and not args.keep and datadir is not None:
        subprocess.run(
            [
                os.path.join(BIN, "mysqladmin"),
                "--socket=" + sock,
                "-uroot",
                "shutdown",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        shutil.rmtree(datadir, ignore_errors=True)


if __name__ == "__main__":
    main()
