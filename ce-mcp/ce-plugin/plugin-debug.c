/**
 * plugin-debug.c — 调试追踪命令
 *
 * SET_BP / GET_REGISTERS / GET_CALLSTACK / REGISTER_TRACE
 */

#include "plugin.h"

#define MAX_TRACE_PAIRS 100

/**
 * GET_REGISTERS
 * Returns current register values from the debugged thread.
 */
void cmd_GET_REGISTERS(Command *cmd) {
    (void)cmd;

    if (!Exported.GetThreadContext) {
        ERR("GetThreadContext not available");
        return;
    }

    HANDLE hThread = GetDebugThread();
    if (!hThread) {
        ERR("cannot open debug thread - is a process being debugged?");
        return;
    }

    CONTEXT ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;

    BOOL ok = GTC(hThread, &ctx);
    CloseHandle(hThread);

    if (!ok) { ERR("GetThreadContext failed"); return; }

#ifdef _WIN64
    OK("{\"rax\":\"0x%llX\",\"rbx\":\"0x%llX\",\"rcx\":\"0x%llX\",\"rdx\":\"0x%llX\","
       "\"rsi\":\"0x%llX\",\"rdi\":\"0x%llX\",\"rbp\":\"0x%llX\",\"rsp\":\"0x%llX\","
       "\"r8\":\"0x%llX\",\"r9\":\"0x%llX\",\"r10\":\"0x%llX\",\"r11\":\"0x%llX\","
       "\"r12\":\"0x%llX\",\"r13\":\"0x%llX\",\"r14\":\"0x%llX\",\"r15\":\"0x%llX\","
       "\"rip\":\"0x%llX\",\"eflags\":\"0x%lX\"}",
       (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx,
       (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx,
       (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi,
       (unsigned long long)ctx.Rbp, (unsigned long long)ctx.Rsp,
       (unsigned long long)ctx.R8,  (unsigned long long)ctx.R9,
       (unsigned long long)ctx.R10, (unsigned long long)ctx.R11,
       (unsigned long long)ctx.R12, (unsigned long long)ctx.R13,
       (unsigned long long)ctx.R14, (unsigned long long)ctx.R15,
       (unsigned long long)ctx.Rip, (unsigned long)ctx.EFlags);
#else
    OK("{\"eax\":\"0x%llX\",\"ebx\":\"0x%llX\",\"ecx\":\"0x%llX\",\"edx\":\"0x%llX\","
       "\"esi\":\"0x%llX\",\"edi\":\"0x%llX\",\"ebp\":\"0x%llX\",\"esp\":\"0x%llX\","
       "\"eip\":\"0x%llX\",\"eflags\":\"0x%lX\"}",
       (unsigned long long)ctx.Eax, (unsigned long long)ctx.Ebx,
       (unsigned long long)ctx.Ecx, (unsigned long long)ctx.Edx,
       (unsigned long long)ctx.Esi, (unsigned long long)ctx.Edi,
       (unsigned long long)ctx.Ebp, (unsigned long long)ctx.Esp,
       (unsigned long long)ctx.Eip, (unsigned long)ctx.EFlags);
#endif
}

/**
 * GET_CALLSTACK:thread_id,max_frames
 * Walks the callstack of the current thread (or specified thread).
 * Returns return addresses and resolved module names.
 */
void cmd_GET_CALLSTACK(Command *cmd) {
    char *tidStr = GetParam(cmd->params, 0);
    char *maxStr = GetParam(cmd->params, 1);
    int maxFrames = maxStr ? atoi(maxStr) : 16;
    if (maxFrames > MAX_CALLSTACK_DEPTH) maxFrames = MAX_CALLSTACK_DEPTH;
    if (maxFrames < 1) maxFrames = 1;

    /* Get thread handle */
    HANDLE hThread = NULL;
    if (tidStr && tidStr[0]) {
        DWORD tid = (DWORD)strtoul(tidStr, NULL, 10);
        hThread = OT(
            THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
            THREAD_SUSPEND_RESUME, FALSE, tid);
    } else {
        hThread = GetDebugThread();
    }

    if (!hThread) {
        ERR("cannot open target thread");
        return;
    }

    /* Capture context */
    CONTEXT ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;

    BOOL ctxOk = GTC(hThread, &ctx);

    if (!ctxOk) {
        CloseHandle(hThread);
        ERR("GetThreadContext failed");
        return;
    }

    /* Walk the stack */
    CallstackFrame frames[MAX_CALLSTACK_DEPTH];
    int depth = WalkCallstack(hThread, &ctx, frames, maxFrames);
    CloseHandle(hThread);

    char result[16384];
    int pos = 0;
    pos += sprintf_s(result + pos, sizeof(result) - pos,
        "{\"depth\":%d,\"frames\":[", depth);
    for (int i = 0; i < depth; i++) {
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "%s{\"frame\":%d,\"address\":\"0x%llX\"",
            i > 0 ? "," : "", i,
            (unsigned long long)frames[i].address);
        if (frames[i].moduleName[0]) {
            pos += sprintf_s(result + pos, sizeof(result) - pos,
                ",\"module\":\"%s\"", frames[i].moduleName);
        }
        pos += sprintf_s(result + pos, sizeof(result) - pos, "}");
    }
    pos += sprintf_s(result + pos, sizeof(result) - pos, "]}");
    OK("%s", result);
}

/**
 * SET_BP:address,type,duration
 * type: 0=execute, 1=write, 2=read/write
 *
 * Sets a hardware breakpoint and monitors it for `duration` seconds.
 * The plugin registers a debug event callback (Type 2) to capture every
 * breakpoint hit during the monitoring window. The target process
 * auto-continues after each hit so monitoring is not blocked.
 *
 * Returns every hit with timestamp, thread ID, and full register context.
 */
void cmd_SET_BP(Command *cmd) {
    char *addrStr = GetParam(cmd->params, 0);
    char *typeStr = GetParam(cmd->params, 1);
    char *durStr  = GetParam(cmd->params, 2);

    if (!addrStr) { ERR("missing address"); return; }

    UINT_PTR addr = ParseAddr(addrStr);
    int type = typeStr ? atoi(typeStr) : 1;  /* default: write */
    int duration = durStr ? atoi(durStr) : 10;
    if (duration > 30) duration = 30;
    if (duration < 1) duration = 1;

    /* Validate prerequisite APIs */
    if (!Exported.debug_setBreakpoint) {
        ERR("debug_setBreakpoint not available");
        return;
    }
    if (!Exported.debug_removeBreakpoint) {
        ERR("debug_removeBreakpoint not available");
        return;
    }
    if (bpCallbackId < 0) {
        ERR("debug event callback not registered - is a process being debugged?");
        return;
    }

    /* Allocate a monitor slot */
    BreakpointMonitor *bm = AllocBpMonitor();
    if (!bm) {
        ERR("too many concurrent breakpoint monitors (max %d)", MAX_BP_MONITORS);
        return;
    }

    bm->address = addr;
    bm->triggerType = type;
    bm->bpSize = 4;              /* 4-byte hardware BP */
    bm->startTick = GetTickCount();
    bm->durationSec = (DWORD)duration;

    /* Set the hardware breakpoint via CE's API */
    BOOL bpOk = Exported.debug_setBreakpoint(addr, bm->bpSize, type);
    if (!bpOk) {
        FreeBpMonitor(bm);
        ERR("failed to set breakpoint at 0x%llX (type=%d)",
            (unsigned long long)addr, type);
        return;
    }

    /* Wait for monitoring duration.
     * During this time, the OnDebugEvent callback records every hit
     * and auto-continues the target process. */
    Sleep((DWORD)(duration * 1000));

    /* Remove the breakpoint */
    Exported.debug_removeBreakpoint(addr);

    /* Build the response with all recorded hits, callstacks, and grouping */
    char result[131072];
    int pos = 0;

    /* Build JSON: header */
    pos += sprintf_s(result + pos, sizeof(result) - pos,
        "{\"address\":\"0x%llX\",\"type\":%d,\"duration_sec\":%d,"
        "\"hit_count\":%d,",
        (unsigned long long)addr, type, duration, bm->hitCount);

    /* Groups: for each unique RIP, list which hit indices belong to it */
    UINT_PTR seenRips[200];
    int seenCount = 0;
    int groupMembers[200];   /* hit indices belonging to current group */
    int groupMemberCount = 0;

    pos += sprintf_s(result + pos, sizeof(result) - pos, "\"groups\":[");

    int firstGroup = 1;
    for (int pass = 0; pass < MAX_BP_HITS && pos < (int)sizeof(result) - 2048; pass++) {
        /* Find the next ungrouped RIP and collect all hits sharing it */
        UINT_PTR groupRip = 0;
        groupMemberCount = 0;

        for (int i = 0; i < bm->hitCount; i++) {
#ifdef _WIN64
            UINT_PTR rip = bm->hits[i].context.Rip;
#else
            UINT_PTR rip = bm->hits[i].context.Eip;
#endif
            BOOL alreadySeen = FALSE;
            for (int s = 0; s < seenCount; s++) {
                if (seenRips[s] == rip) {
                    alreadySeen = TRUE;
                    break;
                }
            }
            if (alreadySeen) continue;

            if (groupMemberCount == 0) {
                groupRip = rip;
                groupMembers[groupMemberCount++] = i;
            } else if (rip == groupRip) {
                groupMembers[groupMemberCount++] = i;
            }
        }

        if (groupMemberCount == 0) break;

        /* Mark this RIP as seen */
        if (seenCount < 200) {
            seenRips[seenCount++] = groupRip;
        }

        if (!firstGroup) pos += sprintf_s(result + pos, sizeof(result) - pos, ",");
        firstGroup = 0;

        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "{\"rip\":\"0x%llX\",\"count\":%d,\"indices\":[",
            (unsigned long long)groupRip, groupMemberCount);

        for (int m = 0; m < groupMemberCount; m++) {
            pos += sprintf_s(result + pos, sizeof(result) - pos,
                "%s%d", m > 0 ? "," : "", groupMembers[m]);
        }
        pos += sprintf_s(result + pos, sizeof(result) - pos, "]}");
    }

    pos += sprintf_s(result + pos, sizeof(result) - pos, "],\"hits\":[");

    /* Now emit each hit with full details (registers + callstack) */
    for (int i = 0; i < bm->hitCount && pos < (int)sizeof(result) - 8192; i++) {
        BPHitRecord *hit = &bm->hits[i];
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "%s{\"index\":%d,\"timestamp_ms\":%lu,\"thread_id\":%lu,",
            i > 0 ? "," : "", i, hit->timestamp, hit->threadId);

        /* Callstack */
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "\"callstack\":[");
        for (int f = 0; f < hit->callstackDepth && pos < (int)sizeof(result) - 1024; f++) {
            pos += sprintf_s(result + pos, sizeof(result) - pos,
                "%s{\"frame\":%d,\"address\":\"0x%llX\"",
                f > 0 ? "," : "", f,
                (unsigned long long)hit->callstack[f].address);
            if (hit->callstack[f].moduleName[0]) {
                pos += sprintf_s(result + pos, sizeof(result) - pos,
                    ",\"module\":\"%s\"", hit->callstack[f].moduleName);
            }
            pos += sprintf_s(result + pos, sizeof(result) - pos, "}");
        }
        pos += sprintf_s(result + pos, sizeof(result) - pos, "],");

        /* Registers */
        pos += sprintf_s(result + pos, sizeof(result) - pos, "\"registers\":{");

#ifdef _WIN64
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "\"rax\":\"0x%llX\",\"rbx\":\"0x%llX\",\"rcx\":\"0x%llX\","
            "\"rdx\":\"0x%llX\",\"rsi\":\"0x%llX\",\"rdi\":\"0x%llX\","
            "\"rbp\":\"0x%llX\",\"rsp\":\"0x%llX\",\"r8\":\"0x%llX\","
            "\"r9\":\"0x%llX\",\"r10\":\"0x%llX\",\"r11\":\"0x%llX\","
            "\"r12\":\"0x%llX\",\"r13\":\"0x%llX\",\"r14\":\"0x%llX\","
            "\"r15\":\"0x%llX\",\"rip\":\"0x%llX\",\"eflags\":\"0x%lX\"",
            (unsigned long long)hit->context.Rax,
            (unsigned long long)hit->context.Rbx,
            (unsigned long long)hit->context.Rcx,
            (unsigned long long)hit->context.Rdx,
            (unsigned long long)hit->context.Rsi,
            (unsigned long long)hit->context.Rdi,
            (unsigned long long)hit->context.Rbp,
            (unsigned long long)hit->context.Rsp,
            (unsigned long long)hit->context.R8,
            (unsigned long long)hit->context.R9,
            (unsigned long long)hit->context.R10,
            (unsigned long long)hit->context.R11,
            (unsigned long long)hit->context.R12,
            (unsigned long long)hit->context.R13,
            (unsigned long long)hit->context.R14,
            (unsigned long long)hit->context.R15,
            (unsigned long long)hit->context.Rip,
            (unsigned long)hit->context.EFlags);
