/**
 * ce-mcp-plugin.c - Cheat Engine MCP Plugin (精简版)
 *
 * 只实现分析类命令，读写/冻结/UI操作全部省略（CE界面直接操作更高效）。
 *
 * TCP协议:
 *   请求:  "COMMAND:param1,param2,...\n"
 *   响应:  "OK:{"key":"value"}\n"  或  "ERR:message\n"
 *
 * 默认监听: 127.0.0.1:8888
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cepluginsdk.h"

#pragma comment(lib, "ws2_32.lib")

/* ========== 全局变量 ========== */
static int selfid;
static ExportedFunctions Exported;

static SOCKET aiSocket = INVALID_SOCKET;
static HANDLE aiThread = NULL;
static volatile BOOL isRunning = FALSE;
static CRITICAL_SECTION aiLock;
static char aiServerIP[16] = "127.0.0.1";
static int aiServerPort = 8888;

/* ========== 命令结构 ========== */
typedef struct {
    char command[64];
    char params[512];
} AICommand;

/* ========== 网络层 (直接复用 CE-MCP-Plugin 的逻辑) ========== */

static BOOL InitWinsock(void) {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
}

static void CleanupWinsock(void) {
    WSACleanup();
}

static BOOL ConnectToAIServer(void) {
    struct addrinfo *result = NULL, *ptr = NULL, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[6];
    sprintf_s(portStr, sizeof(portStr), "%d", aiServerPort);

    if (getaddrinfo(aiServerIP, portStr, &hints, &result) != 0)
        return FALSE;

    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        aiSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (aiSocket == INVALID_SOCKET) continue;

        u_long mode = 1;
        ioctlsocket(aiSocket, FIONBIO, &mode);

        int iResult = connect(aiSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (iResult == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                closesocket(aiSocket); aiSocket = INVALID_SOCKET; continue;
            }

            fd_set writefds, exceptfds;
            FD_ZERO(&writefds); FD_SET(aiSocket, &writefds);
            FD_ZERO(&exceptfds); FD_SET(aiSocket, &exceptfds);
            struct timeval timeout = {3, 0};

            if (select(0, NULL, &writefds, &exceptfds, &timeout) <= 0 ||
                FD_ISSET(aiSocket, &exceptfds)) {
                closesocket(aiSocket); aiSocket = INVALID_SOCKET; continue;
            }
        }

        mode = 0;
        ioctlsocket(aiSocket, FIONBIO, &mode);
        break;
    }

    freeaddrinfo(result);
    return aiSocket != INVALID_SOCKET;
}

static void DisconnectFromAIServer(void) {
    EnterCriticalSection(&aiLock);
    if (aiSocket != INVALID_SOCKET) {
        closesocket(aiSocket);
        aiSocket = INVALID_SOCKET;
    }
    LeaveCriticalSection(&aiLock);
}

/* ========== 响应发送 ========== */

static void SendResponse(const char *type, const char *data) {
    EnterCriticalSection(&aiLock);
    if (aiSocket != INVALID_SOCKET) {
        char buf[4096];
        int len = sprintf_s(buf, sizeof(buf), "%s:%s\n", type, data);
        if (len > 0) send(aiSocket, buf, len, 0);
    }
    LeaveCriticalSection(&aiLock);
}

#define RESP_OK(fmt, ...) do { \
    char _buf[4096]; \
    sprintf_s(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
    SendResponse("OK", _buf); \
} while(0)

#define RESP_ERR(fmt, ...) do { \
    char _buf[1024]; \
    sprintf_s(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
    SendResponse("ERR", _buf); \
} while(0)

/* ========== 命令解析 ========== */

static BOOL ParseAICommand(char *buffer, AICommand *cmd) {
    if (!buffer || !cmd) return FALSE;

    char *colon = strchr(buffer, ':');
    if (!colon) return FALSE;

    *colon = '\0';
    strncpy_s(cmd->command, sizeof(cmd->command), buffer, _TRUNCATE);

    char *params = colon + 1;
    // 去掉末尾换行符
    size_t len = strlen(params);
    while (len > 0 && (params[len-1] == '\n' || params[len-1] == '\r'))
        params[--len] = '\0';

    strncpy_s(cmd->params, sizeof(cmd->params), params, _TRUNCATE);
    return TRUE;
}

