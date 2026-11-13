# /// script
# requires-python = ">=3.11"
# dependencies = ["mcp==1.29.0"]
# ///
"""On-demand MCP access to Computer Watch's persisted samples."""

from contextlib import contextmanager
import ctypes
import json
import ntpath
from datetime import datetime
import os
from pathlib import Path
import sqlite3
import time
from typing import Annotated, Any, Literal

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


Section = Literal["all", "inventory", "cpu", "gpu", "memory", "disk", "network", "board", "displays", "settings"]
TrafficPeriod = Literal["session", "today", "week", "custom"]
TrafficSort = Literal["internet", "lan", "other", "total"]
Offset = Annotated[int, Field(ge=0, le=1000000, strict=True)]
LiveLimit = Annotated[int, Field(ge=1, le=61, strict=True)]


def dashboard_path() -> Path:
    root = os.environ.get("LOCALAPPDATA")
    if not root:
        raise ToolError("未找到 LOCALAPPDATA。请使用 Computer Watch 所在 Windows 用户。")
    return Path(root) / "ComputerWatch" / "dashboard.json"


def read_dashboard() -> dict[str, Any]:
    path = dashboard_path()
    try:
        with path.open("rb") as stream:
            payload = stream.read(16 * 1024 * 1024 + 1)
        if len(payload) > 16 * 1024 * 1024:
            raise ValueError("export too large")
        data = json.loads(payload, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
        if data.get("schema_version") != 1 or not isinstance(data.get("generated_at_ms"), int):
            raise ValueError("unsupported export")
        return data
    except FileNotFoundError as error:
        raise ToolError("尚无详情缓存。请启动新版 Computer Watch，再调用 refresh_dashboard。") from error
    except (OSError, ValueError, AttributeError) as error:
        raise ToolError("无法读取详情缓存，请调用 refresh_dashboard 重新生成。") from error


def signal_collector(name: str) -> None:
    if os.name != "nt":
        raise ToolError("按需刷新只能在运行 Computer Watch 的 Windows 用户会话中使用。")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
    kernel.OpenEventW.restype = ctypes.c_void_p
    kernel.SetEvent.argtypes = [ctypes.c_void_p]
    kernel.SetEvent.restype = ctypes.c_int
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenEventW(2, False, "Local\\ComputerWatch.Mcp." + name + ".1")
    if not handle:
        raise ToolError("未找到后台刷新通道。请启动新版 Computer Watch，并使用相同 Windows 用户会话。")
    try:
        if not kernel.SetEvent(handle):
            raise ToolError("无法通知后台刷新。")
    finally:
        kernel.CloseHandle(handle)


@mcp.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
def refresh_dashboard(start_traffic: bool = False) -> dict[str, Any]:
    """按需刷新所有界面数据，无需打开或切换面板。最长等待 25 秒。

    触发现有后台采集，不修改设置，不自动提权。硬件助手未连接时返回缺失状态。
    start_traffic=True 明确开启本次运行的出站流量采集，后台持续到退出程序。
    默认不启动出站采集。刷新后使用 get_hardware_details/get_realtime/get_traffic 查询。
    """
    requested_at = time.time_ns() // 1_000_000
    if start_traffic:
        signal_collector("StartTraffic")
    signal_collector("Refresh")
    deadline = time.monotonic() + 25
    latest = None
    while time.monotonic() < deadline:
        try:
            candidate = read_dashboard()
            if candidate["generated_at_ms"] >= requested_at:
                latest = candidate
                if not candidate.get("refresh_pending", False):
                    break
        except ToolError:
            pass
        time.sleep(0.2)
    if latest is None:
        raise ToolError("后台未在 25 秒内生成新详情；请检查 Computer Watch 是否仍在运行。")
    return {
        "generated_at_ms": latest["generated_at_ms"],
        "age_ms": time.time_ns() // 1_000_000 - latest["generated_at_ms"],
        "refresh_pending": latest.get("refresh_pending", False),
        "available_sections": [key for key in ("inventory", "cpu", "gpu", "memory", "disk", "network", "displays", "settings") if latest.get(key) is not None],
        "traffic_start_requested": start_traffic,
        "note": "available_sections 仅表示存在该节；传感器可用性请检查 sampled_at_ms、null 和 status。SMART/时序可能复用约 60 秒缓存。",
    }


@mcp.tool(annotations=READ_ONLY)
def get_hardware_details(section: Section = "all") -> dict[str, Any]:
    """读取最近一次 refresh_dashboard 的界面详情缓存，不触发采集。

    包含硬件型号/显卡规格/内存条、逻辑CPU、GPU频率/风扇/限制、内存用量/时序、
    物理磁盘SMART明细、网卡/IP/DNS、主板、显示器分辨率/刷新率/色域，以及主题/保留天数。
    null 是缺失，0 是实测零。始终检查 age_ms 和各节 sampled_at_ms，缓存可能来自已退出的后台。
    字段沿用后台 camelCase；容量为字节，frequencyMhz 为 MHz，温度 C、功耗 W。
    时序字段自带单位、来源、状态和采样时间；smartRows 是界面原始显示文本三列。
    """
    data = read_dashboard()
    now = time.time_ns() // 1_000_000
    for name in ("cpu", "gpu", "memory", "disk", "network"):
        pane = data.get(name)
        if isinstance(pane, dict):
            sampled = pane.get("sampled_at_ms")
            pane["age_ms"] = now - sampled if sampled is not None else None
    if section == "board":
        result = data.get("inventory", {}).get("board")
    elif section == "all":
        result = {key: value for key, value in data.items() if key not in ("live", "schema_version", "generated_at_ms", "refresh_pending")}
    else:
        result = data.get(section)
    return {"generated_at_ms": data["generated_at_ms"], "age_ms": time.time_ns() // 1_000_000 - data["generated_at_ms"],
            "refresh_pending": data.get("refresh_pending", False), "section": section, "data": result}


@mcp.tool(annotations=READ_ONLY)
def get_realtime(limit: LiveLimit = 61, include_rankings: bool = False) -> dict[str, Any]:
    """读取最近一次刷新时的最近60秒实际采样点（最多61个），不是重采样/插值。

    前台1秒、托盘5秒；返回数量可能少于limit。include_rankings可附每个采样点CPU/内存前10名。
    数据缓存的age_ms由最后实际采样点计算，刷新请调用refresh_dashboard。
    """
    data = read_dashboard()
    rows = data.get("live", [])[-limit:]
    if not include_rankings:
        rows = [{"timestamp_ms": row["timestamp_ms"], "metrics": row["metrics"]} for row in rows]
    else:
        rows = [{**row, "by_cpu": [r for r in row["by_cpu"] if r["pid"]],
                 "by_memory": [r for r in row["by_memory"] if r["pid"]]} for row in rows]
    return {"generated_at_ms": data["generated_at_ms"], "samples": rows,
            "age_ms": time.time_ns() // 1_000_000 - rows[-1]["timestamp_ms"] if rows else None}


def traffic_schema(db: sqlite3.Connection) -> None:
    if db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='traffic_bytes'").fetchone() is None:
        raise ToolError("数据库尚不支持出站流量，请更新 Computer Watch。")


@mcp.tool(annotations=READ_ONLY)
def get_traffic(period: TrafficPeriod = "today", from_ms: Timestamp | None = None,
                to_ms: Timestamp | None = None, sort_by: TrafficSort = "internet",
                descending: bool = True, limit: SampleLimit = 120, offset: Offset = 0) -> dict[str, Any]:
    """按程序完整路径查询公网(internet)、局域网(lan)、其他(other)累计发送字节。

    session读取详情缓存（可含rate字节/秒），today/week/custom读取SQLite。
    today按本机日界线，week滚动168小时；custom必须给出Unix毫秒闭区间。
    支持按internet/lan/other/total排序并分页。没有记录不等于期间零流量，采集未启用的时段不能补算。
    口径为TCP/UDP Send事件字节；局域网按私网/链路本地地址；其他含回环/组播/保留地址。
    代理/VPN流量归实际发送进程，不推断原应用。历史不包含每个时刻的丢失事件状态。
    """
    now = time.time_ns() // 1_000_000
    if period != "custom" and (from_ms is not None or to_ms is not None):
        raise ToolError("from_ms/to_ms 仅用于 custom。")
    metadata: dict[str, Any] = {"period": period, "units": "bytes", "sort_by": sort_by, "descending": descending}
    if period == "session":
        data = read_dashboard()
        report = (data.get("network", {}).get("data") or {}).get("traffic", {})
        rows = [{**row, "total": row["internet"] + row["lan"] + row["other"]} for row in report.get("session", [])]
        rows.sort(key=lambda row: row["path"])
        rows.sort(key=lambda row: row[sort_by], reverse=descending)
        metadata.update({"started_at_ms": report.get("started"), "status": report.get("status"), "lost": report.get("lost"),
                         "generated_at_ms": data["generated_at_ms"], "age_ms": now - data["generated_at_ms"], "total_programs": len(rows)})
        selected = rows[offset:offset + limit]
        has_more = offset + limit < len(rows)
    else:
        if period == "custom":
            if from_ms is None or to_ms is None or from_ms > to_ms:
                raise ToolError("custom 要求 from_ms <= to_ms 且两者均提供。")
        else:
            to_ms = now
            from_ms = int(datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()*1000) if period == "today" else max(0, now-7*86400000)
        with read_history() as db:
            traffic_schema(db)
            column = {"internet": "SUM(public)", "lan": "SUM(lan)", "other": "SUM(other)", "total": "SUM(public+lan+other)"}[sort_by]
            direction = "DESC" if descending else "ASC"
            rows = db.execute(f"""SELECT path, SUM(public) AS internet, SUM(lan) AS lan, SUM(other) AS other,
                SUM(public+lan+other) AS total FROM traffic_bytes WHERE time BETWEEN ? AND ?
                GROUP BY path ORDER BY {column} {direction},path LIMIT ? OFFSET ?""", (from_ms,to_ms,limit+1,offset)).fetchall()
            bounds = db.execute("SELECT MIN(time),MAX(time),COUNT(DISTINCT path) FROM traffic_bytes WHERE time BETWEEN ? AND ?", (from_ms,to_ms)).fetchone()
            metadata.update({"from_ms": from_ms, "to_ms": to_ms, "first_record_ms": bounds[0], "last_record_ms": bounds[1],
                             "age_ms": now-bounds[1] if bounds[1] is not None else None, "total_programs": bounds[2],
                             "retention_days": db.execute("SELECT retention_days FROM settings WHERE id=1").fetchone()[0],
                             "coverage_note": "记录范围不代表连续采集覆盖；无记录时不能断言零流量。"})
            selected = [dict(row) for row in rows[:limit]]
            has_more = len(rows) > limit
    return {**metadata, "programs": [{**row, "name": ntpath.basename(row["path"])} for row in selected],
            "next_offset": offset+limit if has_more else None}


if __name__ == "__main__":
    mcp.run(transport="stdio")