#else
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "\"eax\":\"0x%llX\",\"ebx\":\"0x%llX\",\"ecx\":\"0x%llX\","
            "\"edx\":\"0x%llX\",\"esi\":\"0x%llX\",\"edi\":\"0x%llX\","
            "\"ebp\":\"0x%llX\",\"esp\":\"0x%llX\",\"eip\":\"0x%llX\","
            "\"eflags\":\"0x%lX\"",
            (unsigned long long)hit->context.Eax,
            (unsigned long long)hit->context.Ebx,
            (unsigned long long)hit->context.Ecx,
            (unsigned long long)hit->context.Edx,
            (unsigned long long)hit->context.Esi,
            (unsigned long long)hit->context.Edi,
            (unsigned long long)hit->context.Ebp,
            (unsigned long long)hit->context.Esp,
            (unsigned long long)hit->context.Eip,
            (unsigned long)hit->context.EFlags);
#endif
        pos += sprintf_s(result + pos, sizeof(result) - pos, "}}");
    }

    pos += sprintf_s(result + pos, sizeof(result) - pos, "]}");

    /* Free the monitor slot */
    FreeBpMonitor(bm);

    OK("%s", result);
}

/**
 * REGISTER_TRACE:start_addr,end_addr,duration
 *
 * Sets execute breakpoints at start_addr and end_addr, then monitors for
 * `duration` seconds. Each time the start BP fires, registers are captured
 * at function entry. The corresponding end BP capture (at function exit/
 * return) is paired, and a register diff is computed.
 *
 * Returns paired snapshots: entry_registers, exit_registers, diff.
 */
