//=============================================================================
//
//  强制弹出优盘 (Force Eject USB)  v1.0
//
//  专用于强制弹出 / 卸载 U 盘、移动硬盘等可移动磁盘的原创小工具。
//  弹出流程借鉴了 Windows 公开的设备管理 API 行为。
//
//  用法:
//      强制弹出优盘.exe              托盘模式(左键选设备弹出, 右键: 全部弹出/退出)
//      强制弹出优盘.exe E:           强制弹出盘符 E: 所在的整块设备(U盘/移动硬盘/光驱)
//      强制弹出优盘.exe all          弹出全部 USB / 可移动设备 (含 USB 光驱)
//      强制弹出优盘.exe -l           列出所有设备 (标明类型: U盘/移动硬盘/光驱...)
//      强制弹出优盘.exe -f E:        对内置设备(内置光驱/硬盘)也执行弹出(系统盘除外)
//      强制弹出优盘.exe -h           帮助
//
//  强制策略(按顺序):
//      1. FlushFileBuffers + FSCTL_LOCK_VOLUME + FSCTL_DISMOUNT_VOLUME
//      2. 锁定失败 -> 用 NtQuerySystemInformation(SystemExtendedHandleInformation)
//         枚举全系统句柄, 通过 DuplicateHandle(DUPLICATE_CLOSE_SOURCE) 强制
//         关闭其它进程中引用目标卷的文件/卷句柄, 然后重试
//      3. CM_Request_Device_EjectW 弹出整块设备(被占用否决时继续关句柄重试)
//      4. 兜底 IOCTL_STORAGE_EJECT_MEDIA
//
//  安全保护: 永远拒绝弹出 Windows 系统卷所在的物理磁盘, 以及本程序自己
//            所在的磁盘。
//
//  日志: 与 exe 同目录的 <exe名>.log (UTF-8, 带时间戳)
//
//=============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WINVER       0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <initguid.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef CM_GET_DEVICE_INTERFACE_LIST_PRESENT
#define CM_GET_DEVICE_INTERFACE_LIST_PRESENT 0x00000000
#endif

//=============================================================================
// GUIDs (own names to avoid header conflicts)
//=============================================================================

DEFINE_GUID(GUID_DEVCLASS_DISKDRIVE_X, 0x4d36e967, 0xe325, 0x11ce, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18);
DEFINE_GUID(GUID_DEVCLASS_CDROM_X,     0x4d36e965, 0xe325, 0x11ce, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18);
DEFINE_GUID(GUID_DEVINTERFACE_DISK_X,  0x53f56307, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b);
DEFINE_GUID(GUID_DEVINTERFACE_CDROM_X, 0x53f56308, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b);

//=============================================================================
// Constants / globals
//=============================================================================

#define APP_NAME            L"强制弹出优盘"
#define TRAY_MUTEX_NAME     L"ForceEjectUSB_Tray_Mutex"
#define WINDOW_CLASS_NAME   L"ForceEjectUSBWnd"

#define WMAPP_TRAY          (WM_APP + 1)

#define IDM_FIRST_DEVICE    1000
#define IDM_EJECT_ALL       200
#define IDM_EXIT            201

#define MAX_DISKS   64
#define MAX_VOLUMES 128
#define MAX_VOLS_PER_DISK 32

static HINSTANCE g_hInst = NULL;
static HWND      g_hWnd  = NULL;
static UINT      g_msgTaskbarCreated = 0;
static NOTIFYICONDATAW g_Nid;
static WCHAR     g_szLogPath[MAX_PATH] = L"";
static HANDLE    g_hConsole = NULL;

//=============================================================================
// Small utilities
//=============================================================================

static BOOL StartsWithI(LPCWSTR s, LPCWSTR prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (towlower((wint_t)s[i]) != towlower((wint_t)prefix[i]))
            return FALSE;
        i++;
    }
    return TRUE;
}

static void StripTrailingBackslash(LPWSTR s)
{
    size_t n = lstrlenW(s);
    if (n > 0 && s[n - 1] == L'\\')
        s[n - 1] = 0;
}

//-----------------------------------------------------------------------------
// Logging — UTF-8 lines with timestamp, appended next to the exe.
//-----------------------------------------------------------------------------

