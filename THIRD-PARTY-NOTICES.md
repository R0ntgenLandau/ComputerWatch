# 第三方组件

Computer Watch 自有代码采用 GPL-3.0-only。以下组件适用各自的许可条款。

- **PawnIO.Modules IntelMSR 0.2.11** 由 namazso 和上游贡献者发布，采用 LGPL-2.1-or-later。Computer Watch 将上游签名的 `IntelMSR.bin` 嵌入 EXE 的资源中。发布包的 `licenses/` 附有上游许可证及对应源码归档。应用源码提供了资源嵌入和构建步骤，可修改资源输入后重新构建。调试对该 LGPL 组件的修改时，可以按 LGPL 条款进行逆向工程。运行修改后的模块取决于所使用的 PawnIO 版本及其签名要求。[上游发行版](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)
- **编译器运行库**。x64 包附有 GCC 运行库许可及运行库例外、MinGW-w64 运行库和 winpthreads 许可。x86 和 ARM64 包附有 LLVM 许可及 LLVM 例外、MinGW-w64 运行库和 winpthreads 许可。相应文本在各包的 `licenses/` 目录。[GCC](https://gcc.gnu.org/) · [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
- **运行时依赖**。发布包包含 Computer Watch 的 MCP 服务脚本，官方 MCP Python SDK 由 uv 另行获取。PawnIO 驱动和 PawnIOLib、NVIDIA NVML 由用户的系统或硬件厂商提供。[MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) · [PawnIO](https://pawnio.eu/)

Computer Watch 本版本的源码和打包脚本见 [V0.1.0001 源码标签](https://github.com/R0ntgenLandau/ComputerWatch/tree/V0.1.0001)。
