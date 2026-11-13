# Computer Watch

Computer Watch 是一款在 Windows 本机运行的桌面监控工具。面板展示 CPU、内存、磁盘、显卡、网络及 CPU 和内存进程排行。关闭窗口后继续在托盘采集，通过托盘菜单退出可停止采集。

## 下载和安装

当前版本为 [V0.1.0003](https://github.com/R0ntgenLandau/ComputerWatch/releases/tag/V0.1.0003)。根据 Windows 的系统类型选择对应的 ZIP。

| 系统类型 | 下载 |
| --- | --- |
| x86（32 位） | [Windows x86](https://github.com/R0ntgenLandau/ComputerWatch/releases/download/V0.1.0003/ComputerWatch-V0.1.0003-windows-x86.zip) |
| x64（64 位） | [Windows x64](https://github.com/R0ntgenLandau/ComputerWatch/releases/download/V0.1.0003/ComputerWatch-V0.1.0003-windows-x64.zip) |
| ARM64（64 位 ARM） | [Windows ARM64](https://github.com/R0ntgenLandau/ComputerWatch/releases/download/V0.1.0003/ComputerWatch-V0.1.0003-windows-arm64.zip) |

解压整个 ZIP，在解压目录用 PowerShell 运行 `./install.ps1`，并同意管理员权限提示。安装完成后从托盘打开 Computer Watch。卸载时运行 `./install.ps1 -Uninstall`。

需要 Intel CPU 封装温度和功耗读数时，请从 [PawnIO 官网](https://pawnio.eu/) 安装驱动。

## 监控和历史

- 面板可见时每秒采样，收进托盘后每 5 秒采样。历史每 5 秒保存到本地 SQLite，默认保留 7 天，可在设置中调整。
- 实时和历史均提供 CPU 与内存各自的前 10 名，点击列标题可切换排序。
- CPU、显卡和磁盘卡片可进入详情。CPU 封装温度和功耗依赖兼容的 Intel 处理器和 PawnIO，GPU 读数依赖对应架构的 NVIDIA NVML。未提供的读数显示为缺失。
- 顶部「显示器」页逐台列出 Windows 当前启用的显示器，展示名称、信号分辨率、刷新率和连接类型。点击某台显示器可查看 CIE 1931 xy 坐标图、RGB 顶点，以及根据 EDID 估算的 sRGB、Display P3 和 Adobe RGB 覆盖率。
- 设置页支持浅色和深色主题。

## MCP 接入

在 **设置 → MCP 客户端接入** 点击 **复制客户端配置**，将配置添加到本机 MCP 客户端。客户端通过 uv 按需启动 stdio 服务，可以查询落盘快照、历史采样、进程排行、全部硬件详情和出站流量，并按需通知后台刷新。查询工具只读，刷新工具默认不启动出站采集。详见 [MCP 配置说明](MCP.md)。

## 从源码构建

在项目根目录运行 `./build.ps1`，默认使用 PATH 中的 MinGW-w64 GCC 和 windres 生成 x64 版本。三架构打包需要额外提供 LLVM-MinGW，命令及安装包结构见 [发布包说明](package/README.md)。

## 贡献和许可证

欢迎通过 [Issues](https://github.com/R0ntgenLandau/ComputerWatch/issues) 报告问题，或提交 Pull Request。报告问题时请注明 Windows 版本、系统架构、Computer Watch 版本和复现步骤。提交改动前请查阅 [项目约定](AGENTS.md)。

Computer Watch 自有代码采用 [GPL-3.0-only](LICENSE)。第三方组件保留各自的许可证，详见 [第三方组件说明](THIRD-PARTY-NOTICES.md)。


### 按程序统计出站流量

进入「网络 → 详情」后开始按需采集，网卡信息下方展示本次、今天、最近 7 天的发送量，默认按公网发送量降序。离开详情或关闭到托盘后继续采集，退出程序停止；下次运行需要再次进入网络详情开启。相同程序路径的多个进程合并，不读取报文内容。开始时间显示在统计状态中。

- 公网：按目标地址判断的公网单播；局域网：IPv4 私网、链路本地及 IPv6 ULA、链路本地。其他：回环、组播及已识别的保留地址。此分类不是实际路由/地理位置判定，IPv6 公网地址的同网段通信仍归公网。
- 使用 Windows ETW TCP/UDP Send 事件的字节数，不等于运营商账单或网卡线速字节数，未额外累加 TCP 重传事件。经代理/VPN转发可能归到代理/隧道程序，不能据此还原原应用最终目的地。
- 采集助手提权，面板与托盘保持普通用户权限。首次进入网络详情启动 ETW，会话不写原始 ETL 文件。
- 每 5 秒将变化量保存到本地 SQLite，沿用历史保留天数；今天按本地日界线、最近 7 天按滚动 168 小时查询。归属时间为采样保存时间，跨日附近有采样间隔误差；未运行/未启用期间无法补算。异常退出可能丢失最后尚未保存的区间。
- 本次采集最多保留 512 个程序路径，超限或 ETW 丢失事件时明确显示统计不完整；无法解析程序时标记 PID，避免归入其他已知程序。缺少权限或助手断连时显示错误码，不将其当作零流量。

验证辅助：在项目根目录编译 `tests/traffic_tests.cpp`、`traffic.cpp`、`history.cpp` 为控制台程序并链接 `advapi32`，可验证地址分类、重复保存、会话重启、增量与保留清理。测试数据库写入被 Git 忽略的 `build/`。
