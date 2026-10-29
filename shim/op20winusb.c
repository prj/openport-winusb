// J2534 shim: loads the original op20pt32.dll and carries its device I/O over WinUSB.
// The DLL only opens the device, issues overlapped ReadFile/WriteFile, CancelIo and closes it, so those
// syscalls are redirected for handles on the OpenPort interface to a user-mode copy of openport.sys:
// a continuous reader feeding a ring buffer, reads served from the ring, writes straight to the OUT pipe.
// With the original openport.sys still bound, WinUsb_Initialize fails and everything passes through.
#include <windows.h>
#include <winternl.h>
#include <winusb.h>
#include <stdarg.h>

#define TACTRIX_GUID L"6d1781b7-c987-4f6c-8d4f-1efc098bea67"
#define STATUS_CANCELLED_ ((NTSTATUS)0xC0000120L)
#define STATUS_DEVICE_NOT_CONNECTED ((NTSTATUS)0xC000009DL)
#define STATUS_NOT_SUPPORTED_ ((NTSTATUS)0xC00000BBL)
#define ERR_DEVICE_NOT_CONNECTED 0x08

// As openport.sys: 10 reads of one packet each always pending, a 16 KB ring that drops bytes when full.
#define NREADS 10
#define READ_LEN 64
#define RING 0x4000
#define MAX_PARKED 8

typedef NTSTATUS (NTAPI *NtCreateFile_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI *NtRw_t)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
typedef NTSTATUS (NTAPI *NtClose_t)(HANDLE);
typedef NTSTATUS (NTAPI *NtCancel_t)(HANDLE, PIO_STATUS_BLOCK);

static NtCreateFile_t o_create;
static NtRw_t o_read, o_write;
static NtClose_t o_close;
static NtCancel_t o_cancel;

typedef struct { PIO_STATUS_BLOCK io; BYTE *buf; ULONG len; HANDLE ev; DWORD tid; } Parked;

typedef struct {
    HANDLE file, thread, stop;
    WINUSB_INTERFACE_HANDLE wu, io;
    UCHAR in, out;
    CRITICAL_SECTION lock;
    BYTE ring[RING];
    ULONG head, tail, count;
    Parked parked[MAX_PARKED];
    int nparked;
    NTSTATUS gone;
} Dev;

static Dev *g_dev[4];
static SRWLOCK g_lock = SRWLOCK_INIT;
static HANDLE g_log = INVALID_HANDLE_VALUE;

// Support log: opens, pipes and errors. Deliberately never device data.
static void logf(const char *fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char s[512];
    va_list a;
    va_start(a, fmt);
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    int n = wsprintfA(s, "%10lu ", (ULONG)(t.QuadPart * 1000000 / f.QuadPart % 4000000000));
    n += wvsprintfA(s + n, fmt, a);
    va_end(a);
    s[n++] = '\r';
    s[n++] = '\n';
    DWORD w;
    WriteFile(g_log, s, n, &w, NULL);
}

static Dev *find(HANDLE h) {
    Dev *d = NULL;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < 4; i++) if (h && g_dev[i] && g_dev[i]->file == h) { d = g_dev[i]; break; }
    ReleaseSRWLockShared(&g_lock);
    return d;
}

static BOOL thread_alive(DWORD tid) {
    HANDLE t = OpenThread(SYNCHRONIZE, FALSE, tid);
    if (!t) return FALSE;
    BOOL alive = WaitForSingleObject(t, 0) == WAIT_TIMEOUT;
    CloseHandle(t);
    return alive;
}

