/**
 * plugin-gen.c — 脚本生成命令
 *
 * GENERATE_HOOK / GENERATE_API_HOOK
 */

#include "plugin.h"

/**
 * GENERATE_HOOK:address,jump_to(optional),codecave_size(optional)
 *
 * Generates an AutoAssemble script that hooks the instruction at `address`.
 * If `jump_to` is provided, the hook redirects to that address/label.
 * The script includes ENABLE and DISABLE sections with original code restored.
 */
void cmd_GENERATE_HOOK(Command *cmd) {
    char *addrStr     = GetParam(cmd->params, 0);
    char *jumpToStr   = GetParam(cmd->params, 1);
    char *caveSizeStr = GetParam(cmd->params, 2);

    if (!addrStr) { ERR("missing address"); return; }

    UINT_PTR addr = ParseAddr(addrStr);
    int caveSize = caveSizeStr ? atoi(caveSizeStr) : 1024;
    if (caveSize < 64) caveSize = 64;
    if (caveSize > 65536) caveSize = 65536;

    if (!Exported.Disassembler) {
        ERR("disassembler not available");
        return;
    }

#ifdef _WIN64
    int minHookLen = 14; /* absolute jmp [rip+disp] */
#else
    int minHookLen = 5;  /* E9 relative jmp */
#endif

    /* Accumulate instructions to cover minHookLen bytes */
    int hookSize = 0;
    UINT_PTR cur = addr;
    while (cur < addr + 64 && hookSize < minHookLen) {
        char inst[256] = {0};
        if (!Exported.Disassembler(cur, inst, sizeof(inst) - 1))
            break;
        UINT_PTR nextCur = cur;
        if (Exported.disassembleEx)
            Exported.disassembleEx((UINT_PTR)&nextCur, NULL, 0);
        int len = (int)(nextCur - cur);
        if (len <= 0) break;
        hookSize += len;
        cur = addr + hookSize;
    }

    if (hookSize < minHookLen) {
        ERR("cannot accumulate enough bytes for hook (need %d, got %d)",
            minHookLen, hookSize);
        return;
    }

    char labelPrefix[32];
    sprintf_s(labelPrefix, sizeof(labelPrefix), "mcp_hook_%llX",
              (unsigned long long)addr);

    /* Build script */
    char script[8192];
    int pos = 0;
    pos += sprintf_s(script + pos, sizeof(script) - pos, "[ENABLE]\n");
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "// CE-MCP auto-generated hook at 0x%llX\n", (unsigned long long)addr);
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "alloc(%s_codecave,%d)\n", labelPrefix, caveSize);
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "label(%s_original)\n", labelPrefix);
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "label(%s_return)\n\n", labelPrefix);

    if (jumpToStr && jumpToStr[0]) {
        pos += sprintf_s(script + pos, sizeof(script) - pos,
            "%s:\n", labelPrefix);
        pos += sprintf_s(script + pos, sizeof(script) - pos,
            "  push rax\n");
        pos += sprintf_s(script + pos, sizeof(script) - pos,
            "  mov rax,%s\n", jumpToStr);
        pos += sprintf_s(script + pos, sizeof(script) - pos,
            "  call rax\n");
        pos += sprintf_s(script + pos, sizeof(script) - pos,
            "  pop rax\n");
    } else {
        pos += sprintf_s(script + pos, sizeof(script) - pos,
            "%s:\n", labelPrefix);
        pos += sprintf_s(script + pos, sizeof(script) - pos,
            "  // <-- insert your custom code here\n");
    }

    pos += sprintf_s(script + pos, sizeof(script) - pos, "\n");
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "%s_original:\n", labelPrefix);

    /* Emit original instruction bytes */
    cur = addr;
    while (cur < addr + (UINT_PTR)hookSize && pos < (int)sizeof(script) - 200) {
        char inst[256] = {0};
        if (!Exported.Disassembler(cur, inst, sizeof(inst) - 1))
            break;

        UINT_PTR nextCur = cur;
        if (Exported.disassembleEx)
            Exported.disassembleEx((UINT_PTR)&nextCur, NULL, 0);
        int len = (int)(nextCur - cur);
        if (len <= 0) break;

        BYTE raw[16] = {0};
        SIZE_T bytesRead = 0;
        RPM(*Exported.OpenedProcessHandle,
            (LPCVOID)cur, raw, len, &bytesRead);
        pos += sprintf_s(script + pos, sizeof(script) - pos, "  ");
        for (SIZE_T b = 0; b < bytesRead; b++)
            pos += sprintf_s(script + pos, sizeof(script) - pos,
                             "%s%02X", b > 0 ? " " : "", raw[b]);
        pos += sprintf_s(script + pos, sizeof(script) - pos, " // %s\n", inst);
        cur += len;
    }
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "  jmp %s_return\n\n", labelPrefix);

    /* Write the hook jump at the target address */
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "%s+%X:\n", labelPrefix, 0);
#ifdef _WIN64
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "  db EB 0E  // jmp %s (14 bytes, absolute)\n", labelPrefix);
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "  dq %s\n", labelPrefix);
#else
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "  jmp %s  // 5-byte near jump\n", labelPrefix);
#endif
    /* Pad remaining overwritten bytes with nop */
    {
        int jmpSize;
#ifdef _WIN64
        jmpSize = 14; /* db EB 0E + dq addr */
#else
        jmpSize = 5;  /* E9 rel32 */
#endif
        int pad = hookSize - jmpSize;
        for (int n = 0; n < pad && n < 50; n++)
            pos += sprintf_s(script + pos, sizeof(script) - pos, "  nop\n");
    }

    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "%s_return:\n\n", labelPrefix);
    pos += sprintf_s(script + pos, sizeof(script) - pos, "[DISABLE]\n");

    /* Restore original bytes at the hook site */
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "%s+%X:\n", labelPrefix, 0);

    cur = addr;
    while (cur < addr + (UINT_PTR)hookSize && pos < (int)sizeof(script) - 200) {
        char inst[256] = {0};
        if (!Exported.Disassembler(cur, inst, sizeof(inst) - 1))
            break;

        UINT_PTR nextCur = cur;
        if (Exported.disassembleEx)
            Exported.disassembleEx((UINT_PTR)&nextCur, NULL, 0);
        int len = (int)(nextCur - cur);
        if (len <= 0) break;

        BYTE raw[16] = {0};
        SIZE_T bytesRead = 0;
        RPM(*Exported.OpenedProcessHandle,
            (LPCVOID)cur, raw, len, &bytesRead);
        pos += sprintf_s(script + pos, sizeof(script) - pos, "  ");
        for (SIZE_T b = 0; b < bytesRead; b++)
            pos += sprintf_s(script + pos, sizeof(script) - pos,
                             "%s%02X", b > 0 ? " " : "", raw[b]);
        pos += sprintf_s(script + pos, sizeof(script) - pos, " // %s\n", inst);
        cur += len;
    }
    pos += sprintf_s(script + pos, sizeof(script) - pos,
        "dealloc(%s_codecave)\n", labelPrefix);

    /* JSON-escape the script */
    char jsonScript[16384];
    int jpos = 0;
    for (int i = 0; script[i] && jpos < (int)sizeof(jsonScript) - 4; i++) {
        switch (script[i]) {
            case '"':  jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\\""); break;
            case '\\': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\\\"); break;
            case '\n': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\n"); break;
            case '\r': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\r"); break;
            case '\t': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\t"); break;
            default:
                if (script[i] >= 0x20 && script[i] < 0x7F)
                    jsonScript[jpos++] = script[i];
        }
    }
    jsonScript[jpos] = '\0';

    OK("{\"address\":\"0x%llX\",\"hook_size\":%d,\"min_hook_len\":%d,"
       "\"label_prefix\":\"%s\",\"assembly_script\":\"%s\"}",
       (unsigned long long)addr, hookSize, minHookLen,
       labelPrefix, jsonScript);
}

