# /// script
# requires-python = ">=3.11"
# dependencies = ["mcp==1.29.0"]
# ///
"""On-demand MCP access to Computer Watch's persisted samples."""

from contextlib import contextmanager
import os
from pathlib import Path
import sqlite3
import time
from typing import Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError
from mcp.types import ToolAnnotations
from pydantic import Field

mcp = FastMCP("Computer Watch")
READ_ONLY = ToolAnnotations(readOnlyHint=True, openWorldHint=False)
Timestamp = Annotated[int, Field(ge=0, le=2**63 - 1, strict=True)]
SampleLimit = Annotated[int, Field(ge=1, le=300, strict=True)]
SAMPLE_COLUMNS = """
    timestamp_ms, cpu_percent, memory_percent, memory_used, memory_total,
    disk_used, disk_total, disk_read, disk_write, gpu_percent, gpu_memory_used,
    gpu_memory_total, network_down, network_up, cpu_temp, cpu_watts, gpu_temp, gpu_watts
"""


@contextmanager
def read_history():
    local_data = os.environ.get("LOCALAPPDATA")
    if not local_data:
        raise ToolError("未找到 LOCALAPPDATA。请使用运行 Computer Watch 的 Windows 用户连接。")
    path = Path(local_data) / "ComputerWatch" / "history.db"
    if not path.is_file():
        raise ToolError("未找到历史数据库。请先启动 Computer Watch，等待它保存采样。")
    try:
        db = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=0.5)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA trusted_schema=OFF")
            db.execute("BEGIN")
            yield db
        finally:
            db.close()
    except sqlite3.Error as error:
        raise ToolError(f"读取 Computer Watch 历史失败（{error.sqlite_errorname}）。") from error


@mcp.tool(annotations=READ_ONLY)
def get_snapshot(at_ms: Timestamp | None = None) -> dict[str, Any]:
    """读取最新落盘快照，或 at_ms（Unix 毫秒）当时及以前的最近快照。

    包含 CPU 和内存各自保存的前 10 名，place 从 1 开始。
    容量为字节，速率为字节/秒，温度为摄氏度，功耗为瓦，百分比为 0–100。
    null 表示缺失。age_ms 表示快照距当前时间的毫秒数。
    """
    with read_history() as db:
        row = db.execute(
            f"""SELECT {SAMPLE_COLUMNS} FROM samples
                WHERE timestamp_ms <= ? ORDER BY timestamp_ms DESC LIMIT 1""",
            (at_ms if at_ms is not None else 2**63 - 1,),
        ).fetchone()
        if row is None:
            return {"sample": None, "age_ms": None, "by_cpu": [], "by_memory": []}
        ranks = db.execute(
            """SELECT sort_kind, place + 1 AS place, pid, name,
                      cpu AS cpu_percent, memory AS memory_bytes
               FROM rankings WHERE timestamp_ms = ? AND sort_kind IN (0, 1)
               ORDER BY sort_kind, place""",
            (row["timestamp_ms"],),
        )
        by_cpu, by_memory = [], []
        for rank in ranks:
            entry = dict(rank)
            (by_cpu if entry.pop("sort_kind") == 0 else by_memory).append(entry)
        return {
            "sample": dict(row),
            "age_ms": time.time_ns() // 1_000_000 - row["timestamp_ms"],
            "by_cpu": by_cpu,
            "by_memory": by_memory,
        }


@mcp.tool(annotations=READ_ONLY)
def get_history(from_ms: Timestamp, to_ms: Timestamp, limit: SampleLimit = 120) -> dict[str, Any]:
    """按时间升序读取闭区间内的落盘采样，时间使用 Unix 毫秒。

    单位和缺失值与 get_snapshot 一致。继续翻页时将 from_ms 设为 next_from_ms，
    保留原来的 to_ms。next_from_ms 为 null 时已读完。
    """
    if from_ms > to_ms:
        raise ToolError("from_ms 必须小于或等于 to_ms。")
    with read_history() as db:
        rows = db.execute(
            f"""SELECT {SAMPLE_COLUMNS} FROM samples
                WHERE timestamp_ms BETWEEN ? AND ?
                ORDER BY timestamp_ms LIMIT ?""",
            (from_ms, to_ms, limit + 1),
        ).fetchall()
        return {
            "samples": [dict(row) for row in rows[:limit]],
            "next_from_ms": rows[limit - 1]["timestamp_ms"] + 1 if len(rows) > limit else None,
        }


if __name__ == "__main__":
    mcp.run(transport="stdio")
