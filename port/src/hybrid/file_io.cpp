// Native file I/O for the hybrid bring-up. The game opens every asset as "D:\..." and
// reads loose files (no archive). On Xbox "D:" is the disc (\Device\CdRom0); here it
// maps 1:1 to the extracted/ folder. We implement the Nt* file kernel imports with
// Win32 file APIs. Synchronous completion is sufficient: the XAPI ReadFile wrapper and
// GetOverlappedResult both accept an already-completed (non-pending) IO_STATUS_BLOCK.
//
// Struct layouts (Xbox): OBJECT_ATTRIBUTES {HANDLE Root; ANSI_STRING* Name; ULONG Attr}
// ANSI_STRING {u16 Length; u16 Max; char* Buffer}; IO_STATUS_BLOCK {NTSTATUS; ULONG Info}.
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace tj::hybrid {

bool CheckAssetOverride(const char* relPath, char* outPath, size_t cap); // mod_manager.cpp

static const int32_t STATUS_SUCCESS = 0;
static const int32_t STATUS_NO_SUCH_FILE = (int32_t)0xC000000F;
static const int32_t STATUS_OBJECT_NAME_NOT_FOUND = (int32_t)0xC0000034;
static const int32_t NT_NO_MORE_FILES = (int32_t)0x80000006;
static const int32_t NT_INVALID_HANDLE = (int32_t)0xC0000008;

static char g_root[MAX_PATH] = "."; // extracted/ folder (game's D:\ )

void SetAssetRoot(const char* dir) { strncpy_s(g_root, dir, _TRUNCATE); }
const char* AssetRoot() { return g_root; }   // net_lan.cpp hashes this tree for dataHash

// --- WHERE THE GAME MAY WRITE ------------------------------------------------
// A Program Files install is read-only for a standard user, so saves and tomjerry.ini cannot
// live next to the exe there -- they would fail silently, which is exactly the symptom the
// save bug had. Everything writable therefore goes to %LOCALAPPDATA%\TomJerryWOW.
//
// TWO ESCAPE HATCHES, both deliberate:
//   * a `portable.txt` beside the exe forces the old behaviour, so an existing copied folder
//     (and the LAN test harness, which wants its files where it can see them) keeps working;
//   * an existing tomjerry.ini beside the exe, or an existing _SAVES beside the GAME DATA,
//     is COPIED forward on first run. Upgrading must never cost someone their save.
//
// The two migrations happen at different moments because their old locations differ. The ini
// really did live beside the exe, and UserDataDir() knows that folder immediately -- so that
// one runs here, before hybrid_run reads the boot resolution out of it. The saves lived under
// the ASSET ROOT (extracted\_SAVES), which is not known until SetAssetRoot has run, so that
// one is MigrateSaves(), called straight after.
static char g_userDir[MAX_PATH] = "";
static bool g_portable = false;      // valid once UserDataDir() has resolved

static void ExeDir(char* out, size_t cap) {
    GetModuleFileNameA(nullptr, out, (DWORD)cap);
    char* s = strrchr(out, '\\'); if (s) s[1] = 0;
}
static bool Exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

// "Is there anything in here worth keeping?" -- the migration guard. It deliberately tests
// EMPTINESS rather than existence, because SaveRoot() creates the destination folder the first
// time anything asks for it; an Exists() guard would then skip every migration forever.
static bool DirHasEntries(const char* dir) {
    char pat[MAX_PATH]; _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        if (fd.cFileName[0] == '.' && (!fd.cFileName[1] ||
            (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
        any = true; break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return any;
}

// Copy a directory tree (one level of subdirectories is all a save ever has).
static void CopyTree(const char* from, const char* to) {
    CreateDirectoryA(to, nullptr);
    char pat[MAX_PATH]; _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", from);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' && (!fd.cFileName[1] ||
            (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
        char a[MAX_PATH], b[MAX_PATH];
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\%s", from, fd.cFileName);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyTree(a, b);
        else CopyFileA(a, b, TRUE);          // TRUE = never overwrite what is already there
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

const char* UserDataDir() {
    if (g_userDir[0]) return g_userDir;
    char exe[MAX_PATH]; ExeDir(exe, sizeof exe);
    char marker[MAX_PATH];
    _snprintf_s(marker, sizeof marker, _TRUNCATE, "%sportable.txt", exe);
    const char* local = nullptr; size_t ln = 0;
    char* e = nullptr; _dupenv_s(&e, &ln, "LOCALAPPDATA"); local = e;
    if (Exists(marker) || !local || !*local) {
        g_portable = true;
        _snprintf_s(g_userDir, sizeof g_userDir, _TRUNCATE, "%s", exe);
        if (g_userDir[0]) { size_t n = strlen(g_userDir); if (n && g_userDir[n-1] == '\\') g_userDir[n-1] = 0; }
        free(e);
        printf("[fs] user data: %s (portable)\n", g_userDir);
        return g_userDir;
    }
    _snprintf_s(g_userDir, sizeof g_userDir, _TRUNCATE, "%s\\TomJerryWOW", local);
    free(e);
    CreateDirectoryA(g_userDir, nullptr);

    char oldSaves[MAX_PATH], newSaves[MAX_PATH], oldIni[MAX_PATH], newIni[MAX_PATH];
    _snprintf_s(oldSaves, sizeof oldSaves, _TRUNCATE, "%s_SAVES", exe);
    _snprintf_s(newSaves, sizeof newSaves, _TRUNCATE, "%s\\_SAVES", g_userDir);
    _snprintf_s(oldIni, sizeof oldIni, _TRUNCATE, "%stomjerry.ini", exe);
    _snprintf_s(newIni, sizeof newIni, _TRUNCATE, "%s\\tomjerry.ini", g_userDir);
    if (Exists(oldSaves) && !DirHasEntries(newSaves)) {
        CopyTree(oldSaves, newSaves);
        printf("[fs] migrated the existing save to %s\n", newSaves);
    }
    if (Exists(oldIni) && !Exists(newIni)) CopyFileA(oldIni, newIni, TRUE);
    printf("[fs] user data: %s\n", g_userDir);
    return g_userDir;
}

// Where save games actually live. Every writable-drive path (T:, U:, the harddisk device)
// lands under here rather than under the asset root, so a Program Files install can save.
//
// EXCEPT under `portable.txt`, which means "behave exactly as before" -- and before, saves
// lived at <asset root>\_SAVES. Honouring that keeps the dev build and the LAN harness on the
// save they already have, and a folder someone deliberately marked portable is writable by
// definition. That branch depends on g_root, so it is not cached: MapPath only ever runs after
// SetAssetRoot, but the first UserDataDir() call happens before it.
static char g_saveDir[MAX_PATH] = "";
const char* SaveRoot() {
    UserDataDir();                   // resolves g_portable on the first call
    if (g_portable) {
        static char portableSaves[MAX_PATH];
        _snprintf_s(portableSaves, sizeof portableSaves, _TRUNCATE, "%s\\_SAVES", g_root);
        CreateDirectoryA(portableSaves, nullptr);
        return portableSaves;
    }
    if (!g_saveDir[0]) {
        _snprintf_s(g_saveDir, sizeof g_saveDir, _TRUNCATE, "%s\\_SAVES", UserDataDir());
        CreateDirectoryA(g_saveDir, nullptr);
        printf("[fs] saves: %s\n", g_saveDir);
    }
    return g_saveDir;
}

// Normalise for comparison: g_root arrives from the command line and may be relative ("."),
// while the user dir is always absolute. Without this a portable build would decide its own
// save folder is a DIFFERENT folder and copy it onto itself.
static void FullPath(const char* in, char* out, size_t cap) {
    if (!GetFullPathNameA(in, (DWORD)cap, out, nullptr)) strncpy_s(out, cap, in, _TRUNCATE);
    size_t n = strlen(out); if (n > 3 && out[n-1] == '\\') out[n-1] = 0;   // keep "C:\"
}

// The saves used to live under the ASSET ROOT (extracted\_SAVES), which is not known until
// SetAssetRoot has run -- so unlike the ini migration this cannot happen inside UserDataDir().
// Call it straight after SetAssetRoot(). COPY, NEVER MOVE, and never overwrite: a botched
// upgrade must not be able to cost anyone their save. The old folder is left exactly as it was.
void MigrateSaves() {
    char oldSaves[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    _snprintf_s(oldSaves, sizeof oldSaves, _TRUNCATE, "%s\\_SAVES", g_root);
    FullPath(oldSaves, from, sizeof from);
    FullPath(SaveRoot(), to, sizeof to);
    if (_stricmp(from, to) == 0) return;              // portable: same folder, nothing to do
    if (!Exists(from) || DirHasEntries(to)) return;   // nothing to copy, or already migrated
    CopyTree(from, to);
    printf("[fs] migrated the existing save %s -> %s\n", from, to);
}

// --- NT handle table -------------------------------------------------------
struct FileObj {
    HANDLE win; int64_t pos; bool used;
    char   path[MAX_PATH];        // mapped Windows path (directory enumeration)
    HANDLE find; bool findOpen;   // NtQueryDirectoryFile state
};
static FileObj g_files[512];
static CRITICAL_SECTION g_lock; static bool g_lockInit = false;
static void lockInit() { if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; } }

static HANDLE ResolveEvent(uint32_t h);   // defined with the event table below

static uint32_t AllocHandle(HANDLE win, const char* winPath = nullptr) {
    lockInit(); EnterCriticalSection(&g_lock);
    for (int i = 1; i < 512; ++i) if (!g_files[i].used) {
        g_files[i] = {}; g_files[i].win = win; g_files[i].used = true;
        if (winPath) strncpy_s(g_files[i].path, winPath, _TRUNCATE);
        LeaveCriticalSection(&g_lock);
        return 0x80000000u | (uint32_t)i;          // tag so it's distinct from real handles
    }
    LeaveCriticalSection(&g_lock); return 0;
}
static FileObj* Resolve(uint32_t h) {
    if ((h & 0x80000000u) == 0) return nullptr;
    int i = (int)(h & 0x7FFFFFFF);
    if (i <= 0 || i >= 512 || !g_files[i].used) return nullptr;
    return &g_files[i];
}

// Map an Xbox object-name path to a Windows path. Disc paths (D:\ etc.) land under
// extracted/; the writable drives (T:\ title data, U:\ user saves, the harddisk device)
// land under SaveRoot() -- the USER data folder, not the asset root, so the game can still
// save when the assets sit somewhere read-only like Program Files.
static void MapPath(const char* nt, char* out, size_t cap) {
    const char* p = nt;
    bool save = false;
    const char* savePre[] = { "\\??\\T:\\", "\\??\\U:\\", "T:\\", "U:\\",
                              "\\Device\\Harddisk0\\partition1\\" };
    for (const char* pre : savePre) {
        size_t n = strlen(pre);
        if (_strnicmp(p, pre, (int)n) == 0) { p += n; save = true; break; }
    }
    if (!save) {
        const char* prefixes[] = { "\\Device\\CdRom0\\", "\\??\\D:\\", "D:\\", "Z:\\" };
        for (const char* pre : prefixes) {
            size_t n = strlen(pre);
            if (_strnicmp(p, pre, (int)n) == 0) { p += n; break; }
        }
    }
    if (save) {
        _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", SaveRoot(), p);
    } else {
        if (!CheckAssetOverride(p, out, cap)) {
            _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", g_root, p);
        }
    }
    for (char* c = out; *c; ++c) if (*c == '/') *c = '\\';
}

// Create every missing directory on the way to `path` (excluding the final component
// unless includeLast). Needed for save-game writes into fresh _SAVES subtrees.
static void EnsureDirs(const char* path, bool includeLast) {
    char tmp[MAX_PATH]; strcpy_s(tmp, path);
    char* lastSlash = strrchr(tmp, '\\');
    if (!includeLast && lastSlash) *lastSlash = 0;
    for (char* c = tmp + 3; *c; ++c) {           // skip "X:\"
        if (*c == '\\') { *c = 0; CreateDirectoryA(tmp, nullptr); *c = '\\'; }
    }
    CreateDirectoryA(tmp, nullptr);
}

static const char* NameFromObjAttr(void* objAttr, char* buf, size_t cap) {
    if (!objAttr) return nullptr;
    void* ansi = (void*)(uintptr_t)*(uint32_t*)((char*)objAttr + 4);   // ANSI_STRING* (gptr)
    if (!ansi) return nullptr;
    uint16_t len = *(uint16_t*)((char*)ansi + 0);
    char* str = (char*)(uintptr_t)*(uint32_t*)((char*)ansi + 4);        // Buffer (gptr)
    if (!str) return nullptr;
    if (len >= cap) len = (uint16_t)(cap - 1);
    memcpy(buf, str, len); buf[len] = 0;
    return buf;
}

static void SetIosb(void* iosb, int32_t status, uint32_t info) {
    if (iosb) { *(int32_t*)iosb = status; *(uint32_t*)((char*)iosb + 4) = info; }
}

// NtCreateFile(PHANDLE, ACCESS, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
//              ULONG FileAttrs, ULONG Share, ULONG Disposition, ULONG Options)
extern "C" int32_t __stdcall Hy_NtCreateFile(uint32_t* outHandle, uint32_t access, void* objAttr,
        void* iosb, void* allocSize, uint32_t fileAttrs, uint32_t share,
        uint32_t disposition, uint32_t options) {
    (void)access; (void)allocSize; (void)fileAttrs; (void)share; (void)options;
    char nt[MAX_PATH], win[MAX_PATH];
    if (!NameFromObjAttr(objAttr, nt, sizeof(nt))) { SetIosb(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0); return STATUS_OBJECT_NAME_NOT_FOUND; }
    MapPath(nt, win, sizeof(win));
    // Directory create/open (save-game folders): make it exist, return a real dir handle.
    if (options & 0x00000001 /*FILE_DIRECTORY_FILE*/) {
        EnsureDirs(win, true);
        HANDLE dh = CreateFileA(win, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dh == INVALID_HANDLE_VALUE) {
            SetIosb(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0); *outHandle = 0;
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        *outHandle = AllocHandle(dh, win);
        SetIosb(iosb, STATUS_SUCCESS, disposition == 1 ? 1u : 2u /*FILE_CREATED*/);
        printf("[file] DIR  %s -> handle %08x\n", nt, *outHandle);
        return STATUS_SUCCESS;
    }
    DWORD winDisp = OPEN_EXISTING;
    switch (disposition) { case 2: winDisp = CREATE_NEW; break; case 5: winDisp = CREATE_ALWAYS; break;
        case 1: winDisp = OPEN_EXISTING; break; case 3: winDisp = OPEN_ALWAYS; break; case 4: winDisp = TRUNCATE_EXISTING; break; }
    DWORD accessW = (disposition == 1) ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    if (disposition != 1) EnsureDirs(win, false);   // creating a file: parents must exist
    HANDLE h = CreateFileA(win, accessW, FILE_SHARE_READ, nullptr, winDisp,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("[file] MISS %s -> %s (err %lu)\n", nt, win, GetLastError());
        SetIosb(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0); *outHandle = 0; return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    uint32_t nth = AllocHandle(h);
    *outHandle = nth;
    SetIosb(iosb, STATUS_SUCCESS, 1 /*FILE_OPENED*/);
    printf("[file] OPEN %s -> handle %08x\n", nt, nth);
    return STATUS_SUCCESS;
}

// NtOpenFile(PHANDLE, ACCESS, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG Share, ULONG Options)
extern "C" int32_t __stdcall Hy_NtOpenFile(uint32_t* outHandle, uint32_t access, void* objAttr,
        void* iosb, uint32_t share, uint32_t options) {
    return Hy_NtCreateFile(outHandle, access, objAttr, iosb, nullptr, 0, share, 1 /*FILE_OPEN*/, options);
}

// NtReadFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcCtx, PIO_STATUS_BLOCK,
//            PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset)
extern "C" int32_t __stdcall Hy_NtReadFile(uint32_t handle, uint32_t event, void* apc, void* apcCtx,
        void* iosb, void* buffer, uint32_t length, int64_t* byteOffset) {
    (void)apc; (void)apcCtx;
    FileObj* f = Resolve(handle);
    if (!f) { SetIosb(iosb, STATUS_NO_SUCH_FILE, 0); return STATUS_NO_SUCH_FILE; }
    int64_t off = (byteOffset && *byteOffset >= 0) ? *byteOffset : f->pos;
    LARGE_INTEGER li; li.QuadPart = off;
    SetFilePointerEx(f->win, li, nullptr, FILE_BEGIN);
    DWORD got = 0;
    BOOL ok = ReadFile(f->win, buffer, length, &got, nullptr);
    if (!ok) { SetIosb(iosb, STATUS_NO_SUCH_FILE, 0); return STATUS_NO_SUCH_FILE; }
    f->pos = off + got;
    SetIosb(iosb, STATUS_SUCCESS, got);
    // Completion is synchronous; the XAPI wrapper reads IOSB and does not wait. Signal a
    // real event only if one we created was supplied.
    HANDLE ev = ResolveEvent(event);
    if (ev) SetEvent(ev);
    return STATUS_SUCCESS;
}

// NtWriteFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcCtx, PIO_STATUS_BLOCK,
//             PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset)
// Was stubbed to "succeed" and write nothing, which is why SAVE GAME reported success and
// produced an empty _SAVES tree. Same shape as NtReadFile: synchronous, IOSB carries the
// byte count, and an event is only signalled if it is one we created.
extern "C" int32_t __stdcall Hy_NtWriteFile(uint32_t handle, uint32_t event, void* apc, void* apcCtx,
        void* iosb, const void* buffer, uint32_t length, int64_t* byteOffset) {
    (void)apc; (void)apcCtx;
    FileObj* f = Resolve(handle);
    if (!f) { SetIosb(iosb, STATUS_NO_SUCH_FILE, 0); return STATUS_NO_SUCH_FILE; }
    int64_t off = (byteOffset && *byteOffset >= 0) ? *byteOffset : f->pos;
    LARGE_INTEGER li; li.QuadPart = off;
    SetFilePointerEx(f->win, li, nullptr, FILE_BEGIN);
    DWORD put = 0;
    BOOL ok = WriteFile(f->win, buffer, length, &put, nullptr);
    if (!ok) {
        printf("[file] WRITE FAILED handle %08x len %u (err %lu)\n", handle, length, GetLastError());
        SetIosb(iosb, STATUS_NO_SUCH_FILE, 0); return STATUS_NO_SUCH_FILE;
    }
    f->pos = off + put;
    SetIosb(iosb, STATUS_SUCCESS, put);
    HANDLE ev = ResolveEvent(event);
    if (ev) SetEvent(ev);
    return STATUS_SUCCESS;
}

// NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID Info, ULONG Len, FS_INFO_CLASS)
// The ONLY class the game asks for is 3 = FileFsSizeInformation, from the XDK's
// GetDiskFreeSpaceEx at 0x738FC, which the save code calls to decide whether there is room.
// It was stubbed to return STATUS_SUCCESS without touching the buffer, so the game read
// uninitialised stack as the free-space figure -- that is where "you need to free 28307948
// more blocks" came from. Reporting the REAL free space of the PC disk that holds _SAVES is
// the whole fix.
//   FILE_FS_SIZE_INFORMATION { LARGE_INTEGER TotalUnits; LARGE_INTEGER AvailUnits;
//                              ULONG SectorsPerUnit; ULONG BytesPerSector; }  (0x18 bytes)
// Units are reported as the Xbox's own 16 KB blocks (32 x 512 B) so the game's block
// arithmetic keeps producing the numbers a player would have seen on a real console.
extern "C" int32_t __stdcall Hy_NtQueryVolumeInformationFile(uint32_t handle, void* iosb,
        void* info, uint32_t length, uint32_t infoClass) {
    (void)handle;
    if (infoClass != 3 || !info || length < 0x18) {
        SetIosb(iosb, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    ULARGE_INTEGER avail = {}, total = {}, freeTotal = {};
    // The volume that matters is the one the save is about to be WRITTEN to, which since the
    // move to the user data folder is no longer necessarily the one holding the assets.
    if (!GetDiskFreeSpaceExA(SaveRoot(), &avail, &total, &freeTotal)) {
        avail.QuadPart = 1ull << 30;            // 1 GB: never block a save on a query failure
        total.QuadPart = 1ull << 31;
    }
    const uint32_t kBytesPerSector = 512, kSectorsPerUnit = 32;   // 16 KB Xbox block
    const uint64_t kUnit = (uint64_t)kBytesPerSector * kSectorsPerUnit;
    *(uint64_t*)info                     = total.QuadPart / kUnit;
    *(uint64_t*)((char*)info + 8)        = avail.QuadPart / kUnit;
    *(uint32_t*)((char*)info + 0x10)     = kSectorsPerUnit;
    *(uint32_t*)((char*)info + 0x14)     = kBytesPerSector;
    SetIosb(iosb, STATUS_SUCCESS, 0x18);
    return STATUS_SUCCESS;
}

// --- Event objects. Tagged INDICES into a table (0x40000000|idx) so we only ever touch
// handles we actually created -- never CloseHandle on a garbage handle from a stubbed
// kernel call (which raises STATUS_INVALID_HANDLE under strict handle checking). ---
struct EventObj { HANDLE win; bool used; };
static EventObj g_events[256];
static HANDLE ResolveEvent(uint32_t h) {
    if ((h & 0xC0000000u) != 0x40000000u) return nullptr;
    int i = (int)(h & 0x3FFFFFFF);
    if (i <= 0 || i >= 256 || !g_events[i].used) return nullptr;
    return g_events[i].win;
}
static uint32_t AllocEvent(HANDLE win) {
    lockInit(); EnterCriticalSection(&g_lock);
    for (int i = 1; i < 256; ++i) if (!g_events[i].used) {
        g_events[i] = { win, true }; LeaveCriticalSection(&g_lock); return 0x40000000u | (uint32_t)i;
    }
    LeaveCriticalSection(&g_lock); return 0;
}

extern "C" int32_t __stdcall Hy_NtClose(uint32_t handle) {
    FileObj* f = Resolve(handle);
    if (f) {
        if (f->findOpen) { FindClose(f->find); f->findOpen = false; }
        CloseHandle(f->win); f->used = false; return STATUS_SUCCESS;
    }
    if ((handle & 0xC0000000u) == 0x40000000u) {
        int i = (int)(handle & 0x3FFFFFFF);
        if (i > 0 && i < 256 && g_events[i].used) { CloseHandle(g_events[i].win); g_events[i].used = false; }
    }
    return STATUS_SUCCESS; // unknown handles: succeed silently (never touch real handles)
}

// NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
//   FileInformation, Length, FileInformationClass, FileMask(POBJECT_STRING), RestartScan)
// -- 10 args, ret 0x28. Used by XFindFirst/NextSaveGame to enumerate U:\ save folders.
// Returns ONE Xbox FILE_DIRECTORY_INFORMATION (ANSI name at +0x40) per call;
// STATUS_NO_SUCH_FILE on an empty first scan, STATUS_NO_MORE_FILES at the end --
// both negative, which is what makes the XAPI find-wrapper bail cleanly when there are
// no saves (the old always-success stub made it parse an unfilled buffer and crash).
// ⚠ `fileMask` IS A uint32_t, NOT A void*, AND THAT IS LOAD-BEARING — see the 4-byte-slot
// rule at InvokeArgBytes (dispatch.cpp). The kernel table is UNTYPED: the marshaler calls
// every shim through an all-uint32_t prototype. Arguments 1-8 survive a pointer declaration
// by luck (AAPCS64 passes them in registers, and a `w` write zero-extends into `x`), but
// argument 9 onward go ON THE STACK, where the caller writes a 4-byte slot and a `void*`
// callee reads EIGHT — assembling the pointer out of two adjacent argument slots.
// This is the tenth-argument function, so it is the one shim in the table where that bites.
// It cost a crash on Save Game: `fileMask` arrived as 0x487a60_00000000 and the deref of
// fileMask+4 faulted at 0x487a60_00000004. x86 never saw it (both types are 4 bytes there).
extern "C" int32_t __stdcall Hy_NtQueryDirectoryFile(uint32_t handle, uint32_t event, void* apc,
        void* apcCtx, void* iosb, void* info, uint32_t length, uint32_t infoClass,
        uint32_t fileMask, uint32_t restart) {
    (void)event; (void)apc; (void)apcCtx; (void)infoClass;
    FileObj* f = Resolve(handle);
    if (!f || !f->path[0]) { SetIosb(iosb, NT_INVALID_HANDLE, 0); return NT_INVALID_HANDLE; }
    if (restart && f->findOpen) { FindClose(f->find); f->findOpen = false; }
    char mask[128] = "*";
    if (fileMask) {
        const char* fm = (const char*)(uintptr_t)fileMask;   // guest OBJECT_STRING
        uint16_t ml = *(const uint16_t*)fm;
        char* mb = (char*)(uintptr_t)*(const uint32_t*)(fm + 4);   // gptr
        if (mb && ml && ml < sizeof(mask) - 1) { memcpy(mask, mb, ml); mask[ml] = 0; }
    }
    WIN32_FIND_DATAA fd;
    bool first = !f->findOpen;
    for (;;) {
        if (!f->findOpen) {
            char pattern[MAX_PATH + 130];
            _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\%s", f->path, mask);
            f->find = FindFirstFileA(pattern, &fd);
            if (f->find == INVALID_HANDLE_VALUE) {
                SetIosb(iosb, STATUS_NO_SUCH_FILE, 0); return STATUS_NO_SUCH_FILE;
            }
            f->findOpen = true;
        } else if (!FindNextFileA(f->find, &fd)) {
            int32_t st = first ? STATUS_NO_SUCH_FILE : NT_NO_MORE_FILES;
            SetIosb(iosb, st, 0); return st;
        }
        if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) break;
    }
    // One Xbox FILE_DIRECTORY_INFORMATION: hdr 0x40 bytes, ANSI name appended.
    uint32_t nameLen = (uint32_t)strlen(fd.cFileName);
    uint32_t need = 0x40 + nameLen;
    if (!info || length < need) { SetIosb(iosb, NT_NO_MORE_FILES, 0); return NT_NO_MORE_FILES; }
    memset(info, 0, 0x40);
    uint8_t* di = (uint8_t*)info;
    *(int64_t*)(di + 0x28) = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;  // EndOfFile
    *(int64_t*)(di + 0x30) = *(int64_t*)(di + 0x28);                               // AllocationSize
    *(uint32_t*)(di + 0x38) = fd.dwFileAttributes;
    *(uint32_t*)(di + 0x3C) = nameLen;
    memcpy(di + 0x40, fd.cFileName, nameLen);
    SetIosb(iosb, STATUS_SUCCESS, need);
    printf("[file] DIRENUM %s -> %s%s\n", f->path, fd.cFileName,
           (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "\\" : "");
    return STATUS_SUCCESS;
}

// NtCreateEvent(PHANDLE, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN InitialState)
extern "C" int32_t __stdcall Hy_NtCreateEvent(uint32_t* outHandle, void* objAttr,
        uint32_t type, uint32_t initialState) {
    (void)objAttr;
    HANDLE h = CreateEventA(nullptr, type == 0 /*NotificationEvent=manual*/, initialState != 0, nullptr);
    *outHandle = h ? AllocEvent(h) : 0;
    return STATUS_SUCCESS;
}
extern "C" int32_t __stdcall Hy_NtSetEvent(uint32_t handle, uint32_t* prevState) {
    HANDLE ev = ResolveEvent(handle); if (ev) SetEvent(ev);
    if (prevState) *prevState = 0;
    return STATUS_SUCCESS;
}
extern "C" int32_t __stdcall Hy_NtWaitForSingleObject(uint32_t handle, uint32_t alertable, void* timeout) {
    (void)alertable; (void)timeout;
    HANDLE ev = ResolveEvent(handle); if (ev) WaitForSingleObject(ev, 0);
    return STATUS_SUCCESS;   // our I/O never truly pends
}

// NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID Info, ULONG Len, FILE_INFO_CLASS)
extern "C" int32_t __stdcall Hy_NtQueryInformationFile(uint32_t handle, void* iosb, void* info,
        uint32_t len, uint32_t infoClass) {
    FileObj* f = Resolve(handle);
    if (!f) { SetIosb(iosb, STATUS_NO_SUCH_FILE, 0); return STATUS_NO_SUCH_FILE; }
    if (infoClass == 14 && len >= 8) {                 // FilePositionInformation
        *(int64_t*)info = f->pos; SetIosb(iosb, STATUS_SUCCESS, 8); return STATUS_SUCCESS;
    }
    if (infoClass == 34 && len >= 56) {                // FileNetworkOpenInformation
        memset(info, 0, 56);
        LARGE_INTEGER sz; GetFileSizeEx(f->win, &sz);
        *(int64_t*)((char*)info + 0x28) = sz.QuadPart; // AllocationSize
        *(int64_t*)((char*)info + 0x30) = sz.QuadPart; // EndOfFile
        *(uint32_t*)((char*)info + 0x38) = FILE_ATTRIBUTE_NORMAL;
        SetIosb(iosb, STATUS_SUCCESS, 56); return STATUS_SUCCESS;
    }
    SetIosb(iosb, STATUS_SUCCESS, 0); return STATUS_SUCCESS;
}

extern "C" int32_t __stdcall Hy_NtSetInformationFile(uint32_t handle, void* iosb, void* info,
        uint32_t len, uint32_t infoClass) {
    FileObj* f = Resolve(handle);
    if (!f) { SetIosb(iosb, STATUS_NO_SUCH_FILE, 0); return STATUS_NO_SUCH_FILE; }
    if (infoClass == 14 && len >= 8) f->pos = *(int64_t*)info; // FilePositionInformation
    SetIosb(iosb, STATUS_SUCCESS, 0); return STATUS_SUCCESS;
}

extern "C" int32_t __stdcall Hy_IoCreateSymbolicLink(void* a, void* b) { (void)a; (void)b; return STATUS_SUCCESS; }

} // namespace tj::hybrid