static void LogF(LPCWSTR fmt, ...)
{
    if (!g_szLogPath[0])
        return;

    WCHAR line[1600];
    va_list args;
    va_start(args, fmt);
    StringCchVPrintfW(line, _countof(line), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR stamp[64];
    StringCchPrintfW(stamp, _countof(stamp),
                     L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    WCHAR full[2048];
    StringCchCopyW(full, _countof(full), stamp);
    StringCchCatW(full, _countof(full), line);

    HANDLE h = CreateFileW(g_szLogPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    // write a UTF-8 BOM when the file is created
    char bom[3];
    DWORD rd = 0, wr = 0;
    BOOL hasBom = FALSE;
    HANDLE h2 = CreateFileW(g_szLogPath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (h2 != INVALID_HANDLE_VALUE) {
        if (ReadFile(h2, bom, 3, &rd, NULL) && rd == 3 &&
            (BYTE)bom[0] == 0xEF && (BYTE)bom[1] == 0xBB && (BYTE)bom[2] == 0xBF)
            hasBom = TRUE;
        CloseHandle(h2);
    }
    if (!hasBom) {
        static const char BOM[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
        WriteFile(h, BOM, 3, &wr, NULL);
    }

    char out[8192];
    int cb = WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)sizeof(out) - 2, NULL, NULL);
    if (cb > 0) {
        out[cb - 1] = '\r'; out[cb] = '\n';   // replace NUL with CRLF
        WriteFile(h, out, (DWORD)(cb + 1), &wr, NULL);
    }
    CloseHandle(h);
}

//-----------------------------------------------------------------------------
// Console output (command-line mode only; silently skipped otherwise)
//-----------------------------------------------------------------------------

static void ConF(LPCWSTR fmt, ...)
{
    if (!g_hConsole)
        return;
    WCHAR line[1600];
    va_list args;
    va_start(args, fmt);
    StringCchVPrintfW(line, _countof(line), fmt, args);
    va_end(args);
    DWORD wr = 0;
    WriteConsoleW(g_hConsole, line, (DWORD)lstrlenW(line), &wr, NULL);
    WriteConsoleW(g_hConsole, L"\r\n", 2, &wr, NULL);
    LogF(L"[out] %s", line);
}

//=============================================================================
// Volume / disk information
//=============================================================================

struct VOLUME_INFO
{
    WCHAR guidOpen[64];    // \\?\Volume{...}          (no trailing backslash, for CreateFile)
    WCHAR guidFull[64];    // \\?\Volume{...}\         (for mount point APIs)
    DWORD deviceNumber;    // physical disk / cdrom number, (DWORD)-1 when unknown
    DWORD deviceType;      // STORAGE_DEVICE_NUMBER.DeviceType of the owning stack
    WCHAR letter[8];       // "E:" or ""
    WCHAR ntName[64];      // \Device\HarddiskVolumeN or ""
};

#define DEV_KIND_DISK   0
#define DEV_KIND_CDROM  1

struct DEVICE_INFO
{
    DEVINST devInst;
    int     kind;           // DEV_KIND_DISK / DEV_KIND_CDROM
    DWORD   deviceNumber;   // (DWORD)-1 when unknown
    DWORD   storageDevType; // STORAGE_DEVICE_NUMBER.DeviceType
    BOOL    removable;      // devnode has CM_DEVCAP_REMOVABLE
    BOOL    usbBus;         // BusTypeUsb or USB enumerator
    BYTE    busType;        // STORAGE_BUS_TYPE from IOCTL_STORAGE_QUERY_PROPERTY, 0xFF unknown
    BOOL    removableMedia; // device reports removable media (typical for U 盘)
    BOOL    protectedDisk;  // system disk / disk the exe runs from
    WCHAR   friendly[220];
    WCHAR   busName[48];    // enumerator name fallback
    WCHAR   devId[220];
    WCHAR   letters[120];   // "E:,F:"
    int     volumeCount;
};

static BOOL GetVolumeDeviceNumber(LPCWSTR pszGuidNoSlash, DWORD *pdwNumber, DWORD *pdwType)
{
    HANDLE h = CreateFileW(pszGuidNoSlash, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LogF(L"  [diag] 打开失败 %s err=%lu", pszGuidNoSlash, GetLastError());
        return FALSE;
    }
    STORAGE_DEVICE_NUMBER sdn = { 0 };
    DWORD br = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                              NULL, 0, &sdn, sizeof(sdn), &br, NULL);
    if (!ok)
        LogF(L"  [diag] IOCTL 失败 %s err=%lu", pszGuidNoSlash, GetLastError());
    else {
        if (pdwNumber)
            *pdwNumber = sdn.DeviceNumber;
        if (pdwType)
            *pdwType = sdn.DeviceType;
    }
    CloseHandle(h);
    return ok;
}

static int EnumVolumes(VOLUME_INFO *list, int maxCount)
{
    int n = 0;
    WCHAR vol[64];
    HANDLE hFind = FindFirstVolumeW(vol, _countof(vol));
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;
    do {
        StripTrailingBackslash(vol);
        if (lstrlenW(vol) < 12)          // "\\?\Volume{" minimum sanity
            continue;
        if (n >= maxCount)
            break;
        VOLUME_INFO *v = &list[n];
        ZeroMemory(v, sizeof(*v));
        StringCchCopyW(v->guidOpen, _countof(v->guidOpen), vol);
        StringCchCopyW(v->guidFull, _countof(v->guidFull), vol);
        StringCchCatW(v->guidFull, _countof(v->guidFull), L"\\");
        v->deviceNumber = (DWORD)-1;

        DWORD num = 0, dvt = 0;
        if (GetVolumeDeviceNumber(v->guidOpen, &num, &dvt)) {
            v->deviceNumber = num;
            v->deviceType = dvt;
        }

        // mount points: pick a plain drive letter if one exists
        WCHAR names[2048] = L"";
        DWORD cch = _countof(names);
        if (GetVolumePathNamesForVolumeNameW(v->guidFull, names, cch, &cch)) {
            WCHAR *p = names;
            while (*p) {
                if (p[1] == L':' && (p[2] == 0 || p[2] == L'\\')) {
                    WCHAR up = (WCHAR)towupper((wint_t)p[0]);
                    if (up >= L'A' && up <= L'Z') {
                        v->letter[0] = up;
                        v->letter[1] = L':';
                        v->letter[2] = 0;
                        break;
                    }
                }
                p += lstrlenW(p) + 1;
            }
        }
        if (v->letter[0]) {
            WCHAR nt[256];
            if (QueryDosDeviceW(v->letter, nt, _countof(nt)))
                StringCchCopyW(v->ntName, _countof(v->ntName), nt);
        }
        n++;
    } while (FindNextVolumeW(hFind, vol, _countof(vol)));
    FindVolumeClose(hFind);
    LogF(L"  [diag] 卷枚举: 共 %d 个", n);
    return n;
}

static BOOL GetDeviceInterfacePath(LPCWSTR deviceId, const GUID *itfGuid, WCHAR *out, ULONG cchOut)
{
    ULONG len = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(
        &len, (LPGUID)itfGuid,
        (DEVINSTID_W)deviceId, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || len == 0)
        return FALSE;
    if (len > cchOut)
        len = cchOut;
    cr = CM_Get_Device_Interface_ListW(
        (LPGUID)itfGuid,
        (DEVINSTID_W)deviceId, out, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || out[0] == 0)
        return FALSE;
    return TRUE;
}

// Queries the device number and bus/removable-media identity through an
// already-open handle to the device interface.
static VOID QueryStorageIdentity(HANDLE h, DEVICE_INFO *d)
{
    STORAGE_DEVICE_NUMBER sdn = { 0 };
    DWORD br = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        NULL, 0, &sdn, sizeof(sdn), &br, NULL)) {
        d->deviceNumber = sdn.DeviceNumber;
        d->storageDevType = sdn.DeviceType;
    }
    STORAGE_PROPERTY_QUERY query;
    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE buf[2048];
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                        &query, sizeof(query), buf, sizeof(buf), &br, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        d->busType = (BYTE)desc->BusType;
        d->removableMedia = desc->RemovableMedia;
    }
}