/* ========== 辅助函数 ========== */

static UINT_PTR ParseAddr(const char *s) {
    return s ? (UINT_PTR)strtoull(s, NULL, 16) : 0;
}

static char *GetParam(char *params, int idx) {
    static char buf[256];
    char *p = params;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, ',');
        if (!p) return NULL;
        p++;
    }
    char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    len = min(len, sizeof(buf) - 1);
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

/* ========== 命令实现 ========== */

/**
 * PING - 连接测试
 * 请求: PING
 * 响应: OK:pong
 */
static void cmd_PING(AICommand *cmd) {
    if (!Exported.OpenedProcessHandle || !*Exported.OpenedProcessHandle) {
        RESP_OK("pong (no process attached)");
        return;
    }
    DWORD pid = 0;
    if (Exported.GetProcessIDFromProcessHandle) {
        pid = (*Exported.GetProcessIDFromProcessHandle)(*Exported.OpenedProcessHandle);
    }
    RESP_OK("{\"pong\":true,\"pid\":%lu}", pid);
}

/**
 * READ_MEMORY - 读取内存（调试用，读取指定地址附近的反汇编上下文）
 * 请求: READ_MEMORY:address,length
 * 响应: OK:{"bytes":"..."}
 */
static void cmd_READ_MEMORY(AICommand *cmd) {
    char *addrStr = GetParam(cmd->params, 0);
    char *lenStr = GetParam(cmd->params, 1);
    if (!addrStr) { RESP_ERR("missing address"); return; }

    UINT_PTR addr = ParseAddr(addrStr);
    int len = lenStr ? atoi(lenStr) : 256;
    if (len > 4096) len = 4096;

    BYTE buf[4096];
    SIZE_T bytesRead = 0;

    if (!Exported.ReadProcessMemory ||
        !(*Exported.ReadProcessMemory)(*Exported.OpenedProcessHandle, (LPCVOID)addr, buf, len, &bytesRead)) {
        RESP_ERR("read failed at 0x%llX", (unsigned long long)addr);
        return;
    }

    // 转十六进制字符串
    char hex[8192];
    int pos = 0;
    for (SIZE_T i = 0; i < bytesRead && pos < (int)sizeof(hex) - 3; i++) {
        pos += sprintf_s(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
    }

    RESP_OK("{\"address\":\"0x%llX\",\"size\":%llu,\"bytes\":\"%s\"}",
            (unsigned long long)addr, (unsigned long long)bytesRead, hex);
}

/**
 * DISASSEMBLE - 反汇编指定地址附近的代码
 * 请求: DISASSEMBLE:address,count
 * 响应: OK:{"address":"0x...","instructions":[...]}
 */
static void cmd_DISASSEMBLE(AICommand *cmd) {
    char *addrStr = GetParam(cmd->params, 0);
    char *cntStr = GetParam(cmd->params, 1);
    if (!addrStr) { RESP_ERR("missing address"); return; }

    UINT_PTR addr = ParseAddr(addrStr);
    int count = cntStr ? atoi(cntStr) : 20;
    if (count > 100) count = 100;

    if (!Exported.Disassemble) { RESP_ERR("disassembler not available"); return; }

    char result[8192];
    int pos = 0;
    pos += sprintf_s(result + pos, sizeof(result) - pos, "{\"address\":\"0x%llX\",\"instructions\":[",
                     (unsigned long long)addr);

    UINT_PTR current = addr;
    for (int i = 0; i < count; i++) {
        // CE SDK: Disassemble(address, buffer)
        char inst[256];
        int len = (*Exported.Disassemble)(current, inst);
        if (len <= 0) break;

        // 读原始字节
        BYTE raw[16];
        SIZE_T bytesRead = 0;
        if (Exported.ReadProcessMemory) {
            (*Exported.ReadProcessMemory)(*Exported.OpenedProcessHandle, (LPCVOID)current, raw, len, &bytesRead);
        }

        pos += sprintf_s(result + pos, sizeof(result) - pos, "%s{\"offset\":\"0x%llX\",\"bytes\":\"",
                         i > 0 ? "," : "", (unsigned long long)current);
        for (SIZE_T j = 0; j < bytesRead && pos < (int)sizeof(result) - 20; j++)
            pos += sprintf_s(result + pos, sizeof(result) - pos, "%02X", raw[j]);
        pos += sprintf_s(result + pos, sizeof(result) - pos, "\",\"asm\":\"%s\"}", inst);

        current += len;
    }

    pos += sprintf_s(result + pos, sizeof(result) - pos, "]}");
    RESP_OK("%s", result);
}

/**
 * GET_MODULES - 获取进程模块列表
 * 请求: GET_MODULES
 * 响应: OK:{"modules":[{"name":"game.exe","base":"0x...","size":...}]}
 */
static void cmd_GET_MODULES(AICommand *cmd) {
    if (!Exported.GetModuleList) { RESP_ERR("module list not available"); return; }

    char result[8192];
    int pos = 0;
    pos += sprintf_s(result + pos, sizeof(result) - pos, "{\"modules\":[");

    // CE SDK 提供了 GetModuleList 返回 MODULEINFO
    int first = 1;
    for (int i = 0; ; i++) {
        MODULEINFO mi;
        if (!(*Exported.GetModuleList)(i, &mi)) break;

        pos += sprintf_s(result + pos, sizeof(result) - pos,
                         "%s{\"name\":\"%s\",\"base\":\"0x%llX\",\"size\":%lu}",
                         first ? "" : ",",
                         mi.modulename, (unsigned long long)mi.baseaddress, mi.partSize);
        first = 0;
    }

    pos += sprintf_s(result + pos, sizeof(result) - pos, "]}");
    RESP_OK("%s", result);
}

/**
 * GET_REGISTERS - 获取当前调试寄存器快照
 * 请求: GET_REGISTERS
 * 响应: OK:{"rax":"0x...","rbx":"0x...",...}
 */
static void cmd_GET_REGISTERS(AICommand *cmd) {
    if (!Exported.GetContext) { RESP_ERR("debugger not active"); return; }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;

    if (!(*Exported.GetContext)(*Exported.OpenedProcessHandle, &ctx)) {
        RESP_ERR("failed to get context");
        return;
    }

    RESP_OK("{\"rax\":\"0x%llX\",\"rbx\":\"0x%llX\",\"rcx\":\"0x%llX\",\"rdx\":\"0x%llX\","
            "\"rsi\":\"0x%llX\",\"rdi\":\"0x%llX\",\"rbp\":\"0x%llX\",\"rsp\":\"0x%llX\","
            "\"r8\":\"0x%llX\",\"r9\":\"0x%llX\",\"r10\":\"0x%llX\",\"r11\":\"0x%llX\","
            "\"r12\":\"0x%llX\",\"r13\":\"0x%llX\",\"r14\":\"0x%llX\",\"r15\":\"0x%llX\","
            "\"rip\":\"0x%llX\",\"eflags\":\"0x%lX\"}",
#if defined(_M_X64) || defined(_AMD64_)
            (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx,
            (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx,
            (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi,
            (unsigned long long)ctx.Rbp, (unsigned long long)ctx.Rsp,
            (unsigned long long)ctx.R8,  (unsigned long long)ctx.R9,
            (unsigned long long)ctx.R10, (unsigned long long)ctx.R11,
            (unsigned long long)ctx.R12, (unsigned long long)ctx.R13,
            (unsigned long long)ctx.R14, (unsigned long long)ctx.R15,
            (unsigned long long)ctx.Rip, (unsigned long)ctx.EFlags
#else
            (unsigned long long)ctx.Eax, (unsigned long long)ctx.Ebx,
            (unsigned long long)ctx.Ecx, (unsigned long long)ctx.Edx,
            (unsigned long long)ctx.Esi, (unsigned long long)ctx.Edi,
            (unsigned long long)ctx.Ebp, (unsigned long long)ctx.Esp,
            0ULL, 0ULL, 0ULL, 0ULL,
            0ULL, 0ULL, 0ULL, 0ULL,
            (unsigned long long)ctx.Eip, (unsigned long)ctx.EFlags
#endif
            );
}

/**
 * SET_BP - 设置硬件断点（持续监控模式，不中断游戏）
 * 请求: SET_BP:address,type(0=exec/1=write/2=read),duration_seconds
 * 响应: OK:{"records":[...]}  (duration秒后返回)
 */
static void cmd_SET_BP(AICommand *cmd) {
    char *addrStr = GetParam(cmd->params, 0);
    char *typeStr = GetParam(cmd->params, 1);
    char *durStr  = GetParam(cmd->params, 2);

    if (!addrStr) { RESP_ERR("missing address"); return; }

    UINT_PTR addr = ParseAddr(addrStr);
    int type = typeStr ? atoi(typeStr) : 1; // 默认写断点
    int duration = durStr ? atoi(durStr) : 10;
    if (duration > 30) duration = 30;

    if (!Exported.SetBreakpoint || !Exported.RemoveBreakpoint) {
        RESP_ERR("breakpoint API not available");
        return;
    }

    // 设置硬件断点
    int bpId = (*Exported.SetBreakpoint)(0, addr, 4, type); // 4字节
    if (bpId < 0) {
        RESP_ERR("failed to set breakpoint at 0x%llX", (unsigned long long)addr);
        return;
    }

    // 等待触发（注意：CE SDK的硬件断点触发时会暂停调试器）
    // 这里简化处理——等 duration 秒后返回
    Sleep(duration * 1000);

    // 移除断点
    (*Exported.RemoveBreakpoint)(0, bpId);

    RESP_OK("{\"address\":\"0x%llX\",\"type\":%d,\"duration\":%d,\"bp_id\":%d,\"status\":\"completed\"}",
            (unsigned long long)addr, type, duration, bpId);
}

/**
 * AOB_SCAN - 特征码搜索
 * 请求: AOB_SCAN:pattern,module_name(可选)
 * 响应: OK:{"matches":["0x..."]}
 */
static void cmd_AOB_SCAN(AICommand *cmd) {
    char *pattern = GetParam(cmd->params, 0);
    char *moduleName = GetParam(cmd->params, 1);

    if (!pattern) { RESP_ERR("missing pattern"); return; }

    // 先找模块基址
    UINT_PTR base = 0;
    SIZE_T size = 0;

    if (moduleName && Exported.GetModuleList) {
        for (int i = 0; ; i++) {
            MODULEINFO mi;
            if (!(*Exported.GetModuleList)(i, &mi)) break;
            if (_stricmp(mi.modulename, moduleName) == 0) {
                base = (UINT_PTR)mi.baseaddress;
                size = mi.partSize;
                break;
            }
        }
    }

    if (base == 0) {
        // 从主模块开始
        MODULEINFO mi;
        if (Exported.GetModuleList && (*Exported.GetModuleList)(0, &mi)) {
            base = (UINT_PTR)mi.baseaddress;
            size = mi.partSize;
        }
    }

    if (base == 0) { RESP_ERR("no module found"); return; }

    // 解析 pattern 字符串 "AA BB ?? DD" -> 字节数组 + 掩码
    BYTE pat[128];
    BYTE mask[128];
    int patLen = 0;

    char *tok = strtok(pattern, " ");
    while (tok && patLen < 128) {
        if (strcmp(tok, "??") == 0 || strcmp(tok, "?") == 0) {
            mask[patLen] = 0;
            pat[patLen] = 0;
        } else {
            mask[patLen] = 1;
            pat[patLen] = (BYTE)strtoul(tok, NULL, 16);
        }
        patLen++;
        tok = strtok(NULL, " ");
    }

    // 简单线性扫描
    BYTE buf[4096];
    char results[4096];
    int pos = 0;
    int found = 0;
    pos += sprintf_s(results + pos, sizeof(results) - pos, "{\"pattern\":\"%s\",\"matches\":[", pattern);

    for (UINT_PTR cur = base; cur < base + size - patLen; cur += sizeof(buf) - patLen) {
        SIZE_T readSize = min(sizeof(buf), base + size - cur);
        SIZE_T bytesRead = 0;

        if (!Exported.ReadProcessMemory ||
            !(*Exported.ReadProcessMemory)(*Exported.OpenedProcessHandle, (LPCVOID)cur, buf, readSize, &bytesRead))
            break;

        for (SIZE_T i = 0; i < bytesRead - patLen + 1; i++) {
            BOOL match = TRUE;
            for (int j = 0; j < patLen; j++) {
                if (mask[j] && buf[i + j] != pat[j]) { match = FALSE; break; }
            }
            if (match && found < 50) {
                if (found > 0) pos += sprintf_s(results + pos, sizeof(results) - pos, ",");
                pos += sprintf_s(results + pos, sizeof(results) - pos, "\"0x%llX\"", (unsigned long long)(cur + i));
                found++;
            }
        }

        if (cur + bytesRead >= base + size) break;
    }

    pos += sprintf_s(results + pos, sizeof(results) - pos, "],\"count\":%d}", found);
    RESP_OK("%s", results);
}

/* ========== 命令分发 ========== */

static void ExecuteAICommand(AICommand *cmd) {
    if (!cmd || !cmd->command[0]) return;

    if (strcmp(cmd->command, "PING") == 0)               cmd_PING(cmd);
    else if (strcmp(cmd->command, "READ_MEMORY") == 0)    cmd_READ_MEMORY(cmd);
    else if (strcmp(cmd->command, "DISASSEMBLE") == 0)    cmd_DISASSEMBLE(cmd);
    else if (strcmp(cmd->command, "GET_MODULES") == 0)    cmd_GET_MODULES(cmd);
    else if (strcmp(cmd->command, "GET_REGISTERS") == 0)  cmd_GET_REGISTERS(cmd);
    else if (strcmp(cmd->command, "SET_BP") == 0)         cmd_SET_BP(cmd);
    else if (strcmp(cmd->command, "AOB_SCAN") == 0)       cmd_AOB_SCAN(cmd);
    else RESP_ERR("unknown command: %s", cmd->command);
}

/* ========== AI 通信线程 ========== */

static DWORD WINAPI AIThreadProc(LPVOID lpParam) {
    (void)lpParam;
    ConnectToAIServer();

    while (isRunning) {
        EnterCriticalSection(&aiLock);
        SOCKET sock = aiSocket;
        LeaveCriticalSection(&aiLock);

        if (sock == INVALID_SOCKET) {
            Sleep(500);
            ConnectToAIServer();
            continue;
        }

        char buf[1024];
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len > 0) {
            buf[len] = '\0';
            AICommand cmd;
            if (ParseAICommand(buf, &cmd))
                ExecuteAICommand(&cmd);
        } else {
            DisconnectFromAIServer();
        }
    }
    return 0;
}

/* ========== CE 插件生命周期 ========== */

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD reason, LPVOID lpReserved) {
    (void)hModule; (void)lpReserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH: InitWinsock(); break;
        case DLL_PROCESS_DETACH: CleanupWinsock(); break;
    }
    return TRUE;
}

BOOL __stdcall CEPlugin_GetVersion(PPluginVersion pv, int sizeofpluginversion) {
    (void)sizeofpluginversion;
    pv->version = CESDK_VERSION;
    pv->pluginname = "CE MCP Plugin (analysis edition) v0.1";
    return TRUE;
}

BOOL __stdcall CEPlugin_InitializePlugin(PExportedFunctions ef, int pluginid) {
    selfid = pluginid;
    memcpy(&Exported, ef, sizeof(ExportedFunctions));

    InitializeCriticalSection(&aiLock);
    isRunning = TRUE;
    aiThread = CreateThread(NULL, 0, AIThreadProc, NULL, 0, NULL);
    return TRUE;
}

BOOL __stdcall CEPlugin_DisablePlugin(void) {
    isRunning = FALSE;
    if (aiThread) {
        WaitForSingleObject(aiThread, 5000);
        CloseHandle(aiThread);
        aiThread = NULL;
    }
    DisconnectFromAIServer();
    DeleteCriticalSection(&aiLock);
    return TRUE;
}
