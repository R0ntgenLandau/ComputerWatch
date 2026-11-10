# Computer Watch Windows 发布包

根据 Windows 的系统类型选择 x86（32 位）、x64（64 位）或 ARM64（64 位 ARM）。

1. 解压整个 ZIP，保留目录结构。
2. 在解压目录中用 PowerShell 运行 `./install.ps1`。脚本会请求管理员权限，安装主程序、MCP 脚本和登录启动任务。
3. 从托盘打开 Computer Watch。在「设置 → MCP 客户端接入」复制客户端配置。MCP 客户端需要 uv。

卸载时从同一目录运行 `./install.ps1 -Uninstall`。

CPU 封装温度与功耗依赖兼容的 Intel 处理器和 PawnIO。ARM64 设备没有这组 Intel 读数。NVIDIA GPU 读数取决于对应架构的 NVML。缺失读数在界面中显示为缺失。

从源码打包时，在项目根目录运行 `./package.ps1 -LlvmMingw <已解压的 LLVM-MinGW 工具链目录>`。x64 使用 PATH 中的 MinGW-w64 GCC 和 windres，x86 和 ARM64 使用指定的 LLVM-MinGW。生成的 ZIP 位于 `package/`，远程下载请使用 GitHub Releases。