static int EnumAllDevices(DEVICE_INFO *list, int maxCount)
{
    int n = 0;
    const struct { const GUID *cls; const GUID *itf; int kind; } classes[] = {
        { &GUID_DEVCLASS_DISKDRIVE_X, &GUID_DEVINTERFACE_DISK_X,  DEV_KIND_DISK  },
        { &GUID_DEVCLASS_CDROM_X,     &GUID_DEVINTERFACE_CDROM_X, DEV_KIND_CDROM },
    };
    for (int c = 0; c < (int)_countof(classes) && n < maxCount; c++) {
        HDEVINFO hInfo = SetupDiGetClassDevsW(classes[c].cls, NULL, NULL, DIGCF_PRESENT);
        if (hInfo == INVALID_HANDLE_VALUE)
            continue;
        for (DWORD i = 0; n < maxCount; i++) {
            SP_DEVINFO_DATA dd;
            dd.cbSize = sizeof(dd);
            if (!SetupDiEnumDeviceInfo(hInfo, i, &dd))
                break;
            DEVICE_INFO *d = &list[n];
            ZeroMemory(d, sizeof(*d));
            d->deviceNumber = (DWORD)-1;
            d->busType = 0xFF;
            d->devInst = dd.DevInst;
            d->kind = classes[c].kind;

            WCHAR buf[512] = L"";
            if (!SetupDiGetDeviceRegistryPropertyW(hInfo, &dd, SPDRP_FRIENDLYNAME, NULL,
                                                   (PBYTE)buf, sizeof(buf), NULL))
                SetupDiGetDeviceRegistryPropertyW(hInfo, &dd, SPDRP_DEVICEDESC, NULL,
                                                  (PBYTE)buf, sizeof(buf), NULL);
            StringCchCopyW(d->friendly, _countof(d->friendly), buf);

            if (SetupDiGetDeviceRegistryPropertyW(hInfo, &dd, SPDRP_ENUMERATOR_NAME, NULL,
                                                  (PBYTE)buf, sizeof(buf), NULL))
                StringCchCopyW(d->busName, _countof(d->busName), buf);

            SetupDiGetDeviceInstanceIdW(hInfo, &dd, d->devId, (DWORD)_countof(d->devId), NULL);

            DWORD status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, dd.DevInst, 0) == CR_SUCCESS)
                d->removable = (status & CM_DEVCAP_REMOVABLE) != 0;

            if (d->devId[0]) {
                WCHAR ifPath[1024];
                BOOL hasIf = GetDeviceInterfacePath(d->devId, classes[c].itf,
                                                    ifPath, _countof(ifPath));
                if (!hasIf)
                    LogF(L"  [diag] 接口路径获取失败: %s", d->devId);
                if (hasIf) {
                    HANDLE h = CreateFileW(ifPath, GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           NULL, OPEN_EXISTING, 0, NULL);
                    if (h == INVALID_HANDLE_VALUE) {
                        LogF(L"  [diag] 打开接口失败 err=%lu: %s", GetLastError(), ifPath);
                    } else {
                        QueryStorageIdentity(h, d);
                        CloseHandle(h);
                    }
                }
            }
            d->usbBus = (d->busType == 0x07 /*BusTypeUsb*/) || StartsWithI(d->busName, L"USB");

            LogF(L"  [diag] 设备[%s]: %s 编号=%u 类型=0x%X 总线=0x%02X 可移动介质=%d 可移动设备=%d",
                 d->kind == DEV_KIND_CDROM ? L"光驱" : L"磁盘",
                 d->friendly,
                 d->deviceNumber == (DWORD)-1 ? 0xFFFFFFFFu : d->deviceNumber,
                 d->storageDevType, d->busType,
                 (int)d->removableMedia, (int)d->removable);
            n++;
        }
        SetupDiDestroyDeviceInfoList(hInfo);
    }
    return n;
}

