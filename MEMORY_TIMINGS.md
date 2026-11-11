# 内存时序实现与验收状态

当前版本为开发候选，完整时序功能尚未达到正式分发验收条件。

## 本机实测

- Windows x64，CPU 为 Intel Xeon E5-2697A v4。CPUID 和 Windows 同时确认 GenuineIntel、Family 6、Model 79（0x4F）、Stepping 1，签名 0x406F1。
- 主板 SMBIOS 为 INTEL / X99 / V1.0。该字符串是固件自报值。
- Windows 枚举到 PCI 8086:6F68、6FA8 系列控制器设备。通道/控制器内部拓扑还需要对应平台寄存器确认，未用 DIMM 数量推定通道数量。
- 四条 DDR4，每条 16 GiB，总安装容量 64 GiB。位置为 DIMM_A1、DIMM_B1、DIMM_C1、DIMM_D1，BankLabel 分别为 NODE 1、NODE 1、NODE 2、NODE 2。
- 原生探测以普通用户运行，PawnIO 打开返回 0x80070005。提权后打开成功，官方签名 IntelMCHBAR 0.2.11 模块加载返回 0x80070032，无法建立本机读取链路。
- 本机自动采集的当前时序值均为 null。当前没有获得可以交叉验证的实时主时序、次级或三级时序数值。
- 本机旧 HWiNFO 报告文件时间为 2026-04-23，主时序为 15-15-15-36。这份文件用于备用导入测试，其读数属于历史记录。

## 采集链路

普通权限界面和采集器通过既有用户隔离的共享映射、请求和响应事件访问提权助手。内存时序使用独立工作线程，每 60 秒最多采集一次，仅内存详情页请求扩展数据。助手启动同一可执行文件的受限采集子进程，创建命名共享映射接收结果。5 秒未退出时终止该采集子进程。前台轮询响应事件，不等待底层驱动。

原生代码从 CPUID 识别平台。AMD 后端仅匹配 Matisse（17h/71h）和 Vermeer（19h/21h）桌面 DDR4，不对其他 Family/Model 套用此解码。底层使用随 EXE 嵌入的官方签名 AMDFamily17 0.2.11 模块，通过 ioctl_read_smn 读取。SMN 索引选择在驱动内部完成，应用不调用时序、电压、SMU 参数写入接口。使用 Global\Access_PCI 互斥锁协调索引/数据访问。

每个字段保留数值、单位、UMC 范围、原始寄存器位域来源、时间、状态和原因。未取得的数值使用 NaN 作为内部缺失表示，对外语义为 null。实际零值保留为零。AMD 解码结果统一标为 unverified，原因中明确缺少目标平台实测，避免将源代码覆盖误当成准确性验证。

## 硬件平台支持矩阵

| 平台 | 原生实现 | 实机结果 | 完整性状态 | 独立部署 |
| --- | --- | --- | --- | --- |
| Xeon E5 v4，6/4F，DDR4 | CPUID 分派、签名 IntelMCHBAR 真实探测 | 本机提权模块加载失败 0x80070032 | backend_missing；缺少服务器时序寄存器后端和解码 | 未通过 |
| AMD Matisse，17h/71h，DDR4 | AMDFamily17 SMN，两个 UMC，83 项源位域及附加刷新配置 | 尚未实机验证 | 解码已实现；字段全集和单位仍需平台核查 | 尚未实机验证 |
| AMD Vermeer，19h/21h，DDR4 | 同上，按明确 CPUID 匹配 | 尚未实机验证 | 同上 | 尚未实机验证 |
| 其他 Intel/AMD 代际及 DDR5 | 自动识别并返回具体缺少适配状态 | 无目标硬件 | backend_missing | 未验证 |
| Windows x86 / ARM64 程序 | 基础监控可构建；时序原生读取尚未适配 | 未实测 | backend_missing | 时序未验证 |

本矩阵没有可声明“完整时序已实测通过”的平台。

## 字段覆盖依据