void cmd_REGISTER_TRACE(Command *cmd) {
    char *startStr = GetParam(cmd->params, 0);
    char *endStr   = GetParam(cmd->params, 1);
    char *durStr   = GetParam(cmd->params, 2);

    if (!startStr || !endStr) {
        ERR("missing start_addr or end_addr");
        return;
    }

    UINT_PTR startAddr = ParseAddr(startStr);
    UINT_PTR endAddr   = ParseAddr(endStr);
    int duration = durStr ? atoi(durStr) : 10;
    if (duration > 30) duration = 30;
    if (duration < 1) duration = 1;

    if (!Exported.debug_setBreakpoint ||
        !Exported.debug_removeBreakpoint) {
        ERR("breakpoint API not available");
        return;
    }

    /* Set execute breakpoints at both addresses (size=1 for execute BP) */
    BOOL startOk = Exported.debug_setBreakpoint(startAddr, 1, 0);
    BOOL endOk   = Exported.debug_setBreakpoint(endAddr, 1, 0);

    if (!startOk || !endOk) {
        if (startOk) Exported.debug_removeBreakpoint(startAddr);
        if (endOk)   Exported.debug_removeBreakpoint(endAddr);
        ERR("failed to set trace breakpoints");
        return;
    }

    /* Wait for monitoring duration.
     * The OnDebugEvent callback captures every hit with registers + callstack.
     * After the wait, we scan the BP monitor records looking for paired
     * start/end hits. */
    Sleep((DWORD)(duration * 1000));

    Exported.debug_removeBreakpoint(startAddr);
    Exported.debug_removeBreakpoint(endAddr);

    /* Collect all hits from the two breakpoint monitors.
     * Heap-allocate: BPHitRecord can be ~1300 bytes each, 400 of them
     * would overflow a 1MB thread stack at deep call depths. */
    BPHitRecord *startHits = NULL, *endHits = NULL;
    int startCount = 0, endCount = 0;

    startHits = (BPHitRecord *)calloc(150, sizeof(BPHitRecord));
    endHits   = (BPHitRecord *)calloc(150, sizeof(BPHitRecord));
    if (!startHits || !endHits) {
        free(startHits);
        free(endHits);
        Exported.debug_removeBreakpoint(startAddr);
        Exported.debug_removeBreakpoint(endAddr);
        ERR("out of memory");
        return;
    }

    EnterCriticalSection(&bpMonitorLock);
    for (int i = 0; i < MAX_BP_MONITORS; i++) {
        BreakpointMonitor *bm = &bpMonitors[i];
        if (!bm->active) continue;
        if (bm->address == startAddr && bm->triggerType == 0) {
            for (int j = 0; j < bm->hitCount && startCount < 150; j++)
                startHits[startCount++] = bm->hits[j];
        }
        if (bm->address == endAddr && bm->triggerType == 0) {
            for (int j = 0; j < bm->hitCount && endCount < 150; j++)
                endHits[endCount++] = bm->hits[j];
        }
    }
    /* Clean up these monitors */
    for (int i = 0; i < MAX_BP_MONITORS; i++) {
        if (!bpMonitors[i].active) continue;
        if (bpMonitors[i].address == startAddr || bpMonitors[i].address == endAddr) {
            ZeroMemory(&bpMonitors[i], sizeof(BreakpointMonitor));
        }
    }
    LeaveCriticalSection(&bpMonitorLock);

    /* Pair start hits with subsequent end hits by timestamp order.
     * Each start hit pairs with the next end hit that occurs after it. */
    char result[131072];
    int pos = 0;
    pos += sprintf_s(result + pos, sizeof(result) - pos,
        "{\"start_address\":\"0x%llX\",\"end_address\":\"0x%llX\","
        "\"duration_sec\":%d,\"entry_count\":%d,\"exit_count\":%d,"
        "\"pairs\":[",
        (unsigned long long)startAddr, (unsigned long long)endAddr,
        duration, startCount, endCount);

    int endIdx = 0;
    int pairCount = 0;
    for (int s = 0; s < startCount && pairCount < MAX_TRACE_PAIRS; s++) {
        /* Find next end hit after this start hit */
        while (endIdx < endCount &&
               endHits[endIdx].timestamp < startHits[s].timestamp) {
            endIdx++;
        }
        if (endIdx >= endCount) break;

        BPHitRecord *entry = &startHits[s];
        BPHitRecord *exit  = &endHits[endIdx];
        endIdx++;

        if (pairCount > 0)
            pos += sprintf_s(result + pos, sizeof(result) - pos, ",");
        pairCount++;

        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "{\"entry_timestamp_ms\":%lu,\"exit_timestamp_ms\":%lu,"
            "\"thread_id\":%lu,",
            entry->timestamp, exit->timestamp, entry->threadId);

        /* Entry registers */
        pos += sprintf_s(result + pos, sizeof(result) - pos, "\"entry\":{");
#ifdef _WIN64
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "\"rax\":\"0x%llX\",\"rbx\":\"0x%llX\",\"rcx\":\"0x%llX\","
            "\"rdx\":\"0x%llX\",\"rsi\":\"0x%llX\",\"rdi\":\"0x%llX\","
            "\"rbp\":\"0x%llX\",\"rsp\":\"0x%llX\",\"r8\":\"0x%llX\","
            "\"r9\":\"0x%llX\",\"r10\":\"0x%llX\",\"r11\":\"0x%llX\","
            "\"r12\":\"0x%llX\",\"r13\":\"0x%llX\",\"r14\":\"0x%llX\","
            "\"r15\":\"0x%llX\",\"rip\":\"0x%llX\",\"eflags\":\"0x%lX\"",
            (unsigned long long)entry->context.Rax,
            (unsigned long long)entry->context.Rbx,
            (unsigned long long)entry->context.Rcx,
            (unsigned long long)entry->context.Rdx,
            (unsigned long long)entry->context.Rsi,
            (unsigned long long)entry->context.Rdi,
            (unsigned long long)entry->context.Rbp,
            (unsigned long long)entry->context.Rsp,
            (unsigned long long)entry->context.R8,
            (unsigned long long)entry->context.R9,
            (unsigned long long)entry->context.R10,
            (unsigned long long)entry->context.R11,
            (unsigned long long)entry->context.R12,
            (unsigned long long)entry->context.R13,
            (unsigned long long)entry->context.R14,
            (unsigned long long)entry->context.R15,
            (unsigned long long)entry->context.Rip,
            (unsigned long)entry->context.EFlags);
