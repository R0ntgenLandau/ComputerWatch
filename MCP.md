# Computer Watch MCP

MCP 客户端通过本机 stdio 按需启动服务。服务以当前 Windows 用户的权限读取 `%LOCALAPPDATA%\ComputerWatch\history.db` 和按需生成的 `dashboard.json`。历史查询不要求面板运行；按需刷新要求同一 Windows 会话中的新版后台正在运行。

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


## 所有界面数据接入

现有客户端配置保持不变。更新后重启客户端中的 Computer Watch MCP 服务以重新发现工具。

| 工具 | 用途 |
| --- | --- |
| `refresh_dashboard(start_traffic=false)` | 通知现有后台按需采集全部详情，最长等待 25 秒，不打开窗口、不切换界面、不自动提权。返回刷新时间及是否仍在等待。 |
| `get_hardware_details(section="all")` | 获取最近一次刷新缓存；section 支持 inventory、cpu、gpu、memory、disk、network、board、displays、settings、all。 |
| `get_realtime(limit=61, include_rankings=false)` | 获取最近一次刷新时最近 60 秒的实际采样点，可附每点 CPU/内存排行，不插值。 |
| `get_traffic(period="today", ...)` | 按程序完整路径汇总出站流量，支持 session/today/week/custom，公网/局域网/其他/总量排序与分页。 |

### 推荐调用顺序

1. 调用 `refresh_dashboard()` 更新全部详情，再调用 `get_hardware_details()` 或指定 section。
2. 需要当前出站统计时，显式调用 `refresh_dashboard(start_traffic=true)` 开启本次运行的采集，再调用 `get_traffic(period="session")`。
3. 只查询已保存的流量可直接调用 `get_traffic(period="today")` 或 `period="week"`，无须开启新采集。`custom` 必须同时给出 `from_ms` 和 `to_ms`（闭区间）。
4. 通过 `sort_by="internet"|"lan"|"other"|"total"`、`descending` 排序；默认公网降序。`limit` 默认 120、最多 300，后续页使用 `next_offset`。分页期间仍在采集时，排行可能变化，分页不是固定快照。

### 详情覆盖

- inventory：CPU、内存、磁盘、GPU 型号，显卡架构/CUDA能力/显存/总线/PCIe/功耗限制规格，内存条槽位、容量、厂商、序列号、速率、电压、Rank等。
- cpu：各处理器组和逻辑处理器负载、估算频率。
- gpu：核心/显存频率、风扇百分比/转速、热点温度、显存/编码/解码活动、功耗限制、性能状态、PCIe链路、温度/功耗限制状态。
- memory：缓存、提交量/上限/峰值、分页/非分页池、页大小，时序字段的值、单位、来源、状态、原因、采样时间。
- disk：各物理磁盘健康、温度、寿命消耗、通电时间/次数、错误数、说明，以及界面 SMART 三列明细。
- network：物理/虚拟网卡数量、名称/型号/MAC、收发链路速率、IPv4/IPv6/网关/DNS，及最近刷新的本次/今天/7天出站汇总。
- board：主板页面全部分组和字段；displays：活动显示器名称、连接方式、信号分辨率、刷新率、RGB原色色度坐标及sRGB/P3/AdobeRGB覆盖率。
- settings：当前 Windows 用户的主题和历史保留天数。
- 原有 `get_snapshot` / `get_history` 继续提供全部首页指标、温度/功耗历史和进程排行；`get_realtime` 提供 UI 最近60秒曲线的实际点。

### 状态、单位与开销

详情字段沿用后台 camelCase；时间戳为 Unix 毫秒。容量/累计量为字节、上传速率 rate 为字节/秒、网卡链路速率为 bit/s，频率 MHz、温度 °C、功耗 W、风扇 RPM，色域为百分比。内存条 `legacySpeed=true` 时速率单位为 MHz，否则为 MT/s。时序自带 unit，SMART 明细保留界面文本单位。

`null` 是缺失，`0` 是实测零；GPU限制状态未提供时返回 null，不返回 false。每次响应提供刷新时间/age_ms，各详情节另有采样时间/age_ms。SMART与时序可能复用约60秒缓存，时序另含自身采样时间；静态硬件清单与界面一致，在本次后台运行中缓存。未连硬件助手、缺少设备读数或旧缓存均不能解读为零。刷新结束只表示该轮采集结束，不保证所有设备可读。

读取缓存不产生额外采集。只有 refresh_dashboard 会发本机事件通知现有后台，并在有限刷新窗口中采集扩展信息；保持前台1秒、托盘5秒采样和5秒历史落盘规则。缓存文件原子替换，读取不会看到半份JSON；程序退出后缓存仍可读，务必检查时间戳。刷新工具标注为非破坏性写操作，其余工具标注为只读。

出站流量按 ETW TCP/UDP Send 字节计量，代理/VPN归实际发送进程。历史以落库时间归属，不能推断未采集时段为零，也不提供历史每时刻的ETW丢失标记。`session` 中提供当前状态码和丢失提示。连接客户端可以读取硬件序列号、局域网地址、程序路径与名称等本机信息。

## 验证

- `uv run --with mcp==1.29.0 python tests/test_mcp.py`：工具发现/参数校验、分页排序、闭区间、缺失与零、旧数据库/损坏缓存。
- 编译运行 `tests/mcp_export_tests.cpp` 与 `mcp_export.cpp` 可在 build/test-dashboard.json 检查本机 JSON 序列化（Unicode、转义、缺失状态）。

真实 stdio 实机验收（会请求后台刷新）：

```powershell
uv run --with mcp==1.29.0 python tests/mcp_stdio_smoke.py --script 'C:/Program Files/ComputerWatch/mcp_server.py' --refresh
```

需要同时验证开启出站采集时追加 `--start-traffic`。该测试只在明确给出 `--refresh` 时请求刷新。
