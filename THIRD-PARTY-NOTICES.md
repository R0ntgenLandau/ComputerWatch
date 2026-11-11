# 第三方组件

Computer Watch 自有代码采用 GPL-3.0-only。以下组件适用各自的许可条款。

- **Anonymous Pro Bold** 由 Mark Simonson 设计，采用 SIL Open Font License 1.1。字体嵌入 EXE，许可文本随源码和发布包提供。[字体来源](https://www.marksimonson.com/fonts/view/anonymous/)

- **PawnIO.Modules 0.2.11** 由 namazso 和上游贡献者发布，采用 LGPL-2.1-or-later。Computer Watch 将上游签名的 `IntelMSR.bin`、`IntelMCHBAR.bin` 和 `AMDFamily17.bin` 嵌入 EXE。发布包的 `licenses/` 附有上游许可证及对应源码归档。应用源码提供资源嵌入和构建步骤。调试对该 LGPL 组件的修改时，可以按 LGPL 条款进行逆向工程。运行修改后的模块取决于 PawnIO 版本及签名要求。[上游发行版](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)
- **ZenStates-Core DDR4 位域定义** 采用 GPL-3.0，固定提交 `869b7e82572337e5a673e92230edc92bddf0e44b`。原始文件和许可证见 `third_party/zenstates-core/`。本项目将 DDR4Dictionary 中的字段改写为 C++ 描述表，保留读取错误，并增加字段来源、状态及作用范围。[上游源码](https://github.com/irusanov/ZenStates-Core/tree/869b7e82572337e5a673e92230edc92bddf0e44b)
- **PawnIO 官方安装器 2.2.0** 使用官方签名二进制，SHA-256 固定校验。驱动源代码采用 GPL-2.0-or-later 及上游附加例外。候选包的对应源码关系和正式再分发材料还在核查，发布限制见 MEMORY_TIMINGS.md。[上游源码和许可](https://github.com/namazso/PawnIO)
- **编译器运行库**。x64 包附有 GCC 运行库许可及运行库例外、MinGW-w64 运行库和 winpthreads 许可。x86 和 ARM64 包附有 LLVM 许可及 LLVM 例外、MinGW-w64 运行库和 winpthreads 许可。相应文本在各包的 `licenses/` 目录。[GCC](https://gcc.gnu.org/) · [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)
- **运行时依赖**。发布包包含 Computer Watch 的 MCP 服务脚本，官方 MCP Python SDK 由 uv 另行获取。PawnIO 驱动和 PawnIOLib、NVIDIA NVML 由用户的系统或硬件厂商提供。[MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) · [PawnIO](https://pawnio.eu/)

Computer Watch 本版本的源码和打包脚本见 [V0.1.0002 源码标签](https://github.com/R0ntgenLandau/ComputerWatch/tree/V0.1.0002)。