#else
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "\"eax\":\"0x%llX\",\"ebx\":\"0x%llX\",\"ecx\":\"0x%llX\","
            "\"edx\":\"0x%llX\",\"esi\":\"0x%llX\",\"edi\":\"0x%llX\","
            "\"ebp\":\"0x%llX\",\"esp\":\"0x%llX\",\"eip\":\"0x%llX\","
            "\"eflags\":\"0x%lX\"",
            (unsigned long long)entry->context.Eax,
            (unsigned long long)entry->context.Ebx,
            (unsigned long long)entry->context.Ecx,
            (unsigned long long)entry->context.Edx,
            (unsigned long long)entry->context.Esi,
            (unsigned long long)entry->context.Edi,
            (unsigned long long)entry->context.Ebp,
            (unsigned long long)entry->context.Esp,
            (unsigned long long)entry->context.Eip,
            (unsigned long)entry->context.EFlags);
#endif
        pos += sprintf_s(result + pos, sizeof(result) - pos, "},");

        /* Exit registers */
        pos += sprintf_s(result + pos, sizeof(result) - pos, "\"exit\":{");
#ifdef _WIN64
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "\"rax\":\"0x%llX\",\"rbx\":\"0x%llX\",\"rcx\":\"0x%llX\","
            "\"rdx\":\"0x%llX\",\"rsi\":\"0x%llX\",\"rdi\":\"0x%llX\","
            "\"rbp\":\"0x%llX\",\"rsp\":\"0x%llX\",\"r8\":\"0x%llX\","
            "\"r9\":\"0x%llX\",\"r10\":\"0x%llX\",\"r11\":\"0x%llX\","
            "\"r12\":\"0x%llX\",\"r13\":\"0x%llX\",\"r14\":\"0x%llX\","
            "\"r15\":\"0x%llX\",\"rip\":\"0x%llX\",\"eflags\":\"0x%lX\"",
            (unsigned long long)exit->context.Rax,
            (unsigned long long)exit->context.Rbx,
            (unsigned long long)exit->context.Rcx,
            (unsigned long long)exit->context.Rdx,
            (unsigned long long)exit->context.Rsi,
            (unsigned long long)exit->context.Rdi,
            (unsigned long long)exit->context.Rbp,
            (unsigned long long)exit->context.Rsp,
            (unsigned long long)exit->context.R8,
            (unsigned long long)exit->context.R9,
            (unsigned long long)exit->context.R10,
            (unsigned long long)exit->context.R11,
            (unsigned long long)exit->context.R12,
            (unsigned long long)exit->context.R13,
            (unsigned long long)exit->context.R14,
            (unsigned long long)exit->context.R15,
            (unsigned long long)exit->context.Rip,
            (unsigned long)exit->context.EFlags);
