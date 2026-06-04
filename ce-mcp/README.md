# ce-mcp

Cheat Engine 内存分析 MCP Server，为 AI 编程助手（Claude Code）提供游戏逆向和内存分析能力。

## 功能概述

ce-mcp 由两部分组成：

1. **CE 插件 DLL**（C 语言）—— 嵌入 Cheat Engine 进程，调用 CE 7.5 SDK 提供的调试器、反汇编器、内存扫描等底层能力
2. **Python MCP Server**（Python）—— 通过 TCP Bridge 与 CE 插件通信，向 Claude Code 暴露 23 个 MCP 工具

### 通信架构

```
CE (插件 DLL) --TCP Client--> Python MCP Server (bridge.py) --stdio--> Claude Code
      127.0.0.1:8888                            ce-mcp MCP Server
```

CE 插件作为 TCP **客户端**，启动时自动连接 Python MCP Server。MCP Server 再通过 stdio 与 Claude Code 通信。

### 设计原则

只实现**分析类命令**。读写、冻结、UI 操作在 CE 界面直接操作更高效，AI 的价值在于分析和决策。

## MCP 工具列表（23 个）

### 状态与进程

| 工具 | 说明 | 对应命令 |
|------|------|---------|
| `ce_status` | 检查 CE 插件连接状态和当前附加进程 | （本地） |
| `ce_ping` | 测试 CE 插件连接，返回进程信息 | PING |
| `ce_get_process_list` | 列出系统所有运行进程（PID + 名称） | GET_PROCESS_LIST |
| `ce_get_modules` | 目标进程模块列表（名称 / 基址 / 大小） | GET_MODULES |

### 寄存器与调用栈

| 工具 | 说明 | 对应命令 |
|------|------|---------|
| `ce_get_registers` | 当前调试寄存器快照 | GET_REGISTERS |
| `ce_get_callstack` | 当前线程调用栈（地址 + 模块名） | GET_CALLSTACK |
| `ce_read_memory` | 读取指定地址内存数据（hex） | READ_MEMORY |

### 符号与内存布局

| 工具 | 说明 | 对应命令 |
|------|------|---------|
| `ce_get_symbol_info` | 符号名 <-> 地址双向解析（模块/导出/PDB） | GET_SYMBOL_INFO |
| `ce_enum_memory_regions` | 枚举进程完整内存布局（基址/大小/保护/类型） | ENUM_MEMORY_REGIONS |
| `ce_enum_strings` | 扫描内存中可打印字符串（类似 Unix strings） | ENUM_STRINGS |
| `ce_get_rtti_class` | C++ RTTI 类名解析（vtable -> RTTI 链） | GET_RTTI_CLASS |
| `ce_resolve_pointer` | 多级指针链：base + offsets → 最终地址 | RESOLVE_POINTER |

### 代码分析

| 工具 | 说明 | 对应命令 |
|------|------|---------|
| `ce_disassemble` | 反汇编指定地址处的代码 | DISASSEMBLE |
| `ce_assemble` | 汇编单条指令 → 机器码字节 | ASSEMBLE |
| `ce_prev_opcode` | 向前查找相邻指令地址 | PREV_OPCODE |
| `ce_next_opcode` | 向后查找相邻指令地址 | NEXT_OPCODE |

### 断点与追踪

| 工具 | 说明 | 对应命令 |
|------|------|---------|
| `ce_set_breakpoint` | 硬件断点监控（命中记录含寄存器 + 调用栈 + RIP 分组） | SET_BP |
| `ce_register_trace` | 跨函数寄存器追踪（入口/出口配对 + diff） | REGISTER_TRACE |

### 扫描与注入

| 工具 | 说明 | 对应命令 |
|------|------|---------|
| `ce_aob_scan` | AOB 特征码搜索（支持 `??` 通配符） | AOB_SCAN |
| `ce_memory_scan` | 精确值扫描（byte/word/dword/qword/float/double/string） | MEMORY_SCAN |
| `ce_memory_scan_next` | 变/不变过滤链（链式调用逐步缩小范围） | MEMORY_SCAN_NEXT |
| `ce_generate_hook` | 手动生成 AutoAssemble 注入脚本 | GENERATE_HOOK |
| `ce_generate_api_hook` | CE 内置 API Hook 脚本生成（更可靠） | GENERATE_API_HOOK |

## 快速开始

### 环境要求

- Python 3.10+
- Windows 系统
- Cheat Engine 7.5+
- 预编译的插件 DLL（`ce-mcp/ce-plugin/` 下已提供 x64/x86 版本）

### 步骤 1：复制 DLL 到 CE 插件目录

将 `ce-mcp/ce-plugin/ce-mcp-plugin-x64.dll`（或 x86 版本）复制到 Cheat Engine 安装目录下：

```
C:\Program Files\Cheat Engine 7.5\autorun\dll\
```