static void AttachVolumesToDevices(DEVICE_INFO *disks, int nDisks, VOLUME_INFO *vols, int nVols)
{
    for (int i = 0; i < nVols; i++) {
        if (vols[i].deviceNumber == (DWORD)-1)
            continue;
        for (int j = 0; j < nDisks; j++) {
            if (disks[j].deviceNumber == vols[i].deviceNumber &&
                disks[j].storageDevType == vols[i].deviceType) {
                disks[j].volumeCount++;
                if (vols[i].letter[0]) {
                    if (disks[j].letters[0])
                        StringCchCatW(disks[j].letters, _countof(disks[j].letters), L",");
                    StringCchCatW(disks[j].letters, _countof(disks[j].letters), vols[i].letter);
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Device type naming — what the user sees in menus and lists.
//-----------------------------------------------------------------------------

static LPCWSTR DeviceTypeName(const DEVICE_INFO *d)
{
    if (d->kind == DEV_KIND_CDROM) {
        if (d->busType == 0x0E || d->busType == 0x0F) // file-backed virtual
            return L"虚拟光驱";
        return d->usbBus ? L"USB 光驱" : L"内置光驱";
    }
    switch (d->busType) {
    case 0x07: // BusTypeUsb
        return d->removableMedia ? L"U 盘" : L"USB 移动硬盘";
    case 0x04: return L"1394 磁盘";          // FireWire
    case 0x0C: case 0x0D: return L"存储卡";  // SD / MMC
    case 0x0E: case 0x0F: return L"虚拟磁盘"; // virtual / VHD
    default:
        if (d->removable) return L"可移动磁盘";
        return L"内置硬盘";
    }
}

static LPCWSTR BusTypeName(const DEVICE_INFO *d)
{
    switch (d->busType) {
    case 0x01: return L"SCSI";
    case 0x02: return L"ATAPI";
    case 0x03: return L"ATA";
    case 0x04: return L"1394";
    case 0x06: return L"FC";
    case 0x07: return L"USB";
    case 0x08: return L"RAID";
    case 0x09: return L"iSCSI";
    case 0x0A: return L"SAS";
    case 0x0B: return L"SATA";
    case 0x0C: return L"SD";
    case 0x0D: return L"MMC";
    case 0x0E: return L"虚拟";
    case 0x0F: return L"VHD";
    case 0x10: return L"存储空间";
    case 0x11: return L"NVMe";
    }
    return d->busName[0] ? d->busName : L"?";
}

// Candidate for tray menu / "all" / no-flag command-line eject: never the
// system disk, never our own disk; USB or removable-devnode devices only.
static BOOL IsEjectableCandidate(const DEVICE_INFO *d)
{
    return !d->protectedDisk && (d->usbBus || d->removable);
}

//-----------------------------------------------------------------------------
// Protected disks: the system volume's disk and the disk this exe runs from.
//-----------------------------------------------------------------------------

static BOOL GetDeviceNumberOfRoot(LPCWSTR root, DWORD *pdwNumber)
{
    WCHAR vol[64] = L"";
    if (!GetVolumeNameForVolumeMountPointW(root, vol, _countof(vol)))
        return FALSE;
    StripTrailingBackslash(vol);
    return GetVolumeDeviceNumber(vol, pdwNumber, NULL);
}

static DWORD GetSystemDiskNumber(void)
{
    WCHAR winDir[MAX_PATH] = L"";
    GetWindowsDirectoryW(winDir, _countof(winDir));
    if (winDir[1] != L':')
        return (DWORD)-1;
    WCHAR root[8] = { winDir[0], L':', L'\\', 0 };
    DWORD n = (DWORD)-1;
    GetDeviceNumberOfRoot(root, &n);
    return n;
}

static DWORD GetOurDiskNumber(void)
{
    WCHAR exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, _countof(exe));
    if (exe[1] != L':')
        return (DWORD)-1;
    WCHAR root[8] = { exe[0], L':', L'\\', 0 };
    DWORD n = (DWORD)-1;
    GetDeviceNumberOfRoot(root, &n);
    return n;
}

static void MarkProtectedDisks(DEVICE_INFO *disks, int nDisks)
{
    DWORD sys = GetSystemDiskNumber();
    DWORD ours = GetOurDiskNumber();
    for (int i = 0; i < nDisks; i++) {
        // only disks can host the system volume; optical drives never qualify
        disks[i].protectedDisk =
            (disks[i].kind == DEV_KIND_DISK) &&
            (disks[i].deviceNumber != (DWORD)-1) &&
            (disks[i].deviceNumber == sys || disks[i].deviceNumber == ours);
    }
}

//=============================================================================
// "Force" core — close other processes' handles that reference target volumes
//=============================================================================

typedef LONG NTSTATUS_X;
typedef LONG (WINAPI *PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);

#pragma pack(push, 8)
typedef struct _HS_HANDLE_ENTRY {
    ULONG_PTR Object;
    ULONG_PTR ProcessId;
    ULONG_PTR Handle;
    ULONG     GrantedAccess;
    USHORT    CreatorBackTraceIndex;
    USHORT    ObjectTypeIndex;
    ULONG     HandleAttributes;
    ULONG     Reserved;
} HS_HANDLE_ENTRY;

typedef struct _HS_HANDLE_INFO_EX {
    ULONG_PTR       NumberOfHandles;
    ULONG           Reserved;      // header padding — Handles[] starts at offset 16
    HS_HANDLE_ENTRY Handles[1];
} HS_HANDLE_INFO_EX;
#pragma pack(pop)

// Closes every handle held by other processes that points into one of the
// given volumes.  Returns the number of closed handles (-1 on enumeration
// failure).
static int CloseForeignHandlesForVolumes(const VOLUME_INFO *vols, int nVols)
{
    PFN_NtQuerySystemInformation pNtQ = (PFN_NtQuerySystemInformation)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!pNtQ || nVols <= 0)
        return -1;

    ULONG cb = 1UL << 22;
    HS_HANDLE_INFO_EX *info = NULL;
    NTSTATUS_X st;
    for (;;) {
        info = (HS_HANDLE_INFO_EX *)HeapAlloc(GetProcessHeap(), 0, cb);
        if (!info)
            return -1;
        st = pNtQ(64 /* SystemExtendedHandleInformation */, info, cb, NULL);
        if (st == 0)
            break;
        HeapFree(GetProcessHeap(), 0, info);
        info = NULL;
        if (st != (NTSTATUS_X)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */)
            return -1;
        cb *= 2;
        if (cb > (1UL << 28))
            return -1;
    }

    DWORD myPid = GetCurrentProcessId();
    ULONG_PTR n = info->NumberOfHandles;
    HS_HANDLE_ENTRY *e = info->Handles;
    int closed = 0;
    int statOpened = 0, statDup = 0, statDiskType = 0, statPathOk = 0, statMatched = 0;

    for (ULONG_PTR i = 0; i < n; i++) {
        if (e[i].ProcessId == 0 || e[i].ProcessId == (ULONG_PTR)myPid)
            continue;
        HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, (DWORD)e[i].ProcessId);
        if (!hProc)
            continue;
        statOpened++;
        HANDLE hDup = NULL;
        // duplicate for inspection only, with just attribute-read rights
        if (DuplicateHandle(hProc, (HANDLE)e[i].Handle, GetCurrentProcess(), &hDup,
                            FILE_READ_ATTRIBUTES, FALSE, 0)) {
            statDup++;
            BOOL match = FALSE;
            if (GetFileType(hDup) == FILE_TYPE_DISK) {
                statDiskType++;
                WCHAR path[1100];
                if (GetFinalPathNameByHandleW(hDup, path, _countof(path),
                                              VOLUME_NAME_GUID | FILE_NAME_NORMALIZED)) {
                    statPathOk++;
                    for (int k = 0; k < nVols; k++) {
                        size_t m = lstrlenW(vols[k].guidOpen);
                        if (m > 0 && StartsWithI(path, vols[k].guidOpen)) {
                            WCHAR c = path[m];
                            if (c == 0 || c == L'\\') { match = TRUE; break; }
                        }
                    }
                }
                if (!match && vols[0].ntName[0]) {
                    WCHAR path2[1100];
                    if (GetFinalPathNameByHandleW(hDup, path2, _countof(path2),
                                                  VOLUME_NAME_NT | FILE_NAME_NORMALIZED)) {
                        for (int k = 0; k < nVols; k++) {
                            size_t m = lstrlenW(vols[k].ntName);
                            if (m > 0 && StartsWithI(path2, vols[k].ntName)) {
                                WCHAR c = path2[m];
                                if (c == 0 || c == L'\\') { match = TRUE; break; }
                            }
                        }
                    }
                }
            }
            CloseHandle(hDup);
            if (match) {
                statMatched++;
                if (DuplicateHandle(hProc, (HANDLE)e[i].Handle, NULL, NULL, 0, FALSE,
                                    DUPLICATE_CLOSE_SOURCE)) {
                    closed++;
                    WCHAR procName[300] = L"?";
                    DWORD sz = _countof(procName);
                    if (!QueryFullProcessImageNameW(hProc, 0, procName, &sz))
                        StringCchCopyW(procName, _countof(procName), L"?");
                    LPCWSTR base = wcsrchr(procName, L'\\');
                    base = base ? base + 1 : procName;
                    LogF(L"  [force] 已关闭句柄: 进程=%s pid=%u handle=0x%I64X",
                         base, (unsigned)e[i].ProcessId, (unsigned long long)e[i].Handle);
                } else {
                    LogF(L"  [scan] 关闭源句柄失败 err=%lu pid=%u", GetLastError(),
                         (unsigned)e[i].ProcessId);
                }
            }
        }
        CloseHandle(hProc);
    }

    LogF(L"  [scan] 句柄统计: 总数=%I64u 进程可开=%d 复制=%d 磁盘类型=%d 取路径成功=%d 命中=%d 关闭=%d",
         n, statOpened, statDup, statDiskType, statPathOk, statMatched, closed);
    HeapFree(GetProcessHeap(), 0, info);
    return closed;
}

//=============================================================================
// Eject
//=============================================================================

static LPCWSTR VetoText(int v)
{
    switch (v) {
    case 1:  return L"旧式设备，无法移除";
    case 2:  return L"旧式驱动占用设备";
    case 3:  return L"电源不足";
    case 4:  return L"设备不可禁用";
    case 6:  return L"设备正在使用中";
    case 7:  return L"设备上有未关闭的句柄";
    case 8:  return L"设备驱动拒绝弹出 (通常因为卷仍被占用)";
    case 11: return L"权限不足";
    case 13: return L"被 Windows 应用占用";
    case 14: return L"被 Windows 服务占用";
    case 15: return L"存在未完成的打开操作";
    default: return L"系统拒绝了弹出请求";
    }
}

static LPCWSTR CRText(CONFIGRET cr)
{
    switch (cr) {
    case CR_SUCCESS:        return L"成功";
    case CR_NO_SUCH_DEVNODE:return L"设备不存在";
    case CR_REMOVE_VETOED:  return L"移除被否决";
    case CR_ACCESS_DENIED:  return L"拒绝访问";
    default:                return L"失败";
    }
}

static void CloseVolumeHandles(HANDLE *locks, int n)
{
    for (int i = 0; i < n; i++) {
        if (locks[i]) {
            CloseHandle(locks[i]);
            locks[i] = NULL;
        }
    }
}

// TRUE when at least one volume of this disk is still mounted (i.e. the
// disk did not really disappear).
static BOOL VolumeStillMounted(DWORD deviceNumber)
{
    if (deviceNumber == (DWORD)-1)
        return FALSE;
    VOLUME_INFO vols[MAX_VOLUMES];
    int n = EnumVolumes(vols, _countof(vols));
    for (int i = 0; i < n; i++) {
        if (vols[i].deviceNumber == deviceNumber)
            return TRUE;
    }
    return FALSE;
}

static BOOL DeviceStillPresent(DEVINST devInst)
{
    DEVICE_INFO check[MAX_DISKS];
    int n = EnumAllDevices(check, _countof(check));
    for (int i = 0; i < n; i++) {
        if (check[i].devInst == devInst)
            return TRUE;
    }
    return FALSE;
}

// Force-ejects the whole physical disk.  Returns TRUE on success and fills
// msgOut with a user-facing result line.
static BOOL ForceEjectDevice(const DEVICE_INFO *disk, WCHAR *msgOut, size_t cchMsg)
{
    VOLUME_INFO allVols[MAX_VOLUMES];
    int nAll = EnumVolumes(allVols, _countof(allVols));
    VOLUME_INFO mine[MAX_VOLS_PER_DISK];
    int nMine = 0;
    for (int i = 0; i < nAll && nMine < MAX_VOLS_PER_DISK; i++) {
        if (disk->deviceNumber != (DWORD)-1 &&
            allVols[i].deviceNumber == disk->deviceNumber &&
            allVols[i].deviceType == disk->storageDevType)
            mine[nMine++] = allVols[i];
    }

    LogF(L"== 弹出%s %u 「%s」 [%s] 盘符[%s] 总线=%s 卷数=%d",
         disk->kind == DEV_KIND_CDROM ? L"光驱" : L"磁盘",
         disk->deviceNumber == (DWORD)-1 ? 0u : disk->deviceNumber,
         disk->friendly[0] ? disk->friendly : L"未知设备",
         DeviceTypeName(disk),
         disk->letters[0] ? disk->letters : L"无",
         BusTypeName(disk), nMine);

    // ---- step 1/2: lock + dismount, retrying with foreign handle closing ----
    HANDLE locks[MAX_VOLS_PER_DISK] = { 0 };
    BOOL allLocked = FALSE;
    for (int attempt = 1; attempt <= 3 && !allLocked; attempt++) {
        if (attempt > 1) {
            CloseVolumeHandles(locks, nMine);
            int closed = CloseForeignHandlesForVolumes(mine, nMine);
            LogF(L"  [force] 第 %d 次重试: 强制关闭了 %d 个占用句柄", attempt, closed);
            Sleep(300);
        }
        allLocked = TRUE;
        for (int k = 0; k < nMine; k++) {
            HANDLE h = CreateFileW(mine[k].guidOpen,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                LogF(L"  卷 %s 打开失败 err=%lu", mine[k].guidOpen, GetLastError());
                locks[k] = NULL;
                allLocked = FALSE;
                continue;
            }
            locks[k] = h;
            FlushFileBuffers(h);
            DWORD br = 0;
            if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL)) {
                LogF(L"  卷 %s 锁定失败 err=%lu (被占用)", mine[k].guidOpen, GetLastError());
                allLocked = FALSE;
            } else if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &br, NULL)) {
                LogF(L"  卷 %s 卸载失败 err=%lu", mine[k].guidOpen, GetLastError());
                allLocked = FALSE;
            } else {
                LogF(L"  卷 %s 已锁定并卸载", mine[k].guidOpen);
            }
        }
        CloseVolumeHandles(locks, nMine);
    }
    if (!allLocked && nMine > 0)
        LogF(L"  [force] 重试后仍有卷被占用, 继续尝试弹出");

    // ---- step 3: CM_Request_Device_EjectW with retries ----
    CONFIGRET cr = 0xFFFF;
    int veto = 0;
    WCHAR vetoName[256] = L"";
    for (int i = 0; i < 3; i++) {
        vetoName[0] = 0;
        PNP_VETO_TYPE vt = PNP_VetoTypeUnknown;
        cr = CM_Request_Device_EjectW(disk->devInst, &vt, vetoName,
                                      (ULONG)_countof(vetoName), 0);
        veto = (int)vt;
        LogF(L"  CM_Request_Device_Eject -> 0x%04X (%s) veto=%d name=%s",
             (unsigned)cr, CRText(cr), veto, vetoName[0] ? vetoName : L"-");
        if (cr == CR_SUCCESS)
            break;
        // veto types 6/7/15 = in use / open handles / outstanding opens
        if (veto == 6 || veto == 7 || veto == 15) {
            int closed = CloseForeignHandlesForVolumes(mine, nMine);
            LogF(L"  [force] 弹出被占用否决, 强制关闭了 %d 个句柄后重试", closed);
            Sleep(400);
        } else {
            break;
        }
    }

    BOOL ok = (cr == CR_SUCCESS);

    // ---- step 4: fallback to IOCTL_STORAGE_EJECT_MEDIA, then verify ----
    if (!ok) {
        WCHAR devPath[1024] = L"";
        if (disk->kind == DEV_KIND_DISK && disk->deviceNumber != (DWORD)-1) {
            StringCchPrintfW(devPath, _countof(devPath), L"\\\\.\\PhysicalDrive%u", disk->deviceNumber);
        } else if (disk->kind == DEV_KIND_CDROM && disk->devId[0]) {
            GetDeviceInterfacePath(disk->devId, &GUID_DEVINTERFACE_CDROM_X,
                                   devPath, _countof(devPath));
        }
        if (devPath[0]) {
            HANDLE h = CreateFileW(devPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD br = 0;
                if (DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &br, NULL)) {
                    Sleep(400);
                    if (!DeviceStillPresent(disk->devInst) &&
                        !VolumeStillMounted(disk->deviceNumber)) {
                        ok = TRUE;
                        LogF(L"  IOCTL_STORAGE_EJECT_MEDIA 成功 (设备已消失)");
                    } else {
                        LogF(L"  IOCTL_STORAGE_EJECT_MEDIA 被接受但设备仍在, 判定为失败");
                    }
                } else {
                    LogF(L"  IOCTL_STORAGE_EJECT_MEDIA 失败 err=%lu", GetLastError());
                }
                CloseHandle(h);
            }
        }
    }

    // ---- step 5: last resort — direct removal.  Only when every volume has
    // been dismounted cleanly and the veto was usage/driver related.  This is
    // what the original HotSwap! applet did (SetupDiRemoveDevice). ----
    if (!ok && allLocked && (veto == 6 || veto == 7 || veto == 8 || veto == 15)) {
        const GUID *classes[2] = { &GUID_DEVCLASS_DISKDRIVE_X, &GUID_DEVCLASS_CDROM_X };
        for (int c = 0; c < 2; c++) {
            HDEVINFO hInfo = SetupDiGetClassDevsW(classes[c], NULL, NULL, DIGCF_PRESENT);
            if (hInfo == INVALID_HANDLE_VALUE)
                continue;
            for (DWORD i = 0; ; i++) {
                SP_DEVINFO_DATA dd;
                dd.cbSize = sizeof(dd);
                if (!SetupDiEnumDeviceInfo(hInfo, i, &dd))
                    break;
                if (dd.DevInst == disk->devInst) {
                    BOOL removed = SetupDiRemoveDevice(hInfo, &dd);
                    LogF(L"  SetupDiRemoveDevice -> %d err=%lu", removed, GetLastError());
                    break;
                }
            }
            SetupDiDestroyDeviceInfoList(hInfo);
        }
        Sleep(600);
        if (!DeviceStillPresent(disk->devInst)) {
            ok = TRUE;
            if (disk->kind == DEV_KIND_DISK && VolumeStillMounted(disk->deviceNumber))
                LogF(L"  设备节点已移除 (卷对象尚未释放, 稍后自动消失)");
            else
                LogF(L"  设备已被强制移除");
        }
    }

    if (ok) {
        StringCchPrintfW(msgOut, cchMsg, L"已弹出 [%s] %s (%s)",
                         DeviceTypeName(disk),
                         disk->friendly[0] ? disk->friendly : L"未知设备",
                         disk->letters[0] ? disk->letters : L"无盘符");
        LogF(L"== 弹出成功: %s %u", disk->kind == DEV_KIND_CDROM ? L"光驱" : L"磁盘",
             disk->deviceNumber == (DWORD)-1 ? 0u : disk->deviceNumber);
    } else {
        LPCWSTR reason = VetoText(veto);
        StringCchPrintfW(msgOut, cchMsg, L"弹出失败 [%s] %s — %s",
                         DeviceTypeName(disk),
                         disk->friendly[0] ? disk->friendly : L"未知设备",
                         reason);
        LogF(L"== 弹出失败: 磁盘 %u cr=0x%04X veto=%d",
             disk->deviceNumber == (DWORD)-1 ? 0u : disk->deviceNumber,
             (unsigned)cr, veto);
    }
    return ok;
}