#else
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "\"eax\":\"0x%llX\",\"ebx\":\"0x%llX\",\"ecx\":\"0x%llX\","
            "\"edx\":\"0x%llX\",\"esi\":\"0x%llX\",\"edi\":\"0x%llX\","
            "\"ebp\":\"0x%llX\",\"esp\":\"0x%llX\",\"eip\":\"0x%llX\","
            "\"eflags\":\"0x%lX\"",
            (unsigned long long)exit->context.Eax,
            (unsigned long long)exit->context.Ebx,
            (unsigned long long)exit->context.Ecx,
            (unsigned long long)exit->context.Edx,
            (unsigned long long)exit->context.Esi,
            (unsigned long long)exit->context.Edi,
            (unsigned long long)exit->context.Ebp,
            (unsigned long long)exit->context.Esp,
            (unsigned long long)exit->context.Eip,
            (unsigned long)exit->context.EFlags);
#endif
        pos += sprintf_s(result + pos, sizeof(result) - pos, "},");

        /* Diff: compute register changes */
        pos += sprintf_s(result + pos, sizeof(result) - pos, "\"diff\":{");
#ifdef _WIN64
        /* Only emit registers that changed (keep output compact) */
        const char *regNames64[] = {"rax","rbx","rcx","rdx","rsi","rdi",
                                     "rbp","rsp","r8","r9","r10","r11",
                                     "r12","r13","r14","r15","rip","eflags"};
        UINT_PTR entryVals[] = {
            entry->context.Rax, entry->context.Rbx, entry->context.Rcx,
            entry->context.Rdx, entry->context.Rsi, entry->context.Rdi,
            entry->context.Rbp, entry->context.Rsp, entry->context.R8,
            entry->context.R9,  entry->context.R10, entry->context.R11,
            entry->context.R12, entry->context.R13, entry->context.R14,
            entry->context.R15, entry->context.Rip, (UINT_PTR)entry->context.EFlags
        };
        UINT_PTR exitVals[] = {
            exit->context.Rax, exit->context.Rbx, exit->context.Rcx,
            exit->context.Rdx, exit->context.Rsi, exit->context.Rdi,
            exit->context.Rbp, exit->context.Rsp, exit->context.R8,
            exit->context.R9,  exit->context.R10, exit->context.R11,
            exit->context.R12, exit->context.R13, exit->context.R14,
            exit->context.R15, exit->context.Rip, (UINT_PTR)exit->context.EFlags
        };
        int nRegs = 18;