// Information first, Status last: GetOverlappedResult treats any Status other than STATUS_PENDING as done.
// The DLL kills its reader thread with a read pending; that OVERLAPPED was on its stack, so the write is guarded.
static void complete(PIO_STATUS_BLOCK io, HANDLE ev, NTSTATUS s, ULONG n) {
    __try {
        io->Information = n;
        MemoryBarrier();
        io->Status = s;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (ev) SetEvent(ev);
}

static ULONG take(Dev *d, BYTE *dst, ULONG len) {
    ULONG n = len < d->count ? len : d->count;
    for (ULONG i = 0; i < n; i++) {
        dst[i] = d->ring[d->tail];
        d->tail = (d->tail + 1) % RING;
    }
    d->count -= n;
    return n;
}

// Called with the lock held. As openport.sys mode 2: a read completes as soon as any byte is there.
static void serve_parked(Dev *d) {
    while (d->nparked && d->count) {
        Parked p = d->parked[0];
        memmove(d->parked, d->parked + 1, --d->nparked * sizeof(Parked));
        if (!thread_alive(p.tid)) continue;
        ULONG n;
        __try {
            n = take(d, p.buf, p.len);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        complete(p.io, p.ev, 0, n);
    }
}

// tid 0 fails every parked read; otherwise only that thread's, as NtCancelIoFile and thread exit do.
static void fail_parked(Dev *d, NTSTATUS s, DWORD tid) {
    int k = 0;
    for (int i = 0; i < d->nparked; i++) {
        Parked p = d->parked[i];
        if (tid && p.tid != tid) d->parked[k++] = p;
        else if (p.tid == GetCurrentThreadId() || thread_alive(p.tid)) complete(p.io, p.ev, s, 0);
    }
    d->nparked = k;
}

// A thread's pending I/O dies with it; the kernel completes it while the thread's stack is still there.
static void cancel_thread(DWORD tid) {
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < 4; i++) {
        Dev *d = g_dev[i];
        if (!d) continue;
        EnterCriticalSection(&d->lock);
        fail_parked(d, STATUS_CANCELLED_, tid);
        LeaveCriticalSection(&d->lock);
    }
    ReleaseSRWLockShared(&g_lock);
}

static DWORD WINAPI reader(void *arg) {
    Dev *d = arg;
    OVERLAPPED ov[NREADS] = { 0 };
    BYTE buf[NREADS][READ_LEN];
    HANDLE ev[NREADS];
    int errors = 0;
    for (int i = 0; i < NREADS; i++) {
        ev[i] = ov[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        WinUsb_ReadPipe(d->io, d->in, buf[i], READ_LEN, NULL, &ov[i]);
    }
    // Packets on one endpoint complete in the order the reads were queued, so take them oldest first.
    for (int i = 0;; i = (i + 1) % NREADS) {
        HANDLE w[2] = { ev[i], d->stop };
        if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) break;
        DWORD n = 0;
        if (WinUsb_GetOverlappedResult(d->io, &ov[i], &n, FALSE)) {
            errors = 0;
            EnterCriticalSection(&d->lock);
            for (DWORD j = 0; j < n && d->count < RING; j++, d->count++) {
                d->ring[d->head] = buf[i][j];
                d->head = (d->head + 1) % RING;
            }
            serve_parked(d);
            LeaveCriticalSection(&d->lock);
        } else {
            DWORD e = GetLastError();
            if (e == ERROR_OPERATION_ABORTED) break;
            logf("in pipe error %lu", e);
            // openport.sys resets the pipe on an I/O error; a device that is gone fails every retry.
            if (++errors > 3 || e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_BAD_COMMAND || e == ERROR_GEN_FAILURE) {
                EnterCriticalSection(&d->lock);
                d->gone = STATUS_DEVICE_NOT_CONNECTED;
                fail_parked(d, d->gone, 0);
                LeaveCriticalSection(&d->lock);
                break;
            }
            WinUsb_ResetPipe(d->io, d->in);
        }
        ResetEvent(ev[i]);
        WinUsb_ReadPipe(d->io, d->in, buf[i], READ_LEN, NULL, &ov[i]);
    }
    WinUsb_AbortPipe(d->io, d->in);
    for (int i = 0; i < NREADS; i++) {
        DWORD n;
        WinUsb_GetOverlappedResult(d->io, &ov[i], &n, TRUE);
        CloseHandle(ev[i]);
    }
    return 0;
}

// What openport.sys sends at device start: "atp 5 34", then 03 00 and the local time as eight ints.
static void send_clock(Dev *d) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int leap = (t.wYear % 4 == 0 && t.wYear % 100) || t.wYear % 400 == 0;
    int f[8] = { t.wSecond, t.wMinute, t.wHour, t.wDay, t.wMonth, t.wYear, t.wDayOfWeek,
        cum[t.wMonth - 1] + t.wDay + (leap && t.wMonth > 2) };
    BYTE pkt[44] = "atp 5 34\r\n\x03";
    memcpy(pkt + 12, f, sizeof f);
    ULONG n = 0;
    if (!WinUsb_WritePipe(d->io, d->out, pkt, sizeof pkt, &n, NULL)) {
        logf("clock failed %lu", GetLastError());
        return;
    }
    // The DLL stalls on the "arp 5 0" reply, which it never sees from the kernel driver: swallow it.
    ULONG ms = 200;
    BYTE reply[64];
    WinUsb_SetPipePolicy(d->io, d->in, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms);
    if (!WinUsb_ReadPipe(d->io, d->in, reply, sizeof reply, &n, NULL)) logf("clock reply missing %lu", GetLastError());
    ms = 0;
    WinUsb_SetPipePolicy(d->io, d->in, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms);
}

