/*
 * kernel_file.c - Xbox File I/O
 *
 * Implements the Nt*File kernel functions. All Xbox device paths are
 * translated through kernel_path.c before use.
 *
 * The Xbox kernel uses NT-style file I/O with ANSI strings in
 * OBJECT_ATTRIBUTES (unlike Windows NT, which uses Unicode).
 *
 * Two backends:
 *   _WIN32  -> Win32 CreateFileW / ReadFile / FindFirstFileW ...
 *   POSIX   -> open / read / write / stat / opendir ...
 * The Xbox semantics (disposition mapping, IO_STATUS_BLOCK, info classes)
 * are identical on both; only the host syscalls differ.
 */

#define _GNU_SOURCE   /* FNM_CASEFOLD */
#include "kernel.h"
#include "../recomp_switch.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stddef.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fnmatch.h>
#endif

#define XBOX_BYTES_PER_SECTOR       512u
#define XBOX_SECTORS_PER_CLUSTER    32u      /* 512 * 32 = 16384 */

/* ── HOW BIG IS THE VOLUME THE TITLE JUST ASKED ABOUT? ───────────────────
 *
 * The geometry above is right and load-bearing. The CAPACITY beside it was
 * not: both platforms answered every FileFsSizeInformation query from the
 * HOST volume -- fstatvfs on POSIX, GetDiskFreeSpaceExW(NULL) on Win32,
 * which is the current directory's disk and not even the handle's. Measured
 * on this machine 19 Sep 2026, the title asking how much room its cache
 * drive has was told 1,858 GB total and 690 GB free, where a retail Xbox
 * cache partition is 750 MB. Nine hundred times over.
 *
 * Retail 8 GB drive, which is what a 2002 title was built against:
 *
 *   Partition0   the whole device
 *   Partition1   E:  ~4.8 GB   game and title data (TDATA/UDATA)
 *   Partition2   C:   500 MB   system
 *   Partition3   X:   750 MB   cache      <- all three are the same size,
 *   Partition4   Y:   750 MB   cache         and all three land in one
 *   Partition5   Z:   750 MB   cache         Cache/ directory here
 *
 * The volume is decided by where the handle's host path sits relative to the
 * save root, because that is what the path layer's own rules key on. A path
 * under <save>/Cache is a cache partition; anything else under <save> is
 * Partition1; anything else at all is the disc, which is read-only and whose
 * free space is zero on a console.
 *
 * FREE SPACE IS MEASURED, NOT ASSUMED. Reporting a fixed 750 MB free would
 * tell a title with a full cache that it may write another 750 MB. So the
 * bytes actually in the directory are walked and subtracted. The walk is
 * cached for a second: ordinal 218 is called a few hundred times a run
 * (221 and 353 in two runs read today) and the cache holds 258 files, so an
 * uncached walk would be tens of thousands of stat calls for a number that
 * cannot move meaningfully between two calls in the same frame.
 *
 * DEFAULT ON since 19 Sep 2026, and the promotion is a measurement rather
 * than an argument. The prediction was that a title told it has 690 GB might
 * cache differently from one told 750 MB, so a cold-boot pair was run from
 * the stock tree (empty cache) with the switch off and on:
 *
 *   off   1,410 file opens at boot
 *   on    1,413 file opens at boot
 *   cache built, both arms: 258 files, 119 MB, and byte-identical to the
 *   player's own cache except JSRF_TEXS0/1.JTX -- the two graffiti sheets
 *   that differ between ANY two builds, switch or no switch.
 *
 * So it changes nothing the title does, which is exactly why it can be on:
 * the OFF value is not a conservative choice, it is a wrong answer (no Xbox
 * ever reported 690 GB free), and the one consequence anybody predicted for
 * fixing it has been measured and is absent.
 *
 * NOT covered by that pair, and worth saying: save-game writes, and whether
 * a long session caches differently once the disc cache is warm. RECOMP_HDD_SIZES=0
 * restores the host-volume answer and is the control for any of that.
 */
#if defined(_WIN32)
#define SEP '\\'
#else
#define SEP '/'
#endif
#define XBOX_CACHE_PARTITION_BYTES   (750ull * 1024ull * 1024ull)
#define XBOX_DATA_PARTITION_BYTES   (4787ull * 1024ull * 1024ull)

static int hdd_sizes_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on_default("RECOMP_HDD_SIZES", 1);
    return on;
}

/* Bytes used below `root`, following directories, not following symlinks.
 * Best effort: an unreadable subtree contributes nothing rather than
 * aborting the answer. */
static unsigned long long dir_bytes_used(const char *root)
{
    unsigned long long total = 0;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char child[MAX_PATH];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(child, sizeof child, "%s\\%s", root, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += dir_bytes_used(child);
        else
            total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(root);
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char child[MAX_PATH];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(child, sizeof child, "%s/%s", root, e->d_name) >= (int)sizeof child)
            continue;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) total += dir_bytes_used(child);
        else if (S_ISREG(st.st_mode)) total += (unsigned long long)st.st_size;
    }
    closedir(d);
#endif
    return total;
}

/* Which emulated volume does this host path belong to, and how big is it?
 * Returns 0 if the switch is off or the volume cannot be decided, in which
 * case the caller keeps the host-volume answer. */
static int xbox_volume_capacity(const char *host_path,
                                unsigned long long *out_total,
                                unsigned long long *out_avail)
{
    const char *save = xbox_path_save_root();
    char cache_root[MAX_PATH];
    size_t save_len;
    unsigned long long used;

    if (!hdd_sizes_on() || !out_total || !out_avail)
        return 0;
    if (!save || !host_path)
        return 0;
    save_len = strlen(save);
    if (strncmp(host_path, save, save_len) != 0) {
        /* Not on the emulated disk at all: the disc. Read-only, and a console
         * reports no free space on it. Total is the DVD-9 the game shipped on. */
        *out_total = 6800ull * 1024ull * 1024ull;
        *out_avail = 0;
        return 1;
    }
    if (snprintf(cache_root, sizeof cache_root, "%s%cCache", save, SEP)
            >= (int)sizeof cache_root)
        return 0;

    if (!strncmp(host_path, cache_root, strlen(cache_root))) {
        static unsigned long long cached_used;
        static time_t cached_at;
        time_t now = time(NULL);
        if (now != cached_at) { cached_used = dir_bytes_used(cache_root); cached_at = now; }
        used = cached_used;
        *out_total = XBOX_CACHE_PARTITION_BYTES;
    } else {
        /* THE DATA PARTITION IS NOT THE WHOLE SAVE ROOT, and walking the root
         * was a defect that reached the player within the hour: the tree also
         * holds Partition0-5.img (5,000 MB of them) and Cache/ (a DIFFERENT
         * volume), so `used` came out at 5,120 MB against a 4,787 MB
         * partition, available clamped to zero, and JSRF put up "Insufficient
         * memory. To create a new save game, 5 more free blocks are required."
         * before the title screen. The real content is ~1 MB.
         *
         * Only the directories that actually live on Partition1 are counted.
         * Anything else in the tree belongs to another volume or is not a
         * filesystem at all. */
        static const char *const data_dirs[] = {
            "TDATA", "UDATA", "TitleData", "UserData", "SystemData"
        };
        static unsigned long long cached_used;
        static time_t cached_at;
        time_t now = time(NULL);
        if (now != cached_at) {
            size_t i;
            unsigned long long sum = 0;
            for (i = 0; i < sizeof data_dirs / sizeof data_dirs[0]; i++) {
                char sub[MAX_PATH];
                if (snprintf(sub, sizeof sub, "%s%c%s", save, SEP, data_dirs[i])
                        < (int)sizeof sub)
                    sum += dir_bytes_used(sub);
            }
            cached_used = sum; cached_at = now;
        }
        used = cached_used;
        *out_total = XBOX_DATA_PARTITION_BYTES;
    }
    *out_avail = (used >= *out_total) ? 0 : (*out_total - used);
    return 1;
}