//=============================================================================
// Command line actions
//=============================================================================

static int ListAllDisks(void)
{
    DEVICE_INFO disks[MAX_DISKS];
    VOLUME_INFO vols[MAX_VOLUMES];
    int nD = EnumAllDevices(disks, _countof(disks));
    int nV = EnumVolumes(vols, _countof(vols));
    AttachVolumesToDevices(disks, nD, vols, nV);
    MarkProtectedDisks(disks, nD);

    ConF(L"--- 设备列表 (%d 个) ---", nD);
    for (int i = 0; i < nD; i++) {
        LPCWSTR mark = disks[i].protectedDisk ? L"[系统盘/本程序所在盘, 禁止弹出]"
                        : IsEjectableCandidate(&disks[i])
                          ? L"[可弹出]" : L"[内置设备, 需 -f 才可弹出]";
        WCHAR numStr[16];
        if (disks[i].deviceNumber == (DWORD)-1)
            StringCchCopyW(numStr, _countof(numStr), L"?");
        else
            StringCchPrintfW(numStr, _countof(numStr), L"%u", disks[i].deviceNumber);
        ConF(L"%s %s  [%s]  总线:%s  %s",
             disks[i].kind == DEV_KIND_CDROM ? L"光驱" : L"磁盘", numStr,
             DeviceTypeName(&disks[i]), BusTypeName(&disks[i]),
             disks[i].friendly[0] ? disks[i].friendly : L"未知设备");
        ConF(L"    盘符: %s    卷数: %d    %s",
             disks[i].letters[0] ? disks[i].letters : L"无",
             disks[i].volumeCount, mark);
        ConF(L"    设备实例: %s", disks[i].devId[0] ? disks[i].devId : L"?");
    }
    return 0;
}