static BOOL find_pipes(WINUSB_INTERFACE_HANDLE wu, UCHAR *in, UCHAR *out) {
    USB_INTERFACE_DESCRIPTOR id;
    WINUSB_PIPE_INFORMATION pi;
    *in = *out = 0;
    if (!WinUsb_QueryInterfaceSettings(wu, 0, &id)) return FALSE;
    for (UCHAR i = 0; i < id.bNumEndpoints; i++) {
        if (!WinUsb_QueryPipe(wu, 0, i, &pi) || pi.PipeType != UsbdPipeTypeBulk) continue;
        if (USB_ENDPOINT_DIRECTION_IN(pi.PipeId)) { if (!*in) *in = pi.PipeId; }
        else if (!*out) *out = pi.PipeId;
    }
    return *in && *out;
}

static void attach(HANDLE h) {
    WINUSB_INTERFACE_HANDLE wu;
    if (!WinUsb_Initialize(h, &wu)) {
        logf("open %p: WinUsb_Initialize failed %lu, passing through", h, GetLastError());
        return;
    }
    Dev *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Dev));
    d->file = h;
    d->wu = d->io = wu;
    // As openport.sys: with several interfaces the J2534 pipes are on the second one (CC4D);
    // a device bound per interface (CC4C&MI_00) has them on the first.
    if (!find_pipes(wu, &d->in, &d->out))
        for (UCHAR i = 0; i < 4; i++) {
            WINUSB_INTERFACE_HANDLE a;
            if (!WinUsb_GetAssociatedInterface(wu, i, &a)) break;
            if (find_pipes(a, &d->in, &d->out)) { d->io = a; break; }
            WinUsb_Free(a);
        }
    int slot = -1;
    if (d->in && d->out) {
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < 4 && slot < 0; i++) if (!g_dev[i]) slot = i;
        ReleaseSRWLockExclusive(&g_lock);
    }
    logf("open %p: in %02X out %02X slot %d", h, d->in, d->out, slot);
    if (slot < 0) {
        if (d->io != wu) WinUsb_Free(d->io);
        WinUsb_Free(wu);
        HeapFree(GetProcessHeap(), 0, d);
        return;
    }
    InitializeCriticalSection(&d->lock);
    send_clock(d);
    d->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->thread = CreateThread(NULL, 0, reader, d, 0, NULL);
    AcquireSRWLockExclusive(&g_lock);
    g_dev[slot] = d;
    ReleaseSRWLockExclusive(&g_lock);
}

static void detach(Dev *d) {
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < 4; i++) if (g_dev[i] == d) g_dev[i] = NULL;
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(d->stop);
    WaitForSingleObject(d->thread, INFINITE);
    CloseHandle(d->thread);
    CloseHandle(d->stop);
    EnterCriticalSection(&d->lock);
    fail_parked(d, STATUS_CANCELLED_, 0);
    LeaveCriticalSection(&d->lock);
    DeleteCriticalSection(&d->lock);
    if (d->io != d->wu) WinUsb_Free(d->io);
    WinUsb_Free(d->wu);
    HeapFree(GetProcessHeap(), 0, d);
}