static void close_dir_context(HANDLE handle);

/* Get the ANSI path from OBJECT_ATTRIBUTES (platform-independent). */
static const char* get_xbox_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    if (!ObjectAttributes || !ObjectAttributes->ObjectName ||
        !ObjectAttributes->ObjectName->Buffer)
        return NULL;
    return ObjectAttributes->ObjectName->Buffer;
}

/* Partition0 is the Xbox hard disk's fixed 512 KiB configuration area, not a
 * FATX filesystem path.  Keep a small process-local backing store for that
 * device instead of exposing a host raw disk or routing it through open(). */
#define PARTITION0_CONFIG_SIZE (512u * 1024u)
static unsigned char s_partition0_config[PARTITION0_CONFIG_SIZE];
static ULONGLONG s_partition0_position;
static unsigned char s_partition0_handle_tag;

/* Partition5 is the 750 MiB cache volume normally mounted as Z:.  JSRF opens
 * the device itself to format an empty cache, so keep sparse 4 KiB pages for
 * the raw view instead of allocating the whole partition or parsing FATX. */
#define PARTITION5_CACHE_SIZE (750ull * 1024ull * 1024ull)
#define PARTITION5_PAGE_SIZE  4096u
#define PARTITION5_PAGE_COUNT \
    ((size_t)(PARTITION5_CACHE_SIZE / PARTITION5_PAGE_SIZE))
static unsigned char** s_partition5_pages;
static ULONGLONG s_partition5_position;
static unsigned char s_partition5_handle_tag;

static HANDLE partition0_handle(void)
{
    return (HANDLE)&s_partition0_handle_tag;
}

static BOOL is_partition0_handle(HANDLE handle)
{
    return handle == partition0_handle();
}

static HANDLE partition5_handle(void)
{
    return (HANDLE)&s_partition5_handle_tag;
}

static BOOL is_partition5_handle(HANDLE handle)
{
    return handle == partition5_handle();
}

static BOOL path_equals_ci(const char* left, const char* right)
{
    if (!left || !right) return FALSE;
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return FALSE;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static BOOL is_partition0_path(const char* path)
{
    return path_equals_ci(path, "\\Device\\Harddisk0\\Partition0");
}

static BOOL is_partition5_path(const char* path)
{
    return path_equals_ci(path, "\\Device\\Harddisk0\\Partition5") ||
           path_equals_ci(path, "\\Device\\Harddisk0\\Partition5\\");
}

/* NtOpenFile is implemented in terms of NtCreateFile throughout the kernel
 * bridge.  Recognize device paths at that shared layer so direct NtCreateFile,
 * IoCreateFile, and the NtOpenFile wrapper all get identical semantics. */
static BOOL try_open_raw_partition(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition,
    ULONG CreateOptions, NTSTATUS* Status)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    BOOL partition0 = is_partition0_path(xbox_path);
    BOOL partition5 = is_partition5_path(xbox_path);

    if (!partition0 && !partition5)
        return FALSE;

    if (!FileHandle || CreateDisposition != XBOX_FILE_OPEN) {
        *Status = STATUS_INVALID_PARAMETER;
        if (IoStatusBlock) {
            IoStatusBlock->Status = *Status;
            IoStatusBlock->Information = 0;
        }
        return TRUE;
    }

    if (partition0) {
        *FileHandle = partition0_handle();
        s_partition0_position = 0;
    } else {
        *FileHandle = partition5_handle();
        s_partition5_position = 0;
    }
    *Status = STATUS_SUCCESS;
    if (IoStatusBlock) {
        IoStatusBlock->Status = *Status;
        IoStatusBlock->Information = 1;
    }
    fprintf(stderr,
            "%s NtCreateFile/open\n"
            "ObjectAttributes: %p root=%p attributes=0x%08X\n"
            "DesiredAccess: 0x%08X\n"
            "ShareAccess: 0x%08X\n"
            "CreateDisposition: 0x%08X\n"
            "CreateOptions: 0x%08X\n"
            "FileAttributes: 0x%08X\n"
            "Xbox path: %s\n"
            "Backing: %s\n"
            "Open status: 0x%08X\n",
            partition0 ? "PARTITION0" : "PARTITION5",
            (void*)ObjectAttributes, ObjectAttributes->RootDirectory,
            ObjectAttributes->Attributes, DesiredAccess, ShareAccess,
            CreateDisposition, CreateOptions, FileAttributes, xbox_path,
            partition0 ? "synthetic 512 KiB config-area device" :
                         "synthetic sparse 750 MiB cache device",
            (uint32_t)*Status);
    return TRUE;
}

static NTSTATUS partition0_read(
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset, HANDLE Event)
{
    ULONGLONG offset = (ByteOffset && ByteOffset->QuadPart >= 0)
        ? (ULONGLONG)ByteOffset->QuadPart : s_partition0_position;
    ULONG transferred = 0;

    if (!IoStatusBlock || (!Buffer && Length != 0))
        return STATUS_INVALID_PARAMETER;
    if (offset < PARTITION0_CONFIG_SIZE) {
        ULONGLONG remaining = PARTITION0_CONFIG_SIZE - offset;
        transferred = Length < remaining ? Length : (ULONG)remaining;
        memcpy(Buffer, s_partition0_config + (size_t)offset, transferred);
    }
    s_partition0_position = offset + transferred;
    IoStatusBlock->Information = transferred;
    IoStatusBlock->Status = (transferred == 0 && Length != 0)
        ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    fprintf(stderr,
            "PARTITION0 NtReadFile: offset=0x%016llX length=0x%08X transferred=0x%08X status=0x%08X\n",
            (unsigned long long)offset, Length, transferred,
            (uint32_t)IoStatusBlock->Status);
    if (Event && NT_SUCCESS(IoStatusBlock->Status)) SetEvent(Event);
    return IoStatusBlock->Status;
}

static NTSTATUS partition0_write(
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset, HANDLE Event)
{
    ULONGLONG offset = (ByteOffset && ByteOffset->QuadPart >= 0)
        ? (ULONGLONG)ByteOffset->QuadPart : s_partition0_position;
    ULONG transferred = 0;

    if (!IoStatusBlock || (!Buffer && Length != 0))
        return STATUS_INVALID_PARAMETER;
    if (offset < PARTITION0_CONFIG_SIZE) {
        ULONGLONG remaining = PARTITION0_CONFIG_SIZE - offset;
        transferred = Length < remaining ? Length : (ULONG)remaining;
        memcpy(s_partition0_config + (size_t)offset, Buffer, transferred);
    }
    s_partition0_position = offset + transferred;
    IoStatusBlock->Information = transferred;
    IoStatusBlock->Status = transferred == Length
        ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    fprintf(stderr,
            "PARTITION0 NtWriteFile: offset=0x%016llX length=0x%08X transferred=0x%08X status=0x%08X\n",
            (unsigned long long)offset, Length, transferred,
            (uint32_t)IoStatusBlock->Status);
    if (Event && NT_SUCCESS(IoStatusBlock->Status)) SetEvent(Event);
    return IoStatusBlock->Status;
}