> 如果没有 `autorun\dll` 目录，手动创建即可。CE 启动时会自动扫描该目录并加载 DLL 插件。

### 步骤 2：启动 Python MCP Bridge

打开终端，进入项目根目录：

```cmd
cd C:\Users\scydr\Desktop\123\nixiang-mcp
venv\Scripts\python -m ce-mcp.src.server
```

看到类似输出说明 Bridge 已在 `127.0.0.1:8888` 等待 CE 插件连接。

### 步骤 3：打开 Cheat Engine 附加进程

1. 启动 Cheat Engine 7.5
2. 通过 File → Open Process 附加目标进程
3. CE 的 Plugin 菜单中应能看到 **"CE MCP Plugin v0.4"**
4. 插件启动后会自动连接 `127.0.0.1:8888`

### 步骤 4：在 Claude Code 中使用

在项目根目录启动 Claude Code：

```cmd
cd C:\Users\scydr\Desktop\123\nixiang-mcp
claude
```

输入 `/mcp` 确认 `ce-mcp` 状态为已连接，然后可以直接使用工具：

```
ce_ping
ce_get_modules
ce_disassemble 0x7FF12345678
```

## 快速验证（不启动 CE）

可以只用 telnet/nc 模拟 CE 插件连接，验证 Python Bridge 是否正常：

```cmd
:: 终端 1 - 启动 Bridge
cd C:\Users\scydr\Desktop\123\nixiang-mcp
venv\Scripts\python -m ce-mcp.src.server

:: 终端 2 - 模拟 CE 插件连接
telnet 127.0.0.1 8888
:: 连接成功后输入:
PING:
:: 应收到 OK:{"pong":true,...} 响应
```

## 协议

- `\n` 分隔请求和响应
- 请求：`COMMAND:param1,param2,...\n`
- 响应：`OK:{"key":"value"}\n` 或 `ERR:message\n`

## 项目结构

```
ce-mcp/
├── README.md                  # 主文档（当前文件）
├── ce-plugin/
│   ├── README.md              # 插件详细文档（含编译说明）
│   ├── plugin.h               # 公共头文件（类型定义、ppinter 宏、函数声明）
│   ├── plugin-core.c          # 核心框架（网络/解析/生命周期/命令分发）
│   ├── plugin-debug.c         # 调试追踪（SET_BP / GET_REGISTERS / GET_CALLSTACK / REGISTER_TRACE）
│   ├── plugin-analyze.c       # 分析命令（14 条：DISASSEMBLE / AOB_SCAN / PREV_OPCODE / ...）
│   ├── plugin-scan.c          # 内存扫描（MEMORY_SCAN / MEMORY_SCAN_NEXT）
│   ├── plugin-gen.c           # 脚本生成（GENERATE_HOOK / GENERATE_API_HOOK）
│   ├── ce-mcp-plugin.def      # DLL 导出符号
│   ├── ce-mcp-plugin-x64.dll  # 预编译 x64 DLL
│   ├── ce-mcp-plugin-x86.dll  # 预编译 x86 DLL
│   └── sdk/                   # CE 7.5 SDK + Lua 头文件
└── src/
    ├── server.py             # MCP Server（23 个工具）
    ├── bridge.py             # TCP 桥接
    └── __init__.py
```

## 编译插件

详见 [ce-plugin/README.md](ce-plugin/README.md)。

```cmd
:: 进入插件目录
cd /d C:\Users\scydr\Desktop\123\nixiang-mcp\ce-mcp\ce-plugin

:: 清理旧编译产物
del /q *.obj *.lib *.exp 2>nul

:: x64 发布版
cl /utf-8 /TC /LD /O2 plugin-core.c plugin-debug.c plugin-analyze.c plugin-scan.c plugin-gen.c /Fe:ce-mcp-plugin-x64.dll /link ws2_32.lib dbghelp.lib /DEF:ce-mcp-plugin.def

:: x64 调试版（出问题时用 VS 断点排查）
cl /utf-8 /TC /LD /Od /Zi plugin-core.c plugin-debug.c plugin-analyze.c plugin-scan.c plugin-gen.c /Fe:ce-mcp-plugin-x64-debug.dll /link ws2_32.lib dbghelp.lib /DEF:ce-mcp-plugin.def
```

x86 编译需要在 **x86 Native Tools Command Prompt for VS 2022** 中执行。

## 相关文档

| 文档 | 说明 |
|------|------|
| [ce-plugin/README.md](ce-plugin/README.md) | CE 插件详细文档（编译、API 兼容性、命令列表） |
| [../docs/ce-design.md](../docs/ce-design.md) | CE MCP 后端架构和场景设计 |
| [../docs/ce-prompt.md](../docs/ce-prompt.md) | CE 逆向分析提示词参考 |
| [../README.md](../README.md) | 项目总览（nixiang-mcp） |