// Runs on every file open in the process: the name is not NUL-terminated, and a caller's bad pointer
// must fail in the kernel as it would without the hook, not crash here.
static int is_target(POBJECT_ATTRIBUTES oa) {
    static const WCHAR guid[] = TACTRIX_GUID;
    const int m = sizeof guid / 2 - 1;
    __try {
        if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer) return 0;
        const WCHAR *s = oa->ObjectName->Buffer;
        for (int i = 0; i + m <= oa->ObjectName->Length / 2; i++)
            if (!memcmp(s + i, guid, m * 2)) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

static NTSTATUS NTAPI h_create(PHANDLE ph, ACCESS_MASK a, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
    PLARGE_INTEGER sz, ULONG attr, ULONG share, ULONG disp, ULONG opt, PVOID ea, ULONG eal) {
    NTSTATUS s = o_create(ph, a, oa, io, sz, attr, share, disp, opt, ea, eal);
    // WinUSB needs an overlapped handle; the DLL's synchronous open is only a presence probe.
    if (s >= 0 && !(opt & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) && is_target(oa))
        attach(*ph);
    return s;
}

static NTSTATUS NTAPI h_read(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, PVOID buf,
    ULONG len, PLARGE_INTEGER off, PULONG key) {
    Dev *d = find(h);
    if (!d) return o_read(h, ev, apc, ctx, io, buf, len, off, key);
    if (apc) return STATUS_NOT_SUPPORTED_;
    // The low bit of ReadFile's event only suppresses completion-port notification.
    ev = (HANDLE)((ULONG_PTR)ev & ~(ULONG_PTR)1);
    if (ev) ResetEvent(ev);
    NTSTATUS s = STATUS_PENDING;
    EnterCriticalSection(&d->lock);
    if (d->gone) s = d->gone;
    else if (d->count && !d->nparked) {
        ULONG n = take(d, buf, len);
        complete(io, ev, 0, n);
        s = 0;
    } else if (d->nparked < MAX_PARKED) {
        io->Status = STATUS_PENDING;
        d->parked[d->nparked++] = (Parked){ io, buf, len, ev, GetCurrentThreadId() };
    } else s = STATUS_DEVICE_NOT_CONNECTED;
    LeaveCriticalSection(&d->lock);
    return s;
}

// ReadFile/WriteFile pass the OVERLAPPED as the IO_STATUS_BLOCK, which is what WinUsb_WritePipe takes.
static NTSTATUS NTAPI h_write(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, PIO_STATUS_BLOCK io, PVOID buf,
    ULONG len, PLARGE_INTEGER off, PULONG key) {
    Dev *d = find(h);
    if (!d) return o_write(h, ev, apc, ctx, io, buf, len, off, key);
    if (apc) return STATUS_NOT_SUPPORTED_;
    if (d->gone) return d->gone;
    OVERLAPPED *ov = (OVERLAPPED *)io;
    if (WinUsb_WritePipe(d->io, d->out, buf, len, NULL, ov)) return io->Status;
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) return STATUS_PENDING;
    logf("write failed %lu status %lX", e, io->Status);
    return io->Status != STATUS_PENDING && io->Status < 0 ? io->Status : STATUS_DEVICE_NOT_CONNECTED;
}

static NTSTATUS NTAPI h_cancel(HANDLE h, PIO_STATUS_BLOCK io) {
    Dev *d = find(h);
    if (d) {
        EnterCriticalSection(&d->lock);
        fail_parked(d, STATUS_CANCELLED_, GetCurrentThreadId());
        LeaveCriticalSection(&d->lock);
    }
    return o_cancel(h, io);
}

static NTSTATUS NTAPI h_close(HANDLE h) {
    Dev *d = find(h);
    if (d) detach(d);
    return o_close(h);
}