#else
        const char *regNames32[] = {"eax","ebx","ecx","edx","esi","edi",
                                     "ebp","esp","eip","eflags"};
        UINT_PTR entryVals[] = {
            entry->context.Eax, entry->context.Ebx, entry->context.Ecx,
            entry->context.Edx, entry->context.Esi, entry->context.Edi,
            entry->context.Ebp, entry->context.Esp, entry->context.Eip,
            (UINT_PTR)entry->context.EFlags
        };
        UINT_PTR exitVals[] = {
            exit->context.Eax, exit->context.Ebx, exit->context.Ecx,
            exit->context.Edx, exit->context.Esi, exit->context.Edi,
            exit->context.Ebp, exit->context.Esp, exit->context.Eip,
            (UINT_PTR)exit->context.EFlags
        };
        const char **regNames = regNames32;
        int nRegs = 10;
#endif

        int firstDiff = 1;
        for (int r = 0; r < nRegs && pos < (int)sizeof(result) - 500; r++) {
#ifdef _WIN64
            const char **regNames = regNames64;
#endif
            if (entryVals[r] != exitVals[r]) {
                INT_PTR delta = (INT_PTR)(exitVals[r] - entryVals[r]);
                pos += sprintf_s(result + pos, sizeof(result) - pos,
                    "%s\"%s\":{\"from\":\"0x%llX\",\"to\":\"0x%llX\""
                    ",\"delta\":\"%+lld\"}",
                    firstDiff ? "" : ",",
                    regNames[r],
                    (unsigned long long)entryVals[r],
                    (unsigned long long)exitVals[r],
                    (long long)delta);
                firstDiff = 0;
            }
        }
        if (firstDiff) {
            pos += sprintf_s(result + pos, sizeof(result) - pos,
                "\"_note\":\"no registers changed\"");
        }

        pos += sprintf_s(result + pos, sizeof(result) - pos, "}}");
    }

    pos += sprintf_s(result + pos, sizeof(result) - pos,
        "],\"paired_count\":%d}", pairCount);
    free(startHits);
    free(endHits);
    OK("%s", result);
}
