# Computer Watch MCP

MCP 客户端通过本机 stdio 按需启动服务。服务以当前 Windows 用户的权限只读访问 `%LOCALAPPDATA%\ComputerWatch\history.db`，需要 Computer Watch 已经保存过采样。

在 Computer Watch 的「设置 → MCP 客户端接入」中点击「复制客户端配置」，即可获得指向当前程序旁服务脚本的配置。若客户端的 PATH 中没有 uv，请将配置中的 `uv` 改为其绝对路径。

先安装 uv，并在 PowerShell 中运行 `(Get-Command uv.exe).Source` 获取其绝对路径。将以下配置合并到支持 `mcpServers` 的客户端配置中，按实际位置替换两个路径。客户端需使用运行 Computer Watch 的同一 Windows 用户，并保留该用户的 `LOCALAPPDATA`。首次启动时 uv 会准备 Python 3.11+ 和 MCP SDK。

```json
{
  "mcpServers": {
    "computer-watch": {
      "command": "C:/path/to/uv.exe",
      "args": ["run", "--script", "D:/AI/computer-watch/mcp_server.py"]
    }
  }
}
```

- `get_snapshot` 默认返回最新落盘采样及当时的 CPU 和内存前 10 名。传入 `at_ms` 可查看该时间点及以前的最近采样。用 `age_ms` 判断数据有多旧。采集期间约每 5 秒落盘，退出托盘后仍可查询已保存的数据。
- `get_history` 接收 `from_ms` 和 `to_ms`，按时间升序返回闭区间内的采样。默认每页 120 条，最多 300 条。继续查询时将 `next_from_ms` 作为新的 `from_ms`，保留原来的 `to_ms`。

时间戳使用 Unix 毫秒。容量使用字节，磁盘和网络速率使用字节/秒，温度使用摄氏度，功耗使用瓦。CPU 功耗为封装功耗，GPU 功耗为板卡功耗。缺失读数返回 `null`。连接的客户端可以读取采样和进程名称，具体使用方式取决于客户端配置。