static NTSTATUS partition5_read(
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset, HANDLE Event)
{
    ULONGLONG offset = (ByteOffset && ByteOffset->QuadPart >= 0)
        ? (ULONGLONG)ByteOffset->QuadPart : s_partition5_position;
    ULONG transferred = 0;

    if (!IoStatusBlock || (!Buffer && Length != 0))
        return STATUS_INVALID_PARAMETER;
    if (offset < PARTITION5_CACHE_SIZE) {
        ULONGLONG remaining = PARTITION5_CACHE_SIZE - offset;
        transferred = Length < remaining ? Length : (ULONG)remaining;
        memset(Buffer, 0, transferred);
        for (ULONG done = 0; done < transferred;) {
            ULONGLONG current = offset + done;
            size_t page_index = (size_t)(current / PARTITION5_PAGE_SIZE);
            size_t page_offset = (size_t)(current % PARTITION5_PAGE_SIZE);
            ULONG chunk = PARTITION5_PAGE_SIZE - (ULONG)page_offset;
            if (chunk > transferred - done) chunk = transferred - done;
            if (s_partition5_pages && s_partition5_pages[page_index])
                memcpy((unsigned char*)Buffer + done,
                       s_partition5_pages[page_index] + page_offset, chunk);
            done += chunk;
        }
    }
    s_partition5_position = offset + transferred;
    IoStatusBlock->Information = transferred;
    IoStatusBlock->Status = (transferred == 0 && Length != 0)
        ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    fprintf(stderr,
            "PARTITION5 NtReadFile: offset=0x%016llX length=0x%08X transferred=0x%08X status=0x%08X\n",
            (unsigned long long)offset, Length, transferred,
            (uint32_t)IoStatusBlock->Status);
    if (Event && NT_SUCCESS(IoStatusBlock->Status)) SetEvent(Event);
    return IoStatusBlock->Status;
}

