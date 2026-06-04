/**
 * plugin-scan.c — 内存扫描命令
 *
 * MEMORY_SCAN / MEMORY_SCAN_NEXT
 */

#include "plugin.h"

/* Scan type constants */
#define SCAN_TYPE_BYTE   0
#define SCAN_TYPE_WORD   1
#define SCAN_TYPE_DWORD  2
#define SCAN_TYPE_QWORD  3
#define SCAN_TYPE_FLOAT  4
#define SCAN_TYPE_DOUBLE 5
#define SCAN_TYPE_STRING 6

/* Filter types */
#define SCAN_FILTER_EXACT     0
#define SCAN_FILTER_CHANGED   1
#define SCAN_FILTER_UNCHANGED 2

/* ========== Scan cache (shared via plugin.h extern declarations) ========== */
UINT_PTR cachedScanAddrs[MAX_CACHED_ADDRS];
int    cachedScanCount = 0;
BYTE   cachedScanValues[MAX_CACHED_ADDRS * 8];
int    cachedScanValueSize = 0;

/* ========== Static helpers ========== */

static UINT_PTR ReadValueU(const BYTE *buf, int size) {
    UINT_PTR v = 0;
    memcpy(&v, buf, size);
    return v;
}

static float ReadFloat(const BYTE *buf) {
    float f;
    memcpy(&f, buf, sizeof(f));
    return f;
}

static double ReadDouble(const BYTE *buf) {
    double d;
    memcpy(&d, buf, sizeof(d));
    return d;
}

/**
 * MEMORY_SCAN:type,value,start_addr,end_addr,max_results
 *
 * Scans the target process memory for a specific value.
 * Returns matching addresses. Results are cached for follow-up
 * changed/unchanged filtering via MEMORY_SCAN_NEXT.
 */
void cmd_MEMORY_SCAN(Command *cmd) {
    char *typeStr  = GetParam(cmd->params, 0);
    char *valueStr = GetParam(cmd->params, 1);
    char *startStr = GetParam(cmd->params, 2);
    char *endStr   = GetParam(cmd->params, 3);
    char *maxStr   = GetParam(cmd->params, 4);

    if (!typeStr || !valueStr) {
        ERR("missing type or value parameter");
        return;
    }

    int scanType = atoi(typeStr);
    int maxResults = maxStr ? atoi(maxStr) : 500;
    if (maxResults > MAX_CACHED_ADDRS) maxResults = MAX_CACHED_ADDRS;
    if (maxResults < 1) maxResults = 1;

    /* Determine value size and parse the value */
    int valueSize = 0;
    BYTE targetValue[256]; /* large enough for strings */
    UINT_PTR targetU = 0;
    float targetF = 0.0f;
    double targetD = 0.0;

    switch (scanType) {
        case SCAN_TYPE_BYTE:
            valueSize = 1;
            targetU = (UINT_PTR)strtoul(valueStr, NULL, 0);
            memcpy(targetValue, &targetU, valueSize);
            break;
        case SCAN_TYPE_WORD:
            valueSize = 2;
            targetU = (UINT_PTR)strtoul(valueStr, NULL, 0);
            memcpy(targetValue, &targetU, valueSize);
            break;
        case SCAN_TYPE_DWORD:
            valueSize = 4;
            targetU = (UINT_PTR)strtoul(valueStr, NULL, 0);
            memcpy(targetValue, &targetU, valueSize);
            break;
        case SCAN_TYPE_QWORD:
            valueSize = 8;
            targetU = (UINT_PTR)strtoull(valueStr, NULL, 0);
            memcpy(targetValue, &targetU, valueSize);
            break;
        case SCAN_TYPE_FLOAT:
            valueSize = 4;
            targetF = (float)atof(valueStr);
            memcpy(targetValue, &targetF, valueSize);
            break;
        case SCAN_TYPE_DOUBLE:
            valueSize = 8;
            targetD = atof(valueStr);
            memcpy(targetValue, &targetD, valueSize);
            break;
        case SCAN_TYPE_STRING:
            valueSize = (int)strlen(valueStr);
            if (valueSize > 255) valueSize = 255;
            memcpy(targetValue, valueStr, valueSize);
            targetValue[valueSize] = '\0';
            break;
        default:
            ERR("unknown scan type: %d", scanType);
            return;
    }

    if (valueSize <= 0) {
        ERR("invalid value size");
        return;
    }

    /* Determine scan range */
    UINT_PTR startAddr = 0, endAddr = 0;
    DWORD pid = 0;
    if (Exported.OpenedProcessID)
        pid = *Exported.OpenedProcessID;

    if (startStr && startStr[0]) {
        startAddr = ParseAddr(startStr);
    } else {
        /* Default: scan from first module base */
        HANDLE snap = (HANDLE)CS(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me;
            me.dwSize = sizeof(MODULEENTRY32);
            if (M32F(snap, &me)) {
                startAddr = (UINT_PTR)me.modBaseAddr;
                endAddr = startAddr + me.modBaseSize;
            }
            CloseHandle(snap);
        }
    }

    if (endStr && endStr[0]) {
        endAddr = ParseAddr(endStr);
    }

    if (startAddr == 0 || endAddr <= startAddr) {
        ERR("invalid scan range: 0x%llX - 0x%llX",
            (unsigned long long)startAddr, (unsigned long long)endAddr);
        return;
    }

    /* Limit scan range to avoid timeouts */
    UINT_PTR rangeSize = endAddr - startAddr;
    if (rangeSize > 512 * 1024 * 1024) { /* 512 MB max */
        ERR("scan range too large (%llu bytes, max 512MB)", (unsigned long long)rangeSize);
        return;
    }

    /* Aligned chunked scan */
    BYTE chunk[65536];
    UINT_PTR found[MAX_CACHED_ADDRS];
    int foundCount = 0;

    for (UINT_PTR cur = startAddr; cur < endAddr && foundCount < maxResults;) {
        SIZE_T chunkSize = min(sizeof(chunk), endAddr - cur);
        SIZE_T bytesRead = 0;

        if (!RPM(*Exported.OpenedProcessHandle,
                 (LPCVOID)cur, chunk, chunkSize, &bytesRead)) {
            /* Skip unreadable pages (advance by 4KB page) */
            cur += 4096;
            continue;
        }

        SIZE_T limit = bytesRead;
        if (valueSize > 0) {
            if ((SIZE_T)valueSize > limit) { cur += bytesRead; continue; }
            limit = bytesRead - (valueSize - 1);
        }

        for (SIZE_T i = 0; i < limit && foundCount < maxResults; i++) {
            BOOL match = FALSE;

            switch (scanType) {
                case SCAN_TYPE_BYTE:
                case SCAN_TYPE_WORD:
                case SCAN_TYPE_DWORD:
                case SCAN_TYPE_QWORD:
                    match = (memcmp(chunk + i, targetValue, valueSize) == 0);
                    break;
                case SCAN_TYPE_FLOAT:
                case SCAN_TYPE_DOUBLE:
                    match = (memcmp(chunk + i, targetValue, valueSize) == 0);
                    break;
                case SCAN_TYPE_STRING:
                    match = (memcmp(chunk + i, targetValue, valueSize) == 0);
                    break;
            }

            if (match) {
                found[foundCount] = cur + i;
                /* Cache value for later changed/unchanged filtering */
                if (foundCount < MAX_CACHED_ADDRS) {
                    cachedScanAddrs[foundCount] = cur + i;
                    if (valueSize <= 8)
                        memcpy(&cachedScanValues[foundCount * 8], chunk + i, valueSize);
                    else
                        memcpy(&cachedScanValues[foundCount * 8], chunk + i, 8);
                }
                foundCount++;
            }
        }

        if (valueSize == 0) break;
        cur += bytesRead - valueSize + 1;
    }

    /* Store cache state */
    cachedScanCount = foundCount;
    cachedScanValueSize = min(valueSize, 8);

    char result[65536];
    int pos = 0;
    pos += sprintf_s(result + pos, sizeof(result) - pos,
        "{\"scan_type\":%d,\"value_size\":%d,\"range_start\":\"0x%llX\","
        "\"range_end\":\"0x%llX\",\"found\":%d,\"max_results\":%d,"
        "\"addresses\":[",
        scanType, valueSize,
        (unsigned long long)startAddr, (unsigned long long)endAddr,
        foundCount, maxResults);
    for (int i = 0; i < foundCount && pos < (int)sizeof(result) - 128; i++) {
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "%s\"0x%llX\"", i > 0 ? "," : "",
            (unsigned long long)found[i]);
    }
    pos += sprintf_s(result + pos, sizeof(result) - pos, "]}");
    OK("%s", result);
}

/**
 * MEMORY_SCAN_NEXT:filter_type,max_results
 *
 * Filters the cached previous scan results:
 *   filter_type=1: only addresses whose value changed
 *   filter_type=2: only addresses whose value stayed the same
 *
 * Returns the filtered address list and updates the cache for further chaining.
 */
void cmd_MEMORY_SCAN_NEXT(Command *cmd) {
    char *filterStr = GetParam(cmd->params, 0);
    char *maxStr    = GetParam(cmd->params, 1);

    if (!filterStr) { ERR("missing filter type"); return; }

    int filterType = atoi(filterStr);
    int maxResults = maxStr ? atoi(maxStr) : 500;
    if (maxResults > MAX_CACHED_ADDRS) maxResults = MAX_CACHED_ADDRS;
    if (maxResults < 1) maxResults = 1;

    if (cachedScanCount == 0) {
        ERR("no cached scan results - run MEMORY_SCAN first");
        return;
    }

    int valueSize = cachedScanValueSize;
    BYTE curValue[8];
    UINT_PTR filtered[MAX_CACHED_ADDRS];
    int filteredCount = 0;

    for (int i = 0; i < cachedScanCount && filteredCount < maxResults; i++) {
        SIZE_T bytesRead = 0;
        BOOL readOk = RPM(*Exported.OpenedProcessHandle,
                          (LPCVOID)cachedScanAddrs[i],
                          curValue, valueSize, &bytesRead);

        if (!readOk || bytesRead < (SIZE_T)valueSize) continue;

        BOOL changed = (memcmp(curValue, &cachedScanValues[i * 8], valueSize) != 0);

        if ((filterType == SCAN_FILTER_CHANGED && changed) ||
            (filterType == SCAN_FILTER_UNCHANGED && !changed)) {
            filtered[filteredCount] = cachedScanAddrs[i];
            if (valueSize <= 8)
                memcpy(&cachedScanValues[filteredCount * 8], curValue, valueSize);
            filteredCount++;
        }
    }

    /* Update cache with filtered results */
    cachedScanCount = filteredCount;

    char result[65536];
    int pos = 0;
    pos += sprintf_s(result + pos, sizeof(result) - pos,
        "{\"filter\":\"%s\",\"remaining\":%d,\"addresses\":[",
        filterType == SCAN_FILTER_CHANGED ? "changed" : "unchanged",
        filteredCount);
    for (int i = 0; i < filteredCount && pos < (int)sizeof(result) - 128; i++) {
        pos += sprintf_s(result + pos, sizeof(result) - pos,
            "%s\"0x%llX\"", i > 0 ? "," : "",
            (unsigned long long)filtered[i]);
    }
    pos += sprintf_s(result + pos, sizeof(result) - pos, "]}");
    OK("%s", result);
}