static int EjectAllCandidates(void)
{
    DEVICE_INFO disks[MAX_DISKS];
    VOLUME_INFO vols[MAX_VOLUMES];
    int nD = EnumAllDevices(disks, _countof(disks));
    int nV = EnumVolumes(vols, _countof(vols));
    AttachVolumesToDevices(disks, nD, vols, nV);
    MarkProtectedDisks(disks, nD);

    int candidates = 0, okCount = 0;
    for (int i = 0; i < nD; i++) {
        if (!IsEjectableCandidate(&disks[i]))
            continue;
        candidates++;
        WCHAR msg[400] = L"";
        if (ForceEjectDevice(&disks[i], msg, _countof(msg))) {
            okCount++;
            ConF(L"[OK]   %s", msg);
        } else {
            ConF(L"[FAIL] %s", msg);
        }
    }
    if (candidates == 0) {
        ConF(L"没有发现可弹出的 USB / 可移动设备 (U 盘、移动硬盘、光驱等)。");
        return 3;
    }
    ConF(L"完成: %d/%d 个设备弹出成功。", okCount, candidates);
    return (okCount == candidates) ? 0 : 2;
}

static int EjectByTarget(LPCWSTR arg, BOOL bAllowAny)
{
    // normalise
    WCHAR letter[8] = L"";
    DWORD targetDeviceNumber = (DWORD)-1;
    WCHAR targetGuid[64] = L"";

    if (lstrlenW(arg) == 1) {
        letter[0] = (WCHAR)towupper((wint_t)arg[0]);
        letter[1] = L':';
        letter[2] = 0;
    } else if (lstrlenW(arg) >= 2 && arg[1] == L':') {
        letter[0] = (WCHAR)towupper((wint_t)arg[0]);
        letter[1] = L':';
        letter[2] = 0;
    } else if (StartsWithI(arg, L"\\\\?\\Volume{")) {
        StringCchCopyW(targetGuid, _countof(targetGuid), arg);
        StripTrailingBackslash(targetGuid);
    } else if (StartsWithI(arg, L"\\\\.\\PhysicalDrive")) {
        targetDeviceNumber = (DWORD)wcstoul(arg + 15, NULL, 10);
    } else {
        ConF(L"无法识别的参数: %s", arg);
        return 1;
    }

    // find the volume
    VOLUME_INFO vols[MAX_VOLUMES];
    int nV = EnumVolumes(vols, _countof(vols));
    const VOLUME_INFO *vol = NULL;
    for (int i = 0; i < nV; i++) {
        if (letter[0] && vols[i].letter[0] && lstrcmpiW(vols[i].letter, letter) == 0) {
            vol = &vols[i];
            break;
        }
        if (targetGuid[0] && lstrcmpiW(vols[i].guidOpen, targetGuid) == 0) {
            vol = &vols[i];
            break;
        }
        if (targetDeviceNumber != (DWORD)-1 && vols[i].deviceNumber == targetDeviceNumber) {
            vol = &vols[i];
            break;
        }
    }
    if (!vol || vol->deviceNumber == (DWORD)-1) {
        ConF(L"找不到 %s 对应的卷 (磁盘编号无法确定)。", arg);
        return 3;
    }

    // find the device that owns this volume (matching storage stack type)
    DEVICE_INFO disks[MAX_DISKS];
    int nD = EnumAllDevices(disks, _countof(disks));
    AttachVolumesToDevices(disks, nD, vols, nV);
    MarkProtectedDisks(disks, nD);
    DEVICE_INFO *disk = NULL;
    for (int i = 0; i < nD; i++) {
        if (disks[i].deviceNumber == vol->deviceNumber &&
            disks[i].storageDevType == vol->deviceType) {
            disk = &disks[i];
            break;
        }
    }
    if (!disk) {
        ConF(L"找不到 %s 对应的设备 (无法弹出)。", arg);
        return 3;
    }
    if (disk->protectedDisk) {
        ConF(L"拒绝弹出: 磁盘 %u 是系统盘或本程序所在的盘。", disk->deviceNumber);
        LogF(L"拒绝弹出受保护磁盘 %u", disk->deviceNumber);
        return 2;
    }
    if (!IsEjectableCandidate(disk) && !bAllowAny) {
        ConF(L"%s %u 是内置设备 (%s)。如确实需要弹出/卸载, 请加 -f 参数。",
             disk->kind == DEV_KIND_CDROM ? L"光驱" : L"磁盘", disk->deviceNumber,
             DeviceTypeName(disk));
        return 2;
    }

    WCHAR msg[400] = L"";
    BOOL ok = ForceEjectDevice(disk, msg, _countof(msg));
    ConF(L"%s%s", ok ? L"[OK]   " : L"[FAIL] ", msg);
    return ok ? 0 : 2;
}