AMD 代码位域表来自固定版本 ZenStates-Core，提交 869b7e82572337e5a673e92230edc92bddf0e44b。原始 DDR4Dictionary.cs、Ddr4Timings.cs 和 GPL-3.0 许可证存放在 third_party/zenstates-core。memory_ddr4_fields.inc 保留地址、低位和位宽。来源中的非时序配置项保留 encoded 单位，等待语义验证。

这份开源表构成已实现解码的可核对清单。对平台适用字段全集的核查尚未闭合，不能据此宣称完整。

以下项目在本机均无当前值，读取方式、平台适用性和精度需继续完成。对于作用范围尚未确定的项目，未创建每 DIMM 的独立读数。

| 核查项目 | AMD DDR4 实现 | 当前值与单位 | 验证状态 / 剩余问题 |
| --- | --- | --- | --- |
| tCL、tRCDRD、tRCDWR、tRP、tRAS | SMN 0x50204 / 0x50208 | null；tCK | 尚未 AMD 实测；本机 Intel 缺后端 |
| Command Rate | 0x50200[10] + 1，标明编码换算 | null；tCK | GDM 模式下的解释和对照待验证 |
| tRC | 0x50208[7:0] | null；tCK | 待实测 |
| tRFC / tRFC2 / tRFC4 | 分别显示 0x50260、0x50264 两组配置 | null；tCK | 未猜测两组与 Rank 的映射；活动 FGR 模式还需验证 |
| tREFI | 0x50230[15:0] | null；tCK | 温度、刷新模式影响待核查 |
| tRRDS / tRRDL、tFAW | 0x5020C / 0x50210 | null；tCK | 待实测 |
| tWTRS / tWTRL、tCWL、tWR、tRTP | 0x50214 / 0x50218 / 0x5020C | null；tCK | 待实测 |
| tRDRDSCL / SC / SD / DD | 0x50220 | null；tCK | 别名及 bank/rank/DIMM 关系待平台验证 |
| tWRWRSCL / SC / SD / DD | 0x50224 | null；tCK | 同上 |
| tRDWR / tWRRD | 0x50228 | null；tCK | 同上 |
| tCCD 及变体 | 未建立平台别名或独立位域依据 | null | unverified |
| RTL / IOL | 未建立 AMD 适用性和 Intel 服务器读取依据 | null | unverified，未标成硬件不支持 |
| PHY 读写延迟、RDDATAEN | 0x50258 | null；源定义时钟字段 | 与 PHY 运行模式的关系待验证 |
| MRD / MOD / PDA / XS / DLLK / XP / CKE 等 | 详见完整位域表 | null；按字段记录 | 已实现，尚未实测 |
| MemClkRatio | 0x50200[6:0] / 3 | null；ratio | 计算值；未独立测量 BCLK |
| 实际内存时钟和有效数据率 | 读取能力缺失 | null | 未以假定 100 MHz 参考时钟填充 |

## AMD 逐字段覆盖表

以下每行均保留寄存器依据。原生界面还提供实际读取时间和每次读取状态。本机为 Intel，表中 AMD 当前值均为 null。

