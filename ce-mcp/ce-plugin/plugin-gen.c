/**
 * plugin-gen.c — 脚本生成命令
 *
 * GENERATE_HOOK
 */

#include "plugin.h"

/**
 * GENERATE_HOOK:address,jump_to(optional),codecave_size(optional)
 *
 * Generates an AutoAssemble script that hooks the instruction at `address`.
 * If `jump_to` is provided, the hook redirects to that address/label.
 * The script includes ENABLE and DISABLE sections with original code restored.
 *
 * 使用 Zydis 解码目标地址指令，不依赖 CE 的 Disassembler（CE 7.5 缺陷）。
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

#ifdef _WIN64
    int minHookLen = 14; /* absolute jmp [rip+disp] */
#else
    int minHookLen = 5;  /* E9 relative jmp */
#endif

    /* 初始化 Zydis decoder */
    ZydisDecoder decoder;
#ifdef _WIN64
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif

    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE);
    ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SIZE, ZYAN_FALSE);

    /* Accumulate instructions to cover minHookLen bytes */
    int hookSize = 0;
    UINT_PTR cur = addr;
    while (cur < addr + 64 && hookSize < minHookLen) {
        BYTE code[16];
        SIZE_T bytesRead = 0;
        if (!RPM(*Exported.OpenedProcessHandle, (LPCVOID)cur, code,
                 sizeof(code), &bytesRead) || bytesRead == 0)
            break;

        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(&decoder, code, bytesRead,
                                                    &insn, operands);
        if (!ZYAN_SUCCESS(status) || insn.length == 0) break;

        hookSize += insn.length;
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

    /* Emit original instruction bytes (Zydis decode) */
    cur = addr;
    while (cur < addr + (UINT_PTR)hookSize && pos < (int)sizeof(script) - 200) {
        BYTE code[16];
        SIZE_T bytesRead = 0;
        if (!RPM(*Exported.OpenedProcessHandle, (LPCVOID)cur, code,
                 sizeof(code), &bytesRead) || bytesRead == 0)
            break;

        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(&decoder, code, bytesRead,
                                                    &insn, operands);
        if (!ZYAN_SUCCESS(status) || insn.length == 0) break;

        char asmBuf[256];
        ZydisFormatterFormatInstruction(&formatter, &insn, operands,
                                         insn.operand_count_visible,
                                         asmBuf, sizeof(asmBuf),
                                         cur, ZYAN_NULL);

        pos += sprintf_s(script + pos, sizeof(script) - pos, "  ");
        for (ZyanU8 b = 0; b < insn.length; b++)
            pos += sprintf_s(script + pos, sizeof(script) - pos,
                             "%s%02X", b > 0 ? " " : "", code[b]);
        pos += sprintf_s(script + pos, sizeof(script) - pos, " // %s\n", asmBuf);
        cur += insn.length;
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
        BYTE code[16];
        SIZE_T bytesRead = 0;
        if (!RPM(*Exported.OpenedProcessHandle, (LPCVOID)cur, code,
                 sizeof(code), &bytesRead) || bytesRead == 0)
            break;

        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(&decoder, code, bytesRead,
                                                    &insn, operands);
        if (!ZYAN_SUCCESS(status) || insn.length == 0) break;

        char asmBuf[256];
        ZydisFormatterFormatInstruction(&formatter, &insn, operands,
                                         insn.operand_count_visible,
                                         asmBuf, sizeof(asmBuf),
                                         cur, ZYAN_NULL);

        pos += sprintf_s(script + pos, sizeof(script) - pos, "  ");
        for (ZyanU8 b = 0; b < insn.length; b++)
            pos += sprintf_s(script + pos, sizeof(script) - pos,
                             "%s%02X", b > 0 ? " " : "", code[b]);
        pos += sprintf_s(script + pos, sizeof(script) - pos, " // %s\n", asmBuf);
        cur += insn.length;
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

/* cmd_GENERATE_API_HOOK removed in v0.5.2 — ce_generate_hook 替代 */
