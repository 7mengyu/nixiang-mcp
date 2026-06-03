# ce-mcp-plugin

Cheat Engine MCP 插件（精简分析版），为 AI 助手提供 CE 分析能力。

## 设计原则

只实现**分析类命令**。读写/冻结/UI操作在 CE 界面直接操作更高效，AI 的价值在于分析和决策。

## 编译

```bash
# Visual Studio Developer Command Prompt
cl /LD /O2 ce-mcp-plugin.c /Fe:ce-mcp-plugin.dll /link ws2_32.lib /DEF:ce-mcp-plugin.def
```

或使用配套的 CMakeLists.txt / vcxproj。

## 支持的命令

| 命令 | 说明 | 请求示例 |
|------|------|---------|
| PING | 连接测试 | `PING:\n` |
| DISASSEMBLE | 反汇编 | `DISASSEMBLE:0x7FF6A0001000,30\n` |
| GET_MODULES | 进程模块列表 | `GET_MODULES:\n` |
| GET_REGISTERS | 寄存器快照 | `GET_REGISTERS:\n` |
| READ_MEMORY | 读内存(调试用) | `READ_MEMORY:0x7FF6A0001000,256\n` |
| SET_BP | 硬件断点(持续监控) | `SET_BP:0x1A2B3C4D,1,15\n` |
| AOB_SCAN | 特征码搜索 | `AOB_SCAN:48 8B 05 ?? ?? ?? ??,game.exe\n` |

## 协议

- `\n` 分隔请求和响应
- 请求: `COMMAND:param1,param2,...\n`
- 响应: `OK:{"key":"value"}\n` 或 `ERR:message\n`

## 与 CE-MCP-Plugin 的区别

| 特性 | CE-MCP-Plugin (原版) | ce-mcp-plugin (本插件) |
|------|---------------------|----------------------|
| 命令数 | 75 | 7 |
| 响应方式 | ShowMessage弹窗 | TCP 结构化 JSON 返回 |
| UI操作(窗口/按钮/标签) | 有 | 无 |
| 内存扫描(值/变/不变) | 无 | 无 |
| 内存读写 | 有 | 无(CE界面操作) |
| 断点持续监控 | 无 | 有(SET_BP) |
| AOB搜索 | 无 | 有 |
| 寄存器快照 | 无 | 有 |

## 未实现（TODO）

- [ ] 内存扫描（精确值/变/不变过滤链）
- [ ] 持续断点监控自动分组（按指令地址 + 调用栈）
- [ ] 调用栈获取
- [ ] 跨函数寄存器追踪
- [ ] AOB + 注入脚本自动生成