| 字段 | 适用范围 | 读取方式 | 当前值 | 单位 | 验证依据 | 实现状态及剩余问题 |
| --- | --- | --- | --- | --- | --- | --- |
| DimmEccEn | Matisse / Vermeer，UMC 0/1 | SMN 0x50100[12:12] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| BurstCtrl | Matisse / Vermeer，UMC 0/1 | SMN 0x50100[11:10] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| BurstLength | Matisse / Vermeer，UMC 0/1 | SMN 0x50100[9:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| AggrPwrDownEn | Matisse / Vermeer，UMC 0/1 | SMN 0x5012C[30:30] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| PowerDownMode | Matisse / Vermeer，UMC 0/1 | SMN 0x5012C[29:29] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| PowerDown | Matisse / Vermeer，UMC 0/1 | SMN 0x5012C[28:28] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| OdtsIncRefEn | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[28:28] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| OdtsEn | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[27:27] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| ForcePwrDownThrotEn | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[26:26] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| OdtsCmdThrotEn | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[25:25] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| SwCmdThrotEn | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[24:24] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| OdtsCmdThrotCyc | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[23:16] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| SwCmdThrotCyc | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[15:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| RollWindowDepth | Matisse / Vermeer，UMC 0/1 | SMN 0x50130[7:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| Preamble2t | Matisse / Vermeer，UMC 0/1 | SMN 0x50200[12:12] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| GDM | Matisse / Vermeer，UMC 0/1 | SMN 0x50200[11:11] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| Cmd2T | Matisse / Vermeer，UMC 0/1 | SMN 0x50200[10:10] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| BankGroupEn | Matisse / Vermeer，UMC 0/1 | SMN 0x50200[8:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRCDWR | Matisse / Vermeer，UMC 0/1 | SMN 0x50204[29:24] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRCDRD | Matisse / Vermeer，UMC 0/1 | SMN 0x50204[21:16] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRAS | Matisse / Vermeer，UMC 0/1 | SMN 0x50204[14:8] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tCL | Matisse / Vermeer，UMC 0/1 | SMN 0x50204[5:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRC | Matisse / Vermeer，UMC 0/1 | SMN 0x50208[7:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRP | Matisse / Vermeer，UMC 0/1 | SMN 0x50208[21:16] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRTP | Matisse / Vermeer，UMC 0/1 | SMN 0x5020C[28:24] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRRDL | Matisse / Vermeer，UMC 0/1 | SMN 0x5020C[12:8] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRRDS | Matisse / Vermeer，UMC 0/1 | SMN 0x5020C[4:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tFAW | Matisse / Vermeer，UMC 0/1 | SMN 0x50210[6:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWTRL | Matisse / Vermeer，UMC 0/1 | SMN 0x50214[22:16] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWTRS | Matisse / Vermeer，UMC 0/1 | SMN 0x50214[12:8] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tCWL | Matisse / Vermeer，UMC 0/1 | SMN 0x50214[5:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWR | Matisse / Vermeer，UMC 0/1 | SMN 0x50218[6:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| TRCPAGE | Matisse / Vermeer，UMC 0/1 | SMN 0x5021C[31:20] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| RDRDBan | Matisse / Vermeer，UMC 0/1 | SMN 0x50220[31:30] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRDRDSCL | Matisse / Vermeer，UMC 0/1 | SMN 0x50220[27:24] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRDRDSC | Matisse / Vermeer，UMC 0/1 | SMN 0x50220[19:16] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRDRDSD | Matisse / Vermeer，UMC 0/1 | SMN 0x50220[11:8] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRDRDDD | Matisse / Vermeer，UMC 0/1 | SMN 0x50220[3:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| WRWRBan | Matisse / Vermeer，UMC 0/1 | SMN 0x50224[31:30] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWRWRSCL | Matisse / Vermeer，UMC 0/1 | SMN 0x50224[29:24] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWRWRSC | Matisse / Vermeer，UMC 0/1 | SMN 0x50224[19:16] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWRWRSD | Matisse / Vermeer，UMC 0/1 | SMN 0x50224[11:8] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWRWRDD | Matisse / Vermeer，UMC 0/1 | SMN 0x50224[3:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRDWR | Matisse / Vermeer，UMC 0/1 | SMN 0x50228[12:8] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tWRRD | Matisse / Vermeer，UMC 0/1 | SMN 0x50228[3:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| ShortInit | Matisse / Vermeer，UMC 0/1 | SMN 0x5022C[31:31] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| ZqcsInterval | Matisse / Vermeer，UMC 0/1 | SMN 0x5022C[29:20] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| TzqOperCal | Matisse / Vermeer，UMC 0/1 | SMN 0x5022C[19:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| Tzqcs | Matisse / Vermeer，UMC 0/1 | SMN 0x5022C[7:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tREFI | Matisse / Vermeer，UMC 0/1 | SMN 0x50230[15:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tMODPDA | Matisse / Vermeer，UMC 0/1 | SMN 0x50234[29:24] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tMRDPDA | Matisse / Vermeer，UMC 0/1 | SMN 0x50234[21:16] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tMOD | Matisse / Vermeer，UMC 0/1 | SMN 0x50234[13:8] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tMRD | Matisse / Vermeer，UMC 0/1 | SMN 0x50234[5:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tDLLK | Matisse / Vermeer，UMC 0/1 | SMN 0x50238[26:16] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tXS | Matisse / Vermeer，UMC 0/1 | SMN 0x50238[10:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| RankBusyDly | Matisse / Vermeer，UMC 0/1 | SMN 0x5023C[30:24] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| CmdParLatency | Matisse / Vermeer，UMC 0/1 | SMN 0x5023C[19:16] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| AlertParDly | Matisse / Vermeer，UMC 0/1 | SMN 0x5023C[14:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| AlertCrcDly | Matisse / Vermeer，UMC 0/1 | SMN 0x5023C[6:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| AggrPwrDownDly | Matisse / Vermeer，UMC 0/1 | SMN 0x50244[30:25] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| PwrDownDly | Matisse / Vermeer，UMC 0/1 | SMN 0x50244[24:17] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| PD | Matisse / Vermeer，UMC 0/1 | SMN 0x50244[4:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tSTAG | Matisse / Vermeer，UMC 0/1 | SMN 0x50250[23:16] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tCKE | Matisse / Vermeer，UMC 0/1 | SMN 0x50254[28:24] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tCPDED | Matisse / Vermeer，UMC 0/1 | SMN 0x50254[19:16] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tXP | Matisse / Vermeer，UMC 0/1 | SMN 0x50254[5:0] | null | tCK | ZenStates 固定提交 | 已实现，尚未实机验证 |
| PARINL | Matisse / Vermeer，UMC 0/1 | SMN 0x50258[29:28] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tPHYWRD | Matisse / Vermeer，UMC 0/1 | SMN 0x50258[26:24] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tPHYRDL | Matisse / Vermeer，UMC 0/1 | SMN 0x50258[21:16] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tPHYWRL | Matisse / Vermeer，UMC 0/1 | SMN 0x50258[12:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| tRDDATAEN | Matisse / Vermeer，UMC 0/1 | SMN 0x50258[6:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| TgearHold | Matisse / Vermeer，UMC 0/1 | SMN 0x5025C[30:28] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| TgearSetup | Matisse / Vermeer，UMC 0/1 | SMN 0x5025C[26:24] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| LpExitDly | Matisse / Vermeer，UMC 0/1 | SMN 0x5025C[13:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| LpDly | Matisse / Vermeer，UMC 0/1 | SMN 0x5025C[5:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| WRMPR | Matisse / Vermeer，UMC 0/1 | SMN 0x5028C[29:24] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| CmdStgCnt | Matisse / Vermeer，UMC 0/1 | SMN 0x5028C[21:11] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| RcvrWait | Matisse / Vermeer，UMC 0/1 | SMN 0x5028C[10:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| WRPRE | Matisse / Vermeer，UMC 0/1 | SMN 0x502A4[10:8] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| RDPRE | Matisse / Vermeer，UMC 0/1 | SMN 0x502A4[2:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| DdrMaxRate | Matisse / Vermeer，UMC 0/1 | SMN 0x50DF0[7:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |
| DdrMaxRateEnf | Matisse / Vermeer，UMC 0/1 | SMN 0x50DF4[7:0] | null | encoded | ZenStates 固定提交 | 已实现，尚未实机验证 |

## 安装、更新、卸载和分发

- x64 候选包提供官方签名 PawnIO 2.2.0 安装器，SHA-256 为 1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032。prepare-dependencies.ps1 在构建时下载并检查固定摘要和 Authenticode。安装时从包内安装，无需网络。
- install.ps1 检查已有版本，缺失或低于 2.2.0 时使用官方 -install -silent 参数。保留软件面板普通权限和硬件助手提权计划任务。3010 提示需要重启。
- 默认卸载移除本软件任务、主程序及附带安装器。PawnIO 是系统共享依赖。显式使用 -Uninstall -RemovePawnIO 可以卸载共享驱动，这会影响其他使用 PawnIO 的软件。
- AMD 原生链路不依赖 CPU-Z、HWiNFO、AIDA64、其驱动或共享内存。备用报告导入不进入原生链路。
- 驱动的对应源码归档和供应方版本关系仍需完成发布审核，候选包尚不作为正式发行版。
- 本环境没有干净 Windows 虚拟机或 AMD 样机。首次安装、冷启动/重启、驱动首次加载、升级和完整卸载验收尚未完成。

## 可执行验证步骤

2026-11-11 的本机验证已通过 x86、x64、ARM64 编译和候选包生成。独立 UI 测试实例确认 4 × 16 GiB 固件信息、曲线展开收起、详情滚动、连续采样、三次 CPU 详情往返和托盘重开。UI 截图捕获曾超时，尚未完成截图视觉验收。

超时故障注入测试在 5022 ms 返回 read_failed，结果字段为空，采集子进程退出。这里验证的是超时处理。

本机已执行 install.ps1 更新、默认卸载、重新安装，核对已安装路径和 SHA-256 一致，两个计划任务均恢复 Running。已安装程序的真实 IPC 两次返回 Broadwell-EP 模块加载错误 0x80070032，字段数为 0。2026-11-11 09:53:09 的 SQLite 样本记录 CPU 封装温度 61 °C、RAPL 封装功耗 7.05 W、GPU 温度 50 °C、板卡功耗 41.59 W。这些是本软件采集值，未增加外部传感器对照。

现有 PawnIO 2.2.0 已预装，因此这次安装检验没有覆盖首次安装驱动。没有执行重启、干净系统部署、AMD 实测以及共享驱动移除验证。

1. 在有目标硬件的干净 Windows 10/11 x64 系统解压候选包，运行 ./install.ps1，接受安装提权。
2. 打开内存详情。记录自动采集的 CPUID、backend、状态、每个字段的 UMC、数值、时间和原因。若出现 unverified，继续保持这一状态直到完成交叉核验。
3. 在 Matisse/Vermeer 机器上确认两个 UMC 逐个检测空通道，使用已知配置对比主时序、次级和三级字段。参考工具只在验收环境使用。
4. 分别验证单通道、双通道、单 Rank、多 Rank 配置，重点核查 0x50260 / 0x50264 的有效范围和 FGR 模式。核对 MemClkRatio 与独立时钟测量。
5. 验证实时监控 → CPU 详情 → 返回三次，再进入内存页展开/收起曲线，关闭到托盘并重新打开。检查 SQLite 持续写入及 CPU/GPU 温度、功耗。
6. 重启后检查两个 ComputerWatch 计划任务、实际程序路径和字段采集时间。验证无开发环境、无第三方硬件工具时仍由包内依赖启动。
7. 在专用验证机检查 ./install.ps1 更新和 ./install.ps1 -Uninstall。只在允许移除共享驱动的机器运行 -Uninstall -RemovePawnIO。

## 后续所需条件

- 本机 Broadwell-EP 需要具备明确 PCI/内部配置寄存器依据的只读后端，以及可以在正常 Windows 安全配置下加载的签名模块。官方 IntelMCHBAR 现有二进制的本机加载失败已经实测。现有 IntelMSR、SMBus 模块没有该时序读取 API。
- 可行的商业方向是取得允许随本软件分发的 SDK、头文件、平台覆盖说明和再分发授权，并要求提供当前运行值、通道范围和完整字段定义。当前没有取得这样的 SDK。
- AMD 需要 Matisse、Vermeer 实机和完整性/精度交叉核验。后端代码可供实测，当前没有兼容性验收依据。

## 资料

- [Intel Xeon E5 v4 官方寄存器资料](https://cdrdv2-public.intel.com/333810/xeon-e5-v4-datasheet-vol-2.pdf)
- [PawnIO 官方签名发行版](https://pawnio.eu/)
- [IntelMCHBAR 0.2.11 源码](https://github.com/namazso/PawnIO.Modules/blob/52a7e536dff3e53c96917a28caac5e0fa6510696/IntelMCHBAR.p)
- [AMD DDR4 位域来源](https://github.com/irusanov/ZenStates-Core/blob/869b7e82572337e5a673e92230edc92bddf0e44b/Dictionaries/DDR4Dictionary.cs)
- [HWiNFO SDK 能力与授权入口](https://www.hwinfo.com/sdk/)