// WOW64 ntdll stubs are mov eax,svc / mov edx,dispatcher / call edx / ret n: 15 position-independent bytes.
// The original is stored before the jump goes in, since the detour can run the moment it does, and the
// first 8 bytes are swapped in one locked write so a thread entering the stub never sees half a jump.
static BOOL hook(const char *name, void *detour, void **orig) {
    BYTE *f = (BYTE *)GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
    if (!f || ((DWORD)f & 7) || f[0] != 0xB8 || f[5] != 0xBA || f[10] != 0xFF || f[11] != 0xD2 || f[12] != 0xC2)
        return FALSE;
    BYTE *tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return FALSE;
    memcpy(tramp, f, 15);
    DWORD old;
    VirtualProtect(tramp, 16, PAGE_EXECUTE_READ, &old);
    *orig = tramp;
    LONGLONG cur = *(volatile LONGLONG *)f, patched = cur;
    BYTE *p = (BYTE *)&patched;
    p[0] = 0xE9;
    *(DWORD *)(p + 1) = (DWORD)((BYTE *)detour - (f + 5));
    VirtualProtect(f, 8, PAGE_EXECUTE_READWRITE, &old);
    InterlockedCompareExchange64((volatile LONGLONG *)f, patched, cur);
    VirtualProtect(f, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 8);
    return TRUE;
}

// NtCreateFile last: until it is hooked no handle can be attached, so the other detours only pass through.
static BOOL install_hooks(void) {
    return hook("NtClose", h_close, (void **)&o_close) && hook("NtCancelIoFile", h_cancel, (void **)&o_cancel) &&
        hook("NtWriteFile", h_write, (void **)&o_write) && hook("NtReadFile", h_read, (void **)&o_read) &&
        hook("NtCreateFile", h_create, (void **)&o_create);
}

typedef long (WINAPI *Open_t)(void *, unsigned long *);
typedef long (WINAPI *Close_t)(unsigned long);
typedef long (WINAPI *Connect_t)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
typedef long (WINAPI *Disconnect_t)(unsigned long);
typedef long (WINAPI *ReadMsgs_t)(unsigned long, void *, unsigned long *, unsigned long);
typedef long (WINAPI *WriteMsgs_t)(unsigned long, void *, unsigned long *, unsigned long);
typedef long (WINAPI *StartPeriodic_t)(unsigned long, void *, unsigned long *, unsigned long);
typedef long (WINAPI *StopId_t)(unsigned long, unsigned long);
typedef long (WINAPI *StartFilter_t)(unsigned long, unsigned long, void *, void *, void *, unsigned long *);
typedef long (WINAPI *SetVoltage_t)(unsigned long, unsigned long, unsigned long);
typedef long (WINAPI *ReadVersion_t)(unsigned long, char *, char *, char *);
typedef long (WINAPI *GetLastError_t)(char *);
typedef long (WINAPI *Ioctl_t)(unsigned long, unsigned long, void *, void *);

static struct {
    Open_t open; Close_t close; Connect_t connect; Disconnect_t disconnect;
    ReadMsgs_t read; WriteMsgs_t write; StartPeriodic_t start_periodic; StopId_t stop_periodic;
    StartFilter_t start_filter; StopId_t stop_filter; SetVoltage_t voltage; ReadVersion_t version;
    GetLastError_t last_error; Ioctl_t ioctl;
} g_fn;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL g_hooked;
static HMODULE g_self;

// The installer puts this shim at Tactrix's op20pt32.dll and the original beside it under a new name.
static BOOL CALLBACK load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH - 32);
    while (n && path[n - 1] != L'\\') n--;
    lstrcpyW(path + n, L"op20pt32_tactrix.dll");
    HMODULE m = LoadLibraryW(path);
    logf("original %s", m ? "loaded" : "NOT FOUND");
    if (!m) return TRUE;