static void PrintUsage(void)
{
    ConF(APP_NAME L" v1.1 — 强制弹出 / 卸载 U 盘、移动硬盘、光驱等设备的小工具");
    ConF(L"");
    ConF(L"用法:");
    ConF(L"  强制弹出优盘.exe            托盘模式 (左键: 选设备弹出; 右键: 全部弹出/退出)");
    ConF(L"  强制弹出优盘.exe E:         强制弹出盘符 E: 所在的整块设备");
    ConF(L"  强制弹出优盘.exe all        弹出全部 USB / 可移动设备 (含 USB 光驱)");
    ConF(L"  强制弹出优盘.exe -l         列出所有设备 (标明类型: U盘/移动硬盘/光驱...)");
    ConF(L"  强制弹出优盘.exe -f E:      允许对内置设备 (内置光驱/硬盘) 弹出 (系统盘除外)");
    ConF(L"  强制弹出优盘.exe -h         显示本帮助");
    ConF(L"");
    ConF(L"说明: 强制模式会关闭其它进程中占用目标设备的句柄, 请先保存文件。");
    ConF(L"      支持类型: U 盘 / USB 移动硬盘 / USB 光驱 / 内置光驱 / 存储卡等。");
    ConF(L"日志: 与 exe 同目录的 .log 文件。");
}

//=============================================================================
// Tray UI
//=============================================================================

