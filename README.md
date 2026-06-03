# Nixiang MCP

逆向工具 MCP Server 集合，为 AI 编程助手提供 .NET 程序集分析和 Cheat Engine 内存分析能力。

## 项目结构

```
nixiang-mcp/
├── dnlib-mcp/     ← .NET 程序集静态分析（dnlib + de4dot）
├── ce-mcp/        ← Cheat Engine 分析插件 + MCP 后端
└── docs/          ← 设计文档和提示词
```

## 快速开始

### 安装

```bash
git clone https://github.com/7mengyu/nixiang-mcp.git
cd nixiang-mcp

# 创建虚拟环境并安装依赖
python -m venv venv
venv\Scripts\pip install -r dnlib-mcp/requirements.txt
```

### 启动

在项目目录启动 Claude Code：

```bash
claude
```

`.mcp.json` 会自动注册 `dnlib-mcp` 服务器。

## 子项目

### dnlib-mcp

.NET 程序集静态分析，支持：
- 类型/方法搜索和 IL 反编译
- de4dot 混淆器检测和解混淆（22 种混淆器）
- 存档加密密钥提取

详见 [dnlib-mcp/README.md](dnlib-mcp/README.md)

### ce-mcp（开发中）

Cheat Engine 分析插件，通过 TCP 桥接实现 AI 辅助游戏逆向：
- 断点持续监控和写入路径分析
- 汇编分析找基址
- 加密数值反推算法
- AOB + 注入脚本生成

详见 [docs/ce-design.md](docs/ce-design.md)

## 文档

| 文档 | 说明 |
|------|------|
| [CE 设计文档](docs/ce-design.md) | CE MCP 后端架构和场景设计 |
| [CE 提示词](docs/ce-prompt.md) | CE 逆向分析提示词参考 |

## 环境要求

- Python 3.10+
- Windows 系统
- .NET 运行时
- Cheat Engine 7.0+（CE 分析功能需要）