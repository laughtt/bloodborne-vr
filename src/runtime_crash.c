/* Crash and hang capture: minidump + JSON summary. See runtime_crash.h. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime_crash.h"

#ifndef BB_BUILD_ID
#define BB_BUILD_ID "unknown"
#endif

/* Bumped by the video-out flip (gpu/shim/bbgpu.cpp, gpu/shadps4 videoout driver). */
extern volatile unsigned long long bb_heartbeat_ticks;

typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
#pragma pack(push, 4)
typedef struct { DWORD ThreadId; EXCEPTION_POINTERS *ExceptionPointers; BOOL ClientPointers; } DumpExceptionInfo;
#pragma pack(pop)

static char crash_root[MAX_PATH];
static unsigned char *const *guest_image_ref;
static int capture_enabled;
static volatile LONG bundle_written;

static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* Module name and offset of an address; guest code is reported as module "guest". */
static void locate(uintptr_t address, char *module, size_t size, uintptr_t *offset) {
    HMODULE m = NULL;
    snprintf(module, size, "?");
    *offset = address;
    if (guest_image_ref && *guest_image_ref && address - (uintptr_t)*guest_image_ref < 0x20000000) {
        snprintf(module, size, "guest");
        *offset = address - (uintptr_t)*guest_image_ref;
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &m) && m) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(m, path, sizeof(path))) {
            const char *base = strrchr(path, '\\');
            snprintf(module, size, "%s", base ? base + 1 : path);
        }
        *offset = address - (uintptr_t)m;
    }
}

static void write_dump(const char *path, EXCEPTION_POINTERS *info) {
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (!dbghelp) return;
    MiniDumpWriteDump_t write = (MiniDumpWriteDump_t)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (write && file != INVALID_HANDLE_VALUE) {
        DumpExceptionInfo exception = { GetCurrentThreadId(), info, FALSE };
        /* MiniDumpNormal (0) | MiniDumpWithThreadInfo (0x1000): no full memory. */
        write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1000, info ? &exception : NULL, NULL, NULL);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

void bb_crash_write(EXCEPTION_POINTERS *info, const char *kind) {
    if (!capture_enabled || !crash_root[0]) return;
    if (InterlockedExchange(&bundle_written, 1)) return;
    char dir[MAX_PATH + 40], path[MAX_PATH + 64];
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(dir, sizeof(dir), "%s\\%04d%02d%02d-%02d%02d%02d-%s", crash_root, t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, kind);
    CreateDirectoryA(crash_root, NULL);
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    snprintf(path, sizeof(path), "%s\\crash.dmp", dir);
    write_dump(path, info);
    snprintf(path, sizeof(path), "%s\\crash.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\n  \"build\": ");
    json_string(f, BB_BUILD_ID);
    fprintf(f, ",\n  \"kind\": ");
    json_string(f, kind);
    fprintf(f, ",\n  \"time\": \"%04d-%02d-%02dT%02d:%02d:%02d\",\n  \"pid\": %lu,\n  \"heartbeat\": %llu",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), bb_heartbeat_ticks);
    if (info) {
        const EXCEPTION_RECORD *r = info->ExceptionRecord;
        const CONTEXT *c = info->ContextRecord;
        char module[MAX_PATH];
        uintptr_t offset;
        locate((uintptr_t)c->Rip, module, sizeof(module), &offset);
        fprintf(f, ",\n  \"exception_code\": \"0x%08lx\",\n  \"thread\": %lu,\n  \"fault_address\": \"0x%016" PRIxPTR "\",\n"
                   "  \"fault_module\": ", r->ExceptionCode, GetCurrentThreadId(), (uintptr_t)c->Rip);
        json_string(f, module);
        fprintf(f, ",\n  \"fault_offset\": \"0x%" PRIxPTR "\"", offset);
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
            fprintf(f, ",\n  \"access\": \"%s\",\n  \"access_address\": \"0x%016llx\"",
                    r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "execute" : "read",
                    (unsigned long long)r->ExceptionInformation[1]);
        fprintf(f, ",\n  \"registers\": {");
        fprintf(f, "\"rax\":\"%016llx\",\"rbx\":\"%016llx\",\"rcx\":\"%016llx\",\"rdx\":\"%016llx\","
                   "\"rsi\":\"%016llx\",\"rdi\":\"%016llx\",\"rbp\":\"%016llx\",\"rsp\":\"%016llx\","
                   "\"r8\":\"%016llx\",\"r9\":\"%016llx\",\"r10\":\"%016llx\",\"r11\":\"%016llx\","
                   "\"r12\":\"%016llx\",\"r13\":\"%016llx\",\"r14\":\"%016llx\",\"r15\":\"%016llx\",\"rip\":\"%016llx\"}",
                c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11,
                c->R12, c->R13, c->R14, c->R15, c->Rip);
        /* rbp chain (guest and host code keep frame pointers). */
        fprintf(f, ",\n  \"backtrace\": [");
        uintptr_t rbp = (uintptr_t)c->Rbp;
        int first = 1;
        for (int depth = 0; depth < 24 && rbp; ++depth) {
            uintptr_t frame[2];
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void *)rbp, frame, sizeof(frame), &got) ||
                got != sizeof(frame) || frame[0] <= rbp) break;
            locate(frame[1], module, sizeof(module), &offset);
            fprintf(f, "%s\n    \"%s+0x%" PRIxPTR "\"", first ? "" : ",", module, offset);
            first = 0;
            rbp = frame[0];
        }
        fprintf(f, "\n  ]");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    fprintf(stderr, "Crash report written to %s\n", dir);
}

static DWORD WINAPI hang_thread(LPVOID seconds) {
    const ULONGLONG limit = (ULONGLONG)(uintptr_t)seconds * 1000u;
    unsigned long long last = 0;
    ULONGLONG changed = GetTickCount64();
    for (;;) {
        Sleep(1000);
        unsigned long long now = bb_heartbeat_ticks;
        if (now != last) { last = now; changed = GetTickCount64(); continue; }
        /* Only after the game has presented once: loading before the first frame does not count. */
        if (now && GetTickCount64() - changed >= limit) {
            fprintf(stderr, "Hang: no frame for %lu s\n", (unsigned long)(uintptr_t)seconds);
            bb_crash_write(NULL, "hang");
            return 0;
        }
    }
}

static DWORD WINAPI test_thread(LPVOID mode) {
    Sleep(8000);
    if (mode) { /* hang: a few frames, then nothing */
        for (int i = 0; i < 3; ++i) { bb_heartbeat_ticks = bb_heartbeat_ticks + 1; Sleep(200); }
        Sleep(INFINITE);
    }
    fprintf(stderr, "BB_TEST_CRASH: deliberate fault\n");
    *(volatile int *)0 = 1;
    return 0;
}

/* Exit trace (docs/vr/findings/43-pm4-race.md): every process exit but TerminateProcess goes
 * through ntdll!RtlExitUserProcess (exit, _exit, ExitProcess, from any module). Some launches
 * ended with code 1 and no line in the log, a few seconds after the VR session started: a
 * library exiting on its own. The trap names the caller (module+offset backtrace, guest rbp
 * chain), writes a report bundle, then exits as asked. Code 0 passes through silently. */
typedef VOID (NTAPI *RtlExitUserProcess_t)(LONG);
static RtlExitUserProcess_t exit_original;
static unsigned char exit_saved[12];
static volatile LONG exit_traced;

static void exit_restore(void) {
    DWORD old;
    if (VirtualProtect((void *)exit_original, sizeof(exit_saved), PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void *)exit_original, exit_saved, sizeof(exit_saved));
        VirtualProtect((void *)exit_original, sizeof(exit_saved), old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void *)exit_original, sizeof(exit_saved));
    }
}

static VOID NTAPI exit_trap(LONG status) {
    const unsigned code = (unsigned)status;
    if (code != 0 && !InterlockedExchange(&exit_traced, 1)) {
        fflush(NULL);
        fprintf(stderr, "Exit: the process is exiting with code %u (0x%x) on thread %lu, called from:\n",
                code, code, GetCurrentThreadId());
        void *frames[32];
        const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, NULL);
        char module[MAX_PATH];
        uintptr_t offset;
        for (USHORT i = 0; i < n; ++i) {
            locate((uintptr_t)frames[i], module, sizeof(module), &offset);
            fprintf(stderr, "  #%u %s+0x%" PRIxPTR "\n", i, module, offset);
        }
        CONTEXT context;
        RtlCaptureContext(&context);
        EXCEPTION_RECORD record = {0};
        record.ExceptionCode = 0xE0000001u; /* not an exception: the exit, for the report */
        record.ExceptionAddress = (void *)(uintptr_t)context.Rip;
        EXCEPTION_POINTERS info = {&record, &context};
        char kind[32];
        snprintf(kind, sizeof(kind), "exit%u", code);
        bb_crash_write(&info, kind);
        fflush(NULL);
    }
    exit_restore();
    exit_original(status);
}

static void exit_trace_init(void) {
    const char *env = getenv("BB_EXIT_TRACE");
    if (env && !strcmp(env, "0")) return;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    exit_original = ntdll ? (RtlExitUserProcess_t)(void *)GetProcAddress(ntdll, "RtlExitUserProcess") : NULL;
    if (!exit_original) return;
    unsigned char jump[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0}; /* mov rax, imm64; jmp rax */
    const uint64_t target = (uint64_t)(uintptr_t)exit_trap;
    memcpy(jump + 2, &target, sizeof(target));
    DWORD old;
    if (!VirtualProtect((void *)exit_original, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(exit_saved, (void *)exit_original, sizeof(exit_saved));
    memcpy((void *)exit_original, jump, sizeof(jump));
    VirtualProtect((void *)exit_original, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)exit_original, sizeof(jump));
}

void bb_crash_init(const char *user_dir, unsigned char *const *guest_image) {
    const char *enabled = getenv("BB_CRASH_CAPTURE");
    guest_image_ref = guest_image;
    capture_enabled = !(enabled && !strcmp(enabled, "0"));
    snprintf(crash_root, sizeof(crash_root), "%s\\crashes", user_dir && *user_dir ? user_dir : "user");
    for (char *p = crash_root; *p; ++p) if (*p == '/') *p = '\\';
    printf("BB_BUILD_ID=%s\n", BB_BUILD_ID);
    if (!capture_enabled) return;
    exit_trace_init();
    const char *hang = getenv("BB_HANG_SECONDS");
    unsigned long seconds = hang ? strtoul(hang, NULL, 10) : 0;
    if (seconds) { HANDLE h = CreateThread(NULL, 0, hang_thread, (LPVOID)(uintptr_t)seconds, 0, NULL); if (h) CloseHandle(h); }
    const char *test = getenv("BB_TEST_CRASH");
    if (test && *test && strcmp(test, "0")) {
        HANDLE h = CreateThread(NULL, 0, test_thread, (LPVOID)(uintptr_t)(strcmp(test, "hang") == 0), 0, NULL);
        if (h) CloseHandle(h);
    }
}
#endif
