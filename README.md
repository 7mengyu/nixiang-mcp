# Nixiang MCP

逆向工具 MCP Server 集合，为 AI 编程助手提供 .NET 程序集分析和 Cheat Engine 内存分析能力。

## 项目结构

```
nixiang-mcp/
├── venv/                  ← 所有 MCP 共用的 Python 虚拟环境
├── dnlib-mcp/             ← .NET 程序集静态分析（dnlib + de4dot）
├── ce-mcp/                ← Cheat Engine 分析插件 + MCP 后端
├── docs/                  ← 设计文档和提示词
├── .mcp.json              ← MCP Server 注册配置（入口）
├── requirements.txt       ← Python 依赖
└── setup.bat              ← 一键安装脚本
```

## 快速开始

### 安装

```bash
git clone https://github.com/7mengyu/nixiang-mcp.git
cd nixiang-mcp

# 创建虚拟环境并安装依赖
python -m venv venv
venv\Scripts\pip install -r requirements.txt
```

虚拟环境创建在项目根目录，`dnlib-mcp` 和 `ce-mcp` 共用同一个 venv。

### 启动

在项目根目录启动 Claude Code：

```bash
claude
```

`.mcp.json` 会自动注册 `dnlib-mcp` 和 `ce-mcp` 两个 MCP Server。运行 `/mcp` 查看连接状态。

## 子项目

### dnlib-mcp

.NET 程序集静态分析，支持：
- 类型/方法搜索和 IL 反编译
- de4dot 混淆器检测和解混淆（22 种混淆器）
- 存档加密密钥提取

详见 [dnlib-mcp/README.md](dnlib-mcp/README.md)

### ce-mcp

Cheat Engine 分析插件，通过 CE 插件 DLL + TCP 桥接实现 AI 辅助游戏逆向：
- 进程模块枚举和寄存器快照
- 反汇编和 AOB 特征码扫描
- 硬件断点持续监控（写入/执行/读取路径分析）
- 内存数据读取

详见 [ce-mcp/ce-plugin/README.md](ce-mcp/ce-plugin/README.md)

## 文档

| 文档 | 说明 |
|------|------|
| [CE 设计文档](docs/ce-design.md) | CE MCP 后端架构和场景设计 |
| [CE 提示词](docs/ce-prompt.md) | CE 逆向分析提示词参考 |

## 环境要求

- Python 3.10+
- Windows 系统
- .NET 运行时（dnlib-mcp 需要）
- Cheat Engine 7.5+（ce-mcp 需要）