#define BIND(field, name) g_fn.field = (void *)GetProcAddress(m, name)
    BIND(open, "PassThruOpen"); BIND(close, "PassThruClose");
    BIND(connect, "PassThruConnect"); BIND(disconnect, "PassThruDisconnect");
    BIND(read, "PassThruReadMsgs"); BIND(write, "PassThruWriteMsgs");
    BIND(start_periodic, "PassThruStartPeriodicMsg"); BIND(stop_periodic, "PassThruStopPeriodicMsg");
    BIND(start_filter, "PassThruStartMsgFilter"); BIND(stop_filter, "PassThruStopMsgFilter");
    BIND(voltage, "PassThruSetProgrammingVoltage"); BIND(version, "PassThruReadVersion");
    BIND(last_error, "PassThruGetLastError"); BIND(ioctl, "PassThruIoctl");
#undef BIND
    return TRUE;
}

#define FWD(field, ...) \
    InitOnceExecuteOnce(&g_once, load, NULL, NULL); \
    return g_fn.field && g_hooked ? g_fn.field(__VA_ARGS__) : ERR_DEVICE_NOT_CONNECTED

long WINAPI PassThruOpen(void *name, unsigned long *id) { FWD(open, name, id); }
long WINAPI PassThruClose(unsigned long id) { FWD(close, id); }
long WINAPI PassThruConnect(unsigned long dev, unsigned long proto, unsigned long flags, unsigned long baud, unsigned long *ch) { FWD(connect, dev, proto, flags, baud, ch); }
long WINAPI PassThruDisconnect(unsigned long ch) { FWD(disconnect, ch); }
long WINAPI PassThruReadMsgs(unsigned long ch, void *msg, unsigned long *n, unsigned long t) { FWD(read, ch, msg, n, t); }
long WINAPI PassThruWriteMsgs(unsigned long ch, void *msg, unsigned long *n, unsigned long t) { FWD(write, ch, msg, n, t); }
long WINAPI PassThruStartPeriodicMsg(unsigned long ch, void *msg, unsigned long *id, unsigned long t) { FWD(start_periodic, ch, msg, id, t); }
long WINAPI PassThruStopPeriodicMsg(unsigned long ch, unsigned long id) { FWD(stop_periodic, ch, id); }
long WINAPI PassThruStartMsgFilter(unsigned long ch, unsigned long type, void *mask, void *pattern, void *flow, unsigned long *id) { FWD(start_filter, ch, type, mask, pattern, flow, id); }
long WINAPI PassThruStopMsgFilter(unsigned long ch, unsigned long id) { FWD(stop_filter, ch, id); }
long WINAPI PassThruSetProgrammingVoltage(unsigned long dev, unsigned long pin, unsigned long v) { FWD(voltage, dev, pin, v); }
long WINAPI PassThruReadVersion(unsigned long dev, char *fw, char *dll, char *api) { FWD(version, dev, fw, dll, api); }
long WINAPI PassThruIoctl(unsigned long ch, unsigned long id, void *in, void *out) { FWD(ioctl, ch, id, in, out); }

long WINAPI PassThruGetLastError(char *desc) {
    InitOnceExecuteOnce(&g_once, load, NULL, NULL);
    if (g_fn.last_error && g_hooked) return g_fn.last_error(desc);
    lstrcpyA(desc, g_hooked ? "op20pt32_tactrix.dll not found" : "ntdll hook failed");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        // The hooks point into this DLL, so it must outlive any FreeLibrary (J2534-Sharp unloads on dispose).
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)DllMain, &self);
        // Logging is on when OP20WINUSB_LOG names a file, or when op20winusb.log exists beside the DLL.
        WCHAR path[MAX_PATH];
        if (GetEnvironmentVariableW(L"OP20WINUSB_LOG", path, MAX_PATH))
            g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
        else if (GetModuleFileNameW(inst, path, MAX_PATH - 4)) {
            lstrcpyW(path + lstrlenW(path) - 3, L"log");
            g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        }
        g_hooked = install_hooks();
        logf("hooks %s", g_hooked ? "installed" : "FAILED");
    } else if (reason == DLL_THREAD_DETACH) {
        cancel_thread(GetCurrentThreadId());
    }
    return TRUE;
}