static NTSTATUS partition5_write(
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset, HANDLE Event)
{
    ULONGLONG offset = (ByteOffset && ByteOffset->QuadPart >= 0)
        ? (ULONGLONG)ByteOffset->QuadPart : s_partition5_position;
    ULONG transferred = 0;

    if (!IoStatusBlock || (!Buffer && Length != 0))
        return STATUS_INVALID_PARAMETER;
    if (offset < PARTITION5_CACHE_SIZE) {
        ULONGLONG remaining = PARTITION5_CACHE_SIZE - offset;
        ULONG target = Length < remaining ? Length : (ULONG)remaining;
        if (!s_partition5_pages)
            s_partition5_pages = (unsigned char**)calloc(
                PARTITION5_PAGE_COUNT, sizeof(*s_partition5_pages));
        if (!s_partition5_pages && target != 0) {
            IoStatusBlock->Information = 0;
            IoStatusBlock->Status = STATUS_NO_MEMORY;
            return STATUS_NO_MEMORY;
        }
        while (transferred < target) {
            ULONGLONG current = offset + transferred;
            size_t page_index = (size_t)(current / PARTITION5_PAGE_SIZE);
            size_t page_offset = (size_t)(current % PARTITION5_PAGE_SIZE);
            ULONG chunk = PARTITION5_PAGE_SIZE - (ULONG)page_offset;
            if (chunk > target - transferred) chunk = target - transferred;
            if (!s_partition5_pages[page_index]) {
                s_partition5_pages[page_index] =
                    (unsigned char*)calloc(1, PARTITION5_PAGE_SIZE);
                if (!s_partition5_pages[page_index])
                    break;
            }
            memcpy(s_partition5_pages[page_index] + page_offset,
                   (unsigned char*)Buffer + transferred, chunk);
            transferred += chunk;
        }
    }
    s_partition5_position = offset + transferred;
    IoStatusBlock->Information = transferred;
    IoStatusBlock->Status = transferred == Length
        ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    fprintf(stderr,
            "PARTITION5 NtWriteFile: offset=0x%016llX length=0x%08X transferred=0x%08X status=0x%08X\n",
            (unsigned long long)offset, Length, transferred,
            (uint32_t)IoStatusBlock->Status);
    if (Event && NT_SUCCESS(IoStatusBlock->Status)) SetEvent(Event);
    return IoStatusBlock->Status;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  Win32 backend  =================================== */
/* ======================================================================== */

/* Convert Xbox create disposition to Win32 */
static DWORD xbox_disposition_to_win32(ULONG Disposition)
{
    switch (Disposition) {
        case XBOX_FILE_SUPERSEDE:    return CREATE_ALWAYS;
        case XBOX_FILE_OPEN:         return OPEN_EXISTING;
        case XBOX_FILE_CREATE:       return CREATE_NEW;
        case XBOX_FILE_OPEN_IF:      return OPEN_ALWAYS;
        case XBOX_FILE_OVERWRITE:    return TRUNCATE_EXISTING;
        case XBOX_FILE_OVERWRITE_IF: return CREATE_ALWAYS;
        default:                     return OPEN_EXISTING;
    }
}

/* Convert Xbox access mask to Win32 */
static DWORD xbox_access_to_win32(ACCESS_MASK Access)
{
    DWORD result = 0;
    if (Access & XBOX_GENERIC_READ)           result |= GENERIC_READ;
    if (Access & XBOX_GENERIC_WRITE)          result |= GENERIC_WRITE;
    if (Access & XBOX_GENERIC_ALL)            result |= GENERIC_ALL;
    if (Access & XBOX_FILE_READ_DATA)         result |= FILE_READ_DATA;
    if (Access & XBOX_FILE_WRITE_DATA)        result |= FILE_WRITE_DATA;
    if (Access & XBOX_FILE_APPEND_DATA)       result |= FILE_APPEND_DATA;
    if (Access & XBOX_FILE_READ_ATTRIBUTES)   result |= FILE_READ_ATTRIBUTES;
    if (Access & XBOX_FILE_WRITE_ATTRIBUTES)  result |= FILE_WRITE_ATTRIBUTES;
    if (Access & XBOX_SYNCHRONIZE)            result |= SYNCHRONIZE;
    if (Access & XBOX_DELETE)                  result |= DELETE;
    if (result == 0 || result == SYNCHRONIZE)
        result |= GENERIC_READ;
    return result;
}

uint32_t g_xbox_last_file_error;

uint32_t xbox_LastFileError(void)
{
    return g_xbox_last_file_error;
}

/* Convert Xbox share access to Win32 */
static DWORD xbox_share_to_win32(ULONG Share)
{
    DWORD result = 0;
    if (Share & 0x01) result |= FILE_SHARE_READ;
    if (Share & 0x02) result |= FILE_SHARE_WRITE;
    if (Share & 0x04) result |= FILE_SHARE_DELETE;
    return result;
}

/* Translate an Xbox OBJECT_ATTRIBUTES path to a Win32 wide path */
static BOOL translate_obj_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
                               WCHAR* win_path, DWORD buf_size)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path)
        return FALSE;
    if (ObjectAttributes->RootDirectory && xbox_path[0] != '\\' &&
        !(xbox_path[0] && xbox_path[1] == ':')) {
        /* XDeleteSaveGame opens each child relative to its save directory. */
        DWORD used = GetFinalPathNameByHandleW(ObjectAttributes->RootDirectory,
            win_path, buf_size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!used || used >= buf_size || used + 1 >= buf_size) return FALSE;
        if (win_path[used - 1] != L'\\') win_path[used++] = L'\\';
        int count = MultiByteToWideChar(CP_ACP, 0, xbox_path, -1,
            win_path + used, (int)(buf_size - used));
        return count != 0;
    }
    return xbox_translate_path(xbox_path, win_path, buf_size);
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    WCHAR win_path[MAX_PATH];
    HANDLE h;
    DWORD flags_and_attrs = FILE_ATTRIBUTE_NORMAL;
    NTSTATUS device_status;
    (void)AllocationSize;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (try_open_raw_partition(FileHandle, DesiredAccess, ObjectAttributes,
            IoStatusBlock, FileAttributes, ShareAccess, CreateDisposition,
            CreateOptions, &device_status))
        return device_status;

    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* A partition device opened as a directory.
     *
     * The path layer maps \Device\Harddisk0\PartitionN to a PartitionN.img
     * backing file, but a title asking for free space opens the partition with
     * FILE_DIRECTORY_FILE | FILE_OPEN_FOR_FREE_SPACE_QUERY -- it wants the
     * volume, not the bytes. Opening a regular file as a directory fails, and
     * the title reads STATUS_OBJECT_PATH_NOT_FOUND as "no such volume".
     *
     * Half-Life 2's CRT probes partition0 this way during startup and treats
     * the failure as fatal. Redirecting to the directory that holds the image
     * gives a handle that is valid for exactly what the caller is going to do
     * with it, which is NtQueryVolumeInformationFile. */
    if ((CreateOptions & XBOX_FILE_DIRECTORY_FILE) &&
        GetFileAttributesW(win_path) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(win_path) & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR *slash = wcsrchr(win_path, L'\\');
        if (slash && slash != win_path) {
            *slash = 0;
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                     "NtCreateFile: directory open of a device image, "
                     "using its containing directory instead");
        }
    }

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            CreateDirectoryW(win_path, NULL);
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        if (CreateOptions & XBOX_FILE_NO_INTERMEDIATE_BUFFERING)
            flags_and_attrs |= FILE_FLAG_NO_BUFFERING;
        if (FileAttributes & XBOX_FILE_ATTRIBUTE_READONLY)
            flags_and_attrs |= FILE_ATTRIBUTE_READONLY;
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL,
            xbox_disposition_to_win32(CreateDisposition), flags_and_attrs, NULL);
    }

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        /* Kept for the caller's trace. An NTSTATUS says "it did not open";
         * only the Win32 error distinguishes a title probing for a file that
         * is genuinely absent from one it cannot open because this runtime
         * already holds it open with a share mode the second open forbids --
         * and those need opposite responses. */
        g_xbox_last_file_error = (uint32_t)err;
        /* A warning, not a compiled-out trace: a failed open is how a title
         * silently decides a volume or asset is missing, and in a Release
         * build that decision was invisible. */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtCreateFile FAILED: %S (err=%u)", win_path, err);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        switch (err) {
            case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
            case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_PATH_NOT_FOUND;
            case ERROR_ACCESS_DENIED:  return STATUS_ACCESS_DENIED;
            case ERROR_ALREADY_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
            default:                   return STATUS_UNSUCCESSFUL;
        }
    }

    *FileHandle = h;
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %S -> handle=%p", win_path, h);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_read = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    if (is_partition0_handle(FileHandle))
        return partition0_read(IoStatusBlock, Buffer, Length, ByteOffset, Event);
    if (is_partition5_handle(FileHandle))
        return partition5_read(IoStatusBlock, Buffer, Length, ByteOffset, Event);

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, &ov);
    } else {
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, NULL);
    }

    if (result || GetLastError() == ERROR_HANDLE_EOF) {
        IoStatusBlock->Information = bytes_read;
        if (bytes_read == 0 && Length > 0) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            return STATUS_END_OF_FILE;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_written = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    if (is_partition0_handle(FileHandle))
        return partition0_write(IoStatusBlock, Buffer, Length, ByteOffset, Event);
    if (is_partition5_handle(FileHandle))
        return partition5_write(IoStatusBlock, Buffer, Length, ByteOffset, Event);

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, &ov);
    } else {
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, NULL);
    }

    if (result) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = bytes_written;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
    IoStatusBlock->Information = 0;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    close_dir_context(Handle);
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (is_partition0_handle(Handle) || is_partition5_handle(Handle))
        return STATUS_SUCCESS;
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    WCHAR win_path[MAX_PATH];
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %S", win_path);
    if (DeleteFileW(win_path))    return STATUS_SUCCESS;
    if (RemoveDirectoryW(win_path)) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->AllocationSize.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->AllocationSize.QuadPart + 4095) & ~4095LL;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->NumberOfLinks = fi.nNumberOfLinks;
            info->DeletePending = FALSE;
            info->Directory = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            LARGE_INTEGER pos, zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(FileHandle, zero, &pos, FILE_CURRENT))
                return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize.QuadPart = (info->EndOfFile.QuadPart + 4095) & ~4095LL;
            info->FileAttributes = fi.dwFileAttributes;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (!SetFilePointerEx(FileHandle, info->CurrentByteOffset, NULL, FILE_BEGIN)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetFilePosition failed: offset=%lld err=%u",
                    (long long)info->CurrentByteOffset.QuadPart, GetLastError());
                return STATUS_UNSUCCESSFUL;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            LARGE_INTEGER cur, zero = {0};
            SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
            SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
            if (!SetEndOfFile(FileHandle)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetEndOfFile failed: size=%lld err=%u",
                    (long long)info->EndOfFile.QuadPart, GetLastError());
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
                return STATUS_UNSUCCESSFUL;
            }
            if (cur.QuadPart <= info->EndOfFile.QuadPart)
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {
            /* Reserve space for a file. Halo's save path calls this before
             * writing, and an unimplemented class here returned
             * STATUS_NOT_IMPLEMENTED, which the title turned into DOS error
             * 317 and reported as "couldn't open or create saved game file".
             *
             * Same payload shape as EndOfFile: one LARGE_INTEGER. Windows
             * FileAllocationInfo is the direct equivalent; if the filesystem
             * declines it, fall back to setting the size, since the caller
             * only needs the space to exist. */
            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            FILE_ALLOCATION_INFO fai;
            fai.AllocationSize = info->EndOfFile;
            if (!SetFileInformationByHandle(FileHandle, FileAllocationInfo,
                                            &fai, sizeof(fai))) {
                LARGE_INTEGER cur, zero = {0};
                SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
                SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
                SetEndOfFile(FileHandle);
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            FILE_DISPOSITION_INFO fdi;
            fdi.DeleteFile = info->DeleteFile;
            if (!SetFileInformationByHandle(FileHandle, FileDispositionInfo, &fdi, sizeof(fdi)))
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                         "SetFileDispositionInfo failed: err=%u", GetLastError());
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            FILETIME ct, at, wt;
            ct.dwLowDateTime = info->CreationTime.LowPart;
            ct.dwHighDateTime = info->CreationTime.HighPart;
            at.dwLowDateTime = info->LastAccessTime.LowPart;
            at.dwHighDateTime = info->LastAccessTime.HighPart;
            wt.dwLowDateTime = info->LastWriteTime.LowPart;
            wt.dwHighDateTime = info->LastWriteTime.HighPart;
            SetFileTime(FileHandle, &ct, &at, &wt);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

/* Xbox volume geometry.
 *
 * FATX uses 16 KB clusters: 512-byte sectors, 32 sectors per cluster. That is
 * not cosmetic. A title's CRT startup asks for FileFsSizeInformation and
 * multiplies SectorsPerAllocationUnit by BytesPerSector, then *requires* the
 * product to equal the cluster size it was built for. Half-Life 2 checks for
 * 0x4000 and returns STATUS_DEVICE_NOT_READY (0xC000014F) otherwise, which
 * aborts CRT init before main ever runs -- the process then exits cleanly,
 * which reads as a title that did nothing rather than one that failed.
 *
 * Reporting the host's PC-typical 4 KB cluster (512 x 8) fails that check. */

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)FileHandle; (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            ULARGE_INTEGER free_bytes, total_bytes, total_free;
            unsigned long long vtotal = 0, vavail = 0;
            /* The emulated volume's own capacity first, when RECOMP_HDD_SIZES
             * is on. The call below asks the CURRENT DIRECTORY's disk, which
             * is not even the handle's -- see xbox_volume_capacity. */
            if (xbox_volume_capacity(w32_handle_path(FileHandle), &vtotal, &vavail)) {
                ULONGLONG cs = (ULONGLONG)XBOX_BYTES_PER_SECTOR * XBOX_SECTORS_PER_CLUSTER;
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                info->TotalAllocationUnits.QuadPart = (LONGLONG)(vtotal / cs);
                info->AvailableAllocationUnits.QuadPart = (LONGLONG)(vavail / cs);
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
                return STATUS_SUCCESS;
            }
            if (GetDiskFreeSpaceExW(NULL, &free_bytes, &total_bytes, &total_free)) {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                info->TotalAllocationUnits.QuadPart = total_bytes.QuadPart / cs;
                info->AvailableAllocationUnits.QuadPart = free_bytes.QuadPart / cs;
            } else {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    FlushFileBuffers(FileHandle);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    WCHAR win_path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    if (!GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_UNSUCCESSFUL;
    }

    FileInformation->CreationTime.LowPart    = fad.ftCreationTime.dwLowDateTime;
    FileInformation->CreationTime.HighPart   = fad.ftCreationTime.dwHighDateTime;
    FileInformation->LastAccessTime.LowPart  = fad.ftLastAccessTime.dwLowDateTime;
    FileInformation->LastAccessTime.HighPart = fad.ftLastAccessTime.dwHighDateTime;
    FileInformation->LastWriteTime.LowPart   = fad.ftLastWriteTime.dwLowDateTime;
    FileInformation->LastWriteTime.HighPart  = fad.ftLastWriteTime.dwHighDateTime;
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    FileInformation->AllocationSize.QuadPart = (FileInformation->EndOfFile.QuadPart + 4095) & ~4095LL;
    FileInformation->FileAttributes = fad.dwFileAttributes;
    return STATUS_SUCCESS;
}

#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE file_handle;
    HANDLE find_handle;
    BOOL   first_done;
    WIN32_FIND_DATAW find_data;
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

static void close_dir_context(HANDLE handle)
{
    if (!handle || !s_dir_cs_init) return;
    EnterCriticalSection(&s_dir_cs);
    for (int i=0;i<MAX_DIR_CONTEXTS;++i) if (s_dir_contexts[i].file_handle==handle) {
        HANDLE find=s_dir_contexts[i].find_handle;
        if (find && find!=INVALID_HANDLE_VALUE) FindClose(find);
        memset(&s_dir_contexts[i],0,sizeof(s_dir_contexts[i]));
    }
    LeaveCriticalSection(&s_dir_cs);
}

static DIR_CONTEXT* find_or_create_dir_context(HANDLE FileHandle, BOOL create)
{
    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle && s_dir_contexts[i].find_handle != NULL) {
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    if (!create) { LeaveCriticalSection(&s_dir_cs); return NULL; }
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].find_handle == NULL) {
            s_dir_contexts[i].file_handle = FileHandle;
            s_dir_contexts[i].first_done = FALSE;
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    LeaveCriticalSection(&s_dir_cs);
    return NULL;
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    DIR_CONTEXT* ctx;
    PXBOX_FILE_DIRECTORY_INFORMATION entry;
    (void)Event; (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    IoStatusBlock->Information=0;
    if (FileInformationClass!=XboxFileDirectoryInformation)
        return IoStatusBlock->Status=STATUS_INVALID_INFO_CLASS;
    if (Length<offsetof(XBOX_FILE_DIRECTORY_INFORMATION,FileName))
        return IoStatusBlock->Status=STATUS_BUFFER_TOO_SMALL;

    ctx = find_or_create_dir_context(FileHandle, TRUE);
    if (!ctx)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (RestartScan || !ctx->first_done) {
        if (ctx->find_handle && ctx->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
        }
        WCHAR search_path[MAX_PATH];
        WCHAR dir_path[MAX_PATH];
        DWORD path_len = GetFinalPathNameByHandleW(FileHandle, dir_path, MAX_PATH,
                                                   FILE_NAME_NORMALIZED);
        if (path_len == 0 || path_len >= MAX_PATH) {
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        WCHAR* clean_path = dir_path;
        if (wcsncmp(clean_path, L"\\\\?\\", 4) == 0)
            clean_path += 4;
        if (FileName && FileName->Buffer) {
            WCHAR pattern_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, FileName->Buffer, FileName->Length,
                                pattern_wide, MAX_PATH);
            pattern_wide[FileName->Length] = L'\0';
            swprintf_s(search_path, MAX_PATH, L"%s\\%s", clean_path, pattern_wide);
        } else {
            swprintf_s(search_path, MAX_PATH, L"%s\\*", clean_path);
        }
        ctx->find_handle = FindFirstFileW(search_path, &ctx->find_data);
        if (ctx->find_handle == INVALID_HANDLE_VALUE) {
            ctx->find_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        ctx->first_done = TRUE;
    } else {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    /* FATX enumeration never exposes the host's dot directories. */
    while (!wcscmp(ctx->find_data.cFileName, L".") ||
           !wcscmp(ctx->find_data.cFileName, L"..")) {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);
    char filename_ansi[MAX_PATH];
    int name_len = WideCharToMultiByte(CP_ACP, 0, ctx->find_data.cFileName, -1,
                                       filename_ansi, MAX_PATH, NULL, NULL);
    if (name_len > 0) name_len--;

    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    entry->CreationTime.LowPart   = ctx->find_data.ftCreationTime.dwLowDateTime;
    entry->CreationTime.HighPart  = ctx->find_data.ftCreationTime.dwHighDateTime;
    entry->LastAccessTime.LowPart = ctx->find_data.ftLastAccessTime.dwLowDateTime;
    entry->LastAccessTime.HighPart = ctx->find_data.ftLastAccessTime.dwHighDateTime;
    entry->LastWriteTime.LowPart  = ctx->find_data.ftLastWriteTime.dwLowDateTime;
    entry->LastWriteTime.HighPart = ctx->find_data.ftLastWriteTime.dwHighDateTime;
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = ((LONGLONG)ctx->find_data.nFileSizeHigh << 32) | ctx->find_data.nFileSizeLow;
    entry->AllocationSize.QuadPart = (entry->EndOfFile.QuadPart + 4095) & ~4095LL;
    entry->FileAttributes = ctx->find_data.dwFileAttributes;
    entry->FileNameLength = name_len;
    {
        ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
        ULONG copied = (ULONG)name_len;
        if (copied > Length - header_size)
            copied = Length - header_size;
        if (copied)
            memcpy(entry->FileName, filename_ansi, copied);
        IoStatusBlock->Status = copied == (ULONG)name_len
                                   ? STATUS_SUCCESS
                                   : STATUS_BUFFER_OVERFLOW;
        IoStatusBlock->Information = header_size + copied;
    }
    return IoStatusBlock->Status;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  POSIX backend  =================================== */
/* ======================================================================== */

/* Convert Xbox access mask + disposition to POSIX open() flags. */
static int posix_open_flags(ACCESS_MASK access, ULONG disposition)
{
    int wantWrite = (access & (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL |
                               XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA)) != 0;
    int rw = wantWrite ? O_RDWR : O_RDONLY;
    int extra;

    switch (disposition) {
        case XBOX_FILE_SUPERSEDE:    extra = O_CREAT | O_TRUNC; break;
        case XBOX_FILE_OPEN:         extra = 0;                 break;
        case XBOX_FILE_CREATE:       extra = O_CREAT | O_EXCL;  break;
        case XBOX_FILE_OPEN_IF:      extra = O_CREAT;           break;
        case XBOX_FILE_OVERWRITE:    extra = O_TRUNC;           break;
        case XBOX_FILE_OVERWRITE_IF: extra = O_CREAT | O_TRUNC; break;
        default:                     extra = 0;                 break;
    }
    /* O_TRUNC / O_CREAT imply write intent */
    if ((extra & (O_TRUNC | O_CREAT)) && rw == O_RDONLY)
        rw = O_RDWR;
    if (access & XBOX_FILE_APPEND_DATA)
        extra |= O_APPEND;
    return rw | extra;
}

static void unix_to_filetime(time_t sec, long nsec, LARGE_INTEGER* out)
{
    /* 100-ns ticks since 1601-01-01 */
    ULONGLONG t = 116444736000000000ULL
                + (ULONGLONG)sec * 10000000ULL
                + (ULONGLONG)nsec / 100ULL;
    out->LowPart  = (DWORD)(t & 0xFFFFFFFFULL);
    out->HighPart = (LONG)(t >> 32);
}

static ULONG mode_to_xbox_attrs(mode_t m)
{
    ULONG a = 0;
    if (S_ISDIR(m))      a |= XBOX_FILE_ATTRIBUTE_DIRECTORY;
    if (!(m & S_IWUSR))  a |= XBOX_FILE_ATTRIBUTE_READONLY;
    if (a == 0)          a = XBOX_FILE_ATTRIBUTE_NORMAL;
    return a;
}

static NTSTATUS errno_to_status(int e)
{
    switch (e) {
        case ENOENT:  return STATUS_OBJECT_NAME_NOT_FOUND;
        case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:   return STATUS_ACCESS_DENIED;
        case EEXIST:  return STATUS_OBJECT_NAME_COLLISION;
        case ENOMEM:  return STATUS_NO_MEMORY;
        default:      return STATUS_UNSUCCESSFUL;
    }
}

static int is_partition1_tdata_path(const char* path)
{
    return path &&
           strcasecmp(path, "\\Device\\Harddisk0\\Partition1\\TDATA") == 0;
}

/* Create the directories above a host path.
 *
 * A title populating its own cache writes files several levels below the cache
 * root -- JSRF caches through Z:\Media\Font\jetfont.dat~ -- without ever
 * creating the intermediate directories. On a console it does not have to: the
 * cache partition is a formatted volume it cleared itself, and it only creates
 * the handful of directories it enumerates at startup. Font is not among them.
 *
 * Here the cache root is an ordinary host directory, so that open failed with
 * ENOENT, JSRF retried once and then wrote Z:\Media\Cache\JSRF_FATAL.ERR.
 * That is also why the merged runtime opened two fewer files than the tree
 * before it: adding the Partition3/4/5 rules made the cache probe succeed, so
 * the title started using a cache the path layer could not actually serve.
 *
 * Only called for a disposition that creates, so a read of a missing file
 * still fails the way the title expects rather than leaving empty directories
 * behind on every cache miss.
 */
static void ensure_parent_dirs(const char* path)
{
    char   tmp[MAX_PATH];
    char*  last;
    size_t n;

    if (!path)
        return;
    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp))
        return;
    memcpy(tmp, path, n + 1);

    last = strrchr(tmp, '/');
    if (!last || last == tmp)
        return;             /* no parent, or the parent is the root */
    *last = '\0';

    /* Walk the prefixes, making each in turn. EEXIST is the common case. */
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    char host_path[MAX_PATH];
    NTSTATUS device_status;
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (try_open_raw_partition(FileHandle, DesiredAccess, ObjectAttributes,
            IoStatusBlock, FileAttributes, ShareAccess, CreateDisposition,
            CreateOptions, &device_status))
        return device_status;

    const char* xbox_path = get_xbox_path(ObjectAttributes);
    int trace_partition1 = is_partition1_tdata_path(xbox_path);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH)) {
        if (trace_partition1) {
            fprintf(stderr,
                    "PARTITION1 NtCreateFile\n"
                    "Xbox path: %s\n"
                    "Host path: <translation failed>\n"
                    "Create disposition: 0x%08X\n"
                    "Create options: 0x%08X\n"
                    "Result: 0x%08X\n",
                    xbox_path, CreateDisposition, CreateOptions,
                    (uint32_t)STATUS_OBJECT_PATH_NOT_FOUND);
        }
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (trace_partition1) {
        fprintf(stderr,
                "PARTITION1 NtCreateFile\n"
                "Xbox path: %s\n"
                "Host path: %s\n"
                "Create disposition: 0x%08X\n"
                "Create options: 0x%08X\n",
                xbox_path, host_path, CreateDisposition, CreateOptions);
    }
    int fd;
    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF) {
            ensure_parent_dirs(host_path);
            mkdir(host_path, 0755);   /* EEXIST is fine */
        }
        fd = open(host_path, O_RDONLY | O_DIRECTORY);
    } else {
        int flags = posix_open_flags(DesiredAccess, CreateDisposition);
        /* Keyed on O_CREAT rather than re-listing the dispositions, so this
         * cannot drift away from posix_open_flags. */
        if (flags & O_CREAT)
            ensure_parent_dirs(host_path);
        fd = open(host_path, flags, 0644);
    }

    if (fd < 0) {
        int e = errno;
        NTSTATUS status = errno_to_status(e);
        if (trace_partition1)
            fprintf(stderr, "Result: 0x%08X\n", (uint32_t)status);
        XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile FAILED: %s (errno=%d)", host_path, e);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        return status;
    }

    *FileHandle = w32_open_handle(fd, host_path);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %s -> handle=%p", host_path, *FileHandle);
    if (trace_partition1)
        fprintf(stderr, "Result: 0x%08X\n", (uint32_t)STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    if (is_partition0_handle(FileHandle))
        return partition0_read(IoStatusBlock, Buffer, Length, ByteOffset, Event);
    if (is_partition5_handle(FileHandle))
        return partition5_read(IoStatusBlock, Buffer, Length, ByteOffset, Event);

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = read(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Information = (ULONG_PTR)n;
    if (n == 0 && Length > 0) {
        IoStatusBlock->Status = STATUS_END_OF_FILE;
        return STATUS_END_OF_FILE;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    if (is_partition0_handle(FileHandle))
        return partition0_write(IoStatusBlock, Buffer, Length, ByteOffset, Event);
    if (is_partition5_handle(FileHandle))
        return partition5_write(IoStatusBlock, Buffer, Length, ByteOffset, Event);

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = write(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = (ULONG_PTR)n;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    close_dir_context(Handle);
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (is_partition0_handle(Handle) || is_partition5_handle(Handle))
        return STATUS_SUCCESS;
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    char host_path[MAX_PATH];
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %s", host_path);
    if (unlink(host_path) == 0) return STATUS_SUCCESS;
    if (rmdir(host_path)  == 0) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    struct stat st;
    if (FileInformationClass != XboxFilePositionInformation) {
        if (fstat(fd, &st) != 0)
            return STATUS_UNSUCCESSFUL;
    }

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->NumberOfLinks = (ULONG)st.st_nlink;
            info->DeletePending = FALSE;
            info->Directory = S_ISDIR(st.st_mode) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (pos < 0) return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset.QuadPart = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (lseek(fd, (off_t)info->CurrentByteOffset.QuadPart, SEEK_SET) < 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            if (ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            /* POSIX: unlinking an open file removes it on last close -- this
             * matches NT "delete on close" semantics exactly. */
            if (info->DeleteFile) {
                const char* p = w32_handle_path(FileHandle);
                if (p) unlink(p);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation:
            /* Setting file times is non-essential for the game; accept it. */
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            struct statvfs vfs;
            int fd = w32_handle_fd(FileHandle);
            unsigned long long vtotal = 0, vavail = 0;
            /* Xbox geometry, not the host's -- see the note on
             * XBOX_SECTORS_PER_CLUSTER above. */
            info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
            info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
            /* The emulated volume's own capacity, when RECOMP_HDD_SIZES is on
             * and the handle can be placed. See xbox_volume_capacity. */
            if (xbox_volume_capacity(w32_handle_path(FileHandle), &vtotal, &vavail)) {
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                info->TotalAllocationUnits.QuadPart = (LONGLONG)(vtotal / cs);
                info->AvailableAllocationUnits.QuadPart = (LONGLONG)(vavail / cs);
                IoStatusBlock->Status = STATUS_SUCCESS;
                IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
                return STATUS_SUCCESS;
            }
            if (fd >= 0 && fstatvfs(fd, &vfs) == 0) {
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                ULONGLONG total = (ULONGLONG)vfs.f_blocks * vfs.f_frsize;
                ULONGLONG avail = (ULONGLONG)vfs.f_bavail * vfs.f_frsize;
                info->TotalAllocationUnits.QuadPart = total / cs;
                info->AvailableAllocationUnits.QuadPart = avail / cs;
            } else {
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    int fd = w32_handle_fd(FileHandle);
    if (fd >= 0) fsync(fd);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    char host_path[MAX_PATH];
    struct stat st;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (stat(host_path, &st) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    unix_to_filetime(st.st_ctime, 0, &FileInformation->CreationTime);
    unix_to_filetime(st.st_atime, 0, &FileInformation->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &FileInformation->LastWriteTime);
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = st.st_size;
    FileInformation->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    FileInformation->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    return STATUS_SUCCESS;
}

/* Directory enumeration state, keyed by the directory's Nt handle. */
#define MAX_DIR_CONTEXTS 64
typedef struct {
    HANDLE handle;
    DIR*   dir;
    char   pattern[64];
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

static void close_dir_context(HANDLE handle)
{
    if (!handle || !s_dir_cs_init) return;
    EnterCriticalSection(&s_dir_cs);
    for (int i=0;i<MAX_DIR_CONTEXTS;++i) if (s_dir_contexts[i].handle==handle) {
        if (s_dir_contexts[i].dir) closedir(s_dir_contexts[i].dir);
        memset(&s_dir_contexts[i],0,sizeof(s_dir_contexts[i]));
    }
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    IoStatusBlock->Information=0;
    if (FileInformationClass!=XboxFileDirectoryInformation)
        return IoStatusBlock->Status=STATUS_INVALID_INFO_CLASS;
    if (Length<offsetof(XBOX_FILE_DIRECTORY_INFORMATION,FileName))
        return IoStatusBlock->Status=STATUS_BUFFER_TOO_SMALL;

    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);

    /* Locate or create the per-handle enumeration context. */
    DIR_CONTEXT* ctx = NULL;
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
        if (s_dir_contexts[i].handle == FileHandle) { ctx = &s_dir_contexts[i]; break; }
    if (!ctx) {
        for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
            if (s_dir_contexts[i].handle == NULL) { ctx = &s_dir_contexts[i]; break; }
        if (!ctx) { LeaveCriticalSection(&s_dir_cs); return STATUS_INSUFFICIENT_RESOURCES; }
        ctx->handle = FileHandle;
        ctx->dir = NULL;
    }

    if (RestartScan || ctx->dir == NULL) {
        if (ctx->dir) { closedir(ctx->dir); ctx->dir = NULL; }
        const char* dpath = w32_handle_path(FileHandle);
        if (!dpath) { LeaveCriticalSection(&s_dir_cs); return STATUS_UNSUCCESSFUL; }
        ctx->dir = opendir(dpath);
        if (!ctx->dir) {
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (FileName && FileName->Buffer && FileName->Length > 0) {
            USHORT n = FileName->Length;
            if (n >= sizeof(ctx->pattern)) n = sizeof(ctx->pattern) - 1;
            memcpy(ctx->pattern, FileName->Buffer, n);
            ctx->pattern[n] = '\0';
        } else {
            strcpy(ctx->pattern, "*");
        }
    }

    /* Advance to the next entry matching the search pattern. */
    struct dirent* de;
    const char* dpath = w32_handle_path(FileHandle);
    struct stat st;
    for (;;) {
        de = readdir(ctx->dir);
        if (!de) {
            closedir(ctx->dir);
            ctx->dir = NULL;
            ctx->handle = NULL;
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0)
            break;
    }

    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", dpath ? dpath : ".", de->d_name);
    if (stat(full, &st) != 0)
        memset(&st, 0, sizeof(st));

    /* readdir's name belongs to ctx->dir; keep it protected from NtClose
     * until all entry fields and the counted filename have been copied. */
    PXBOX_FILE_DIRECTORY_INFORMATION entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);

    int name_len = (int)strlen(de->d_name);
    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    unix_to_filetime(st.st_ctime, 0, &entry->CreationTime);
    unix_to_filetime(st.st_atime, 0, &entry->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &entry->LastWriteTime);
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = st.st_size;
    entry->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    entry->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    entry->FileNameLength = name_len;

    ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    ULONG copied=(ULONG)name_len;
    if (copied>Length-header_size) copied=Length-header_size;
    if (copied) memcpy(entry->FileName,de->d_name,copied);
    IoStatusBlock->Status=copied==(ULONG)name_len ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
    IoStatusBlock->Information=header_size+copied;
    LeaveCriticalSection(&s_dir_cs);
    return IoStatusBlock->Status;
}

#endif /* _WIN32 */

/* ======================================================================== */
/* ====================  Platform-independent  ============================ */
/* ======================================================================== */

NTSTATUS __stdcall xbox_NtOpenFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess, ULONG OpenOptions)
{
    /* NtOpenFile is NtCreateFile with FILE_OPEN disposition */
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, NULL, 0, ShareAccess, XBOX_FILE_OPEN, OpenOptions);
}

NTSTATUS __stdcall xbox_IoCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG Disposition, ULONG CreateOptions, ULONG Options)
{
    (void)Options;
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
        AllocationSize, FileAttributes, ShareAccess, Disposition, CreateOptions);
}

#define XBOX_IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define XBOX_IOCTL_DISK_GET_PARTITION_INFO 0x00074004u
#define XBOX_FSCTL_DISMOUNT_VOLUME         0x00090020u

typedef struct _XBOX_DISK_GEOMETRY {
    LARGE_INTEGER Cylinders;
    ULONG MediaType;
    ULONG TracksPerCylinder;
    ULONG SectorsPerTrack;
    ULONG BytesPerSector;
} XBOX_DISK_GEOMETRY;

typedef struct _XBOX_PARTITION_INFORMATION {
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER PartitionLength;
    ULONG HiddenSectors;
    ULONG PartitionNumber;
    UCHAR PartitionType;
    UCHAR BootIndicator;
    UCHAR RecognizedPartition;
    UCHAR RewritePartition;
} XBOX_PARTITION_INFORMATION;

_Static_assert(sizeof(XBOX_DISK_GEOMETRY) == 24,
               "Xbox DISK_GEOMETRY layout must be 24 bytes");
_Static_assert(sizeof(XBOX_PARTITION_INFORMATION) == 32,
               "Xbox PARTITION_INFORMATION layout must be 32 bytes");

NTSTATUS __stdcall xbox_NtFsControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    if (is_partition5_handle(FileHandle) &&
        FsControlCode == XBOX_FSCTL_DISMOUNT_VOLUME) {
        if (!IoStatusBlock)
            return STATUS_INVALID_PARAMETER;
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
        fprintf(stderr,
                "PARTITION5 NtFsControlFile: FSCTL_DISMOUNT_VOLUME status=0x%08X\n",
                (uint32_t)STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtFsControlFile(0x%X) - stub", FsControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtDeviceIoControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength;
    if (is_partition5_handle(FileHandle)) {
        ULONG information = 0;

        if (!IoStatusBlock)
            return STATUS_INVALID_PARAMETER;
        if (IoControlCode == XBOX_IOCTL_DISK_GET_DRIVE_GEOMETRY &&
            OutputBuffer && OutputBufferLength >= sizeof(XBOX_DISK_GEOMETRY)) {
            XBOX_DISK_GEOMETRY* geometry = (XBOX_DISK_GEOMETRY*)OutputBuffer;
            memset(geometry, 0, sizeof(*geometry));
            geometry->Cylinders.QuadPart = 3000;
            geometry->MediaType = 12; /* FixedMedia */
            geometry->TracksPerCylinder = 16;
            geometry->SectorsPerTrack = 32;
            geometry->BytesPerSector = 512;
            information = sizeof(*geometry);
        } else if (IoControlCode == XBOX_IOCTL_DISK_GET_PARTITION_INFO &&
                   OutputBuffer &&
                   OutputBufferLength >= sizeof(XBOX_PARTITION_INFORMATION)) {
            XBOX_PARTITION_INFORMATION* partition =
                (XBOX_PARTITION_INFORMATION*)OutputBuffer;
            memset(partition, 0, sizeof(*partition));
            partition->PartitionLength.QuadPart = PARTITION5_CACHE_SIZE;
            partition->PartitionNumber = 5;
            partition->RecognizedPartition = TRUE;
            information = sizeof(*partition);
        } else {
            IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
            IoStatusBlock->Information = 0;
            return STATUS_INVALID_PARAMETER;
        }

        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = information;
        fprintf(stderr,
                "PARTITION5 NtDeviceIoControlFile: code=0x%08X transferred=0x%08X status=0x%08X\n",
                IoControlCode, information, (uint32_t)STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
    /* Upstream's raw-disk queries also apply to ordinary partition-image
     * handles. Keep Partition5's sparse cache semantics above, and marshal
     * completion through the common bridge for every device. */
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    IoStatusBlock->Status = STATUS_NOT_SUPPORTED;
    if (IoControlCode == XBOX_IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        if (!OutputBuffer || OutputBufferLength < sizeof(XBOX_DISK_GEOMETRY))
            return IoStatusBlock->Status = ((NTSTATUS)0xC0000023u);
        XBOX_DISK_GEOMETRY *geometry = OutputBuffer;
        memset(geometry, 0, sizeof(*geometry));
        geometry->Cylinders.QuadPart = 1216;
        geometry->MediaType = 12; /* FixedMedia */
        geometry->TracksPerCylinder = 255;
        geometry->SectorsPerTrack = 63;
        geometry->BytesPerSector = 512;
        IoStatusBlock->Information = sizeof(*geometry);
        IoStatusBlock->Status = STATUS_SUCCESS;
    } else if (IoControlCode == XBOX_IOCTL_DISK_GET_PARTITION_INFO) {
        if (!OutputBuffer || OutputBufferLength < sizeof(XBOX_PARTITION_INFORMATION))
            return IoStatusBlock->Status = ((NTSTATUS)0xC0000023u);
        XBOX_PARTITION_INFORMATION *partition = OutputBuffer;
        LARGE_INTEGER size = {0};
#if defined(_WIN32)
        if (!GetFileSizeEx(FileHandle, &size))
            return IoStatusBlock->Status = STATUS_INVALID_HANDLE;
#else
        struct stat st;
        int fd = w32_handle_fd(FileHandle);
        if (fd < 0 || fstat(fd, &st) != 0)
            return IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        size.QuadPart = st.st_size;
#endif
        memset(partition, 0, sizeof(*partition));
        partition->PartitionLength = size;
        partition->PartitionNumber = 1;
        partition->PartitionType = 6;
        partition->BootIndicator = TRUE;
        partition->RecognizedPartition = TRUE;
        IoStatusBlock->Information = sizeof(*partition);
        IoStatusBlock->Status = STATUS_SUCCESS;
    }
    return IoStatusBlock->Status;
}

NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(
    PHANDLE LinkHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    /*
     * Xbox uses symbolic links for drive-letter mapping (D: -> \Device\CdRom0).
     * Path translation handles this transparently, so return a dummy handle.
     */
    if (LinkHandle)
        *LinkHandle = (HANDLE)(ULONG_PTR)0xDEAD0001;
    XBOX_TRACE(XBOX_LOG_FILE, "NtOpenSymbolicLinkObject(%s) - stub",
        get_xbox_path(ObjectAttributes) ? get_xbox_path(ObjectAttributes) : "?");
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(
    HANDLE LinkHandle, PXBOX_ANSI_STRING LinkTarget, PULONG ReturnedLength)
{
    (void)LinkHandle;
    const char* target = "\\Device\\CdRom0";
    if (LinkTarget && LinkTarget->Buffer) {
        USHORT len = (USHORT)strlen(target);
        if (len < LinkTarget->MaximumLength) {
            memcpy(LinkTarget->Buffer, target, len + 1);
            LinkTarget->Length = len;
        }
    }
    if (ReturnedLength)
        *ReturnedLength = (ULONG)strlen(target);
    return STATUS_SUCCESS;
}