/**
 * GENERATE_API_HOOK:address,jump_to(optional)
 *
 * Calls CE's built-in generateAPIHookScript. Returns the complete
 * AutoAssemble hook script. More reliable than manual GENERATE_HOOK
 * for API-level hooks.
 */
void cmd_GENERATE_API_HOOK(Command *cmd) {
    char *addrStr   = GetParam(cmd->params, 0);
    char *jumpToStr = GetParam(cmd->params, 1);

    if (!addrStr) { ERR("missing address"); return; }

    if (!Exported.sym_generateAPIHookScript) {
        ERR("sym_generateAPIHookScript not available");
        return;
    }

    char script[4096];
    ZeroMemory(script, sizeof(script));
    BOOL ok = Exported.sym_generateAPIHookScript(
        addrStr,                                    /* address (can be string/symbol) */
        jumpToStr && jumpToStr[0] ? jumpToStr : "", /* jump target */
        "",                                         /* new call address (unused) */
        script,
        (int)sizeof(script) - 1);

    if (!ok) {
        ERR("generateAPIHookScript failed for %s", addrStr);
        return;
    }

    /* JSON-escape the script */
    char jsonScript[8192];
    int jpos = 0;
    for (int i = 0; script[i] && jpos < (int)sizeof(jsonScript) - 4; i++) {
        switch (script[i]) {
            case '"':  jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\\""); break;
            case '\\': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\\\"); break;
            case '\n': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\n"); break;
            case '\r': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\r"); break;
            case '\t': jpos += sprintf_s(jsonScript + jpos, sizeof(jsonScript) - jpos, "\\t"); break;
            default:
                if (script[i] >= 0x20 && script[i] < 0x7F)
                    jsonScript[jpos++] = script[i];
        }
    }
    jsonScript[jpos] = '\0';

    OK("{\"address\":\"%s\",\"script\":\"%s\"}", addrStr, jsonScript);
}
