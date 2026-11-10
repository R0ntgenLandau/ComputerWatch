# Computer Watch

Computer Watch 是一款在 Windows 本机运行的桌面监控工具。面板展示 CPU、内存、磁盘、显卡、网络及 CPU 和内存进程排行。关闭窗口后继续在托盘采集，通过托盘菜单退出可停止采集。

## 下载和安装

当前版本为 [V0.1.0001](https://github.com/R0ntgenLandau/ComputerWatch/releases/tag/V0.1.0001)。根据 Windows 的系统类型选择对应的 ZIP。

| 系统类型 | 下载 |
| --- | --- |
| x86（32 位） | [Windows x86](https://github.com/R0ntgenLandau/ComputerWatch/releases/download/V0.1.0001/ComputerWatch-V0.1.0001-windows-x86.zip) |
| x64（64 位） | [Windows x64](https://github.com/R0ntgenLandau/ComputerWatch/releases/download/V0.1.0001/ComputerWatch-V0.1.0001-windows-x64.zip) |
| ARM64（64 位 ARM） | [Windows ARM64](https://github.com/R0ntgenLandau/ComputerWatch/releases/download/V0.1.0001/ComputerWatch-V0.1.0001-windows-arm64.zip) |

解压整个 ZIP，在解压目录用 PowerShell 运行 `./install.ps1`，并同意管理员权限提示。安装完成后从托盘打开 Computer Watch。卸载时运行 `./install.ps1 -Uninstall`。

## 监控和历史

- 面板可见时每秒采样，收进托盘后每 5 秒采样。历史每 5 秒保存到本地 SQLite，默认保留 7 天，可在设置中调整。
- 实时和历史均提供 CPU 与内存各自的前 10 名，点击列标题可切换排序。
- CPU、显卡和磁盘卡片可进入详情。CPU 封装温度和功耗依赖兼容的 Intel 处理器和 PawnIO，GPU 读数依赖对应架构的 NVIDIA NVML。未提供的读数显示为缺失。
- 设置页支持浅色和深色主题。

## MCP 接入

在 **设置 → MCP 客户端接入** 点击 **复制客户端配置**，将配置添加到本机 MCP 客户端。客户端通过 uv 按需启动只读 stdio 服务，可以查询落盘快照、历史采样和当时的进程排行。详见 [MCP 配置说明](MCP.md)。

## 从源码构建

在项目根目录运行 `./build.ps1`，默认使用 PATH 中的 MinGW-w64 GCC 和 windres 生成 x64 版本。三架构打包需要额外提供 LLVM-MinGW，命令及安装包结构见 [发布包说明](package/README.md)。

## 贡献和许可证

欢迎通过 [Issues](https://github.com/R0ntgenLandau/ComputerWatch/issues) 报告问题，或提交 Pull Request。报告问题时请注明 Windows 版本、系统架构、Computer Watch 版本和复现步骤。提交改动前请查阅 [项目约定](AGENTS.md)。

Computer Watch 自有代码采用 [GPL-3.0-only](LICENSE)。第三方组件保留各自的许可证，详见 [第三方组件说明](THIRD-PARTY-NOTICES.md)。