static void TrayBalloon(LPCWSTR text, BOOL warn)
{
    NOTIFYICONDATAW nid = g_Nid;
    nid.uFlags = NIF_INFO;
    StringCchCopyW(nid.szInfoTitle, _countof(nid.szInfoTitle), APP_NAME);
    StringCchCopyW(nid.szInfo, _countof(nid.szInfo), text);
    nid.dwInfoFlags = warn ? NIIF_WARNING : NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayAddIcon(HWND hwnd)
{
    ZeroMemory(&g_Nid, sizeof(g_Nid));
    g_Nid.cbSize = sizeof(g_Nid);
    g_Nid.hWnd = hwnd;
    g_Nid.uID = 1;
    g_Nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_Nid.uCallbackMessage = WMAPP_TRAY;
    g_Nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
    StringCchCopyW(g_Nid.szTip, _countof(g_Nid.szTip),
                   APP_NAME L" — 左键: 选择弹出  右键: 更多操作");
    // explorer may not be ready right after logon — retry a few times
    for (int i = 0; i < 3; i++) {
        if (Shell_NotifyIconW(NIM_ADD, &g_Nid))
            return;
        LogF(L"托盘图标添加失败 (第 %d 次) err=%lu", i + 1, GetLastError());
        Sleep(1000);
    }
}

static int TrayEjectDevice(const DEVICE_INFO *disk)
{
    WCHAR msg[400] = L"";
    BOOL ok = ForceEjectDevice(disk, msg, _countof(msg));
    TrayBalloon(msg, !ok);
    return ok ? 0 : 1;
}

static void ShowDeviceMenu(HWND hwnd)
{
    static DEVICE_INFO s_disks[MAX_DISKS];
    VOLUME_INFO vols[MAX_VOLUMES];
    int nD = EnumAllDevices(s_disks, _countof(s_disks));
    int nV = EnumVolumes(vols, _countof(vols));
    AttachVolumesToDevices(s_disks, nD, vols, nV);
    MarkProtectedDisks(s_disks, nD);

    HMENU hMenu = CreatePopupMenu();
    int candidates = 0;
    for (int i = 0; i < nD; i++) {
        if (!IsEjectableCandidate(&s_disks[i]))
            continue;
        WCHAR label[400];
        if (s_disks[i].letters[0])
            StringCchPrintfW(label, _countof(label), L"弹出  [%s]  %s  (%s)",
                             DeviceTypeName(&s_disks[i]),
                             s_disks[i].friendly[0] ? s_disks[i].friendly : L"未知设备",
                             s_disks[i].letters);
        else
            StringCchPrintfW(label, _countof(label), L"弹出  [%s]  %s  （无盘符）",
                             DeviceTypeName(&s_disks[i]),
                             s_disks[i].friendly[0] ? s_disks[i].friendly : L"未知设备");
        AppendMenuW(hMenu, MF_STRING, IDM_FIRST_DEVICE + i, label);
        candidates++;
    }
    if (candidates == 0)
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"未发现可弹出的设备 (U 盘/移动硬盘/光驱)");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(hMenu,
                              TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_BOTTOMALIGN,
                              pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    if (cmd >= IDM_FIRST_DEVICE && cmd < IDM_FIRST_DEVICE + nD) {
        // refresh the entry freshly in case it changed since the menu opened
        TrayEjectDevice(&s_disks[cmd - IDM_FIRST_DEVICE]);
    }
}

static void ShowOpsMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_EJECT_ALL, L"弹出全部设备 (U盘/移动硬盘/光驱)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(hMenu,
                              TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_BOTTOMALIGN,
                              pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    if (cmd == IDM_EJECT_ALL) {
        DEVICE_INFO disks[MAX_DISKS];
        VOLUME_INFO vols[MAX_VOLUMES];
        int nD = EnumAllDevices(disks, _countof(disks));
        int nV = EnumVolumes(vols, _countof(vols));
        AttachVolumesToDevices(disks, nD, vols, nV);
        MarkProtectedDisks(disks, nD);
        int candidates = 0, okCount = 0;
        for (int i = 0; i < nD; i++) {
            if (!IsEjectableCandidate(&disks[i]))
                continue;
            candidates++;
            WCHAR msg[400] = L"";
            if (ForceEjectDevice(&disks[i], msg, _countof(msg)))
                okCount++;
        }
        if (candidates == 0) {
            TrayBalloon(L"没有发现可弹出的设备 (U 盘/移动硬盘/光驱)。", TRUE);
        } else {
            WCHAR text[300];
            StringCchPrintfW(text, _countof(text), L"完成: %d/%d 个设备弹出成功。",
                             okCount, candidates);
            TrayBalloon(text, okCount != candidates);
        }
    } else if (cmd == IDM_EXIT) {
        DestroyWindow(hwnd);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        TrayAddIcon(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        TrayAddIcon(hwnd);
        return 0;
    case WMAPP_TRAY:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)
            ShowDeviceMenu(hwnd);
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
            ShowOpsMenu(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_Nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//=============================================================================
// Entry point
//=============================================================================

static int CliMain(int argc, LPWSTR *argv)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS))
        g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    LogF(L"===== 命令行模式, %d 个参数 =====", argc - 1);

    BOOL bAllowAny = FALSE;
    BOOL bDid = FALSE;
    int exitCode = 0;

    for (int i = 1; i < argc; i++) {
        LPCWSTR a = argv[i];
        if (!a[0])
            continue;
        if (lstrcmpiW(a, L"-f") == 0 || lstrcmpiW(a, L"/f") == 0) {
            bAllowAny = TRUE;
            LogF(L"选项: -f (允许非 USB / 内置磁盘)");
            continue;
        }
        if (lstrcmpiW(a, L"-h") == 0 || lstrcmpiW(a, L"/h") == 0 ||
            lstrcmpiW(a, L"-?") == 0 || lstrcmpiW(a, L"/?") == 0 ||
            lstrcmpiW(a, L"help") == 0) {
            PrintUsage();
            bDid = TRUE;
            continue;
        }
        if (lstrcmpiW(a, L"-l") == 0 || lstrcmpiW(a, L"/l") == 0 ||
            lstrcmpiW(a, L"list") == 0) {
            exitCode = ListAllDisks();
            bDid = TRUE;
            continue;
        }
        if (lstrcmpiW(a, L"all") == 0 || lstrcmpiW(a, L"-a") == 0 ||
            lstrcmpiW(a, L"/a") == 0) {
            exitCode = EjectAllCandidates();
            bDid = TRUE;
            continue;
        }
        // anything else: drive letter / volume GUID / PhysicalDrive
        int rc = EjectByTarget(a, bAllowAny);
        if (rc > exitCode)
            exitCode = rc;
        bDid = TRUE;
    }

    if (!bDid) {
        PrintUsage();
        exitCode = 1;
    }
    LogF(L"===== 命令行模式结束, 退出码 %d =====", exitCode);
    return exitCode;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInstance;

    GetModuleFileNameW(NULL, g_szLogPath, _countof(g_szLogPath));
    WCHAR *dot = wcsrchr(g_szLogPath, L'.');
    if (dot)
        *dot = 0;
    StringCchCatW(g_szLogPath, _countof(g_szLogPath), L".log");

    int nArgs = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (nArgs > 1) {
        int rc = CliMain(nArgs, argv);
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);

    // ---- tray mode ----
    HANDLE hMutex = CreateMutexW(NULL, TRUE, TRAY_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogF(L"托盘模式: 已有实例在运行, 退出");
        CloseHandle(hMutex);
        return 0;
    }
    LogF(L"===== 托盘模式启动 =====");

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = WINDOW_CLASS_NAME;
    if (!RegisterClassW(&wc)) {
        LogF(L"RegisterClass 失败 err=%lu", GetLastError());
        return 1;
    }
    g_hWnd = CreateWindowExW(0, WINDOW_CLASS_NAME, APP_NAME, WS_OVERLAPPED,
                             0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) {
        LogF(L"CreateWindow 失败 err=%lu", GetLastError());
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogF(L"===== 托盘模式退出 =====");
    CloseHandle(hMutex);
    return 0;
}
