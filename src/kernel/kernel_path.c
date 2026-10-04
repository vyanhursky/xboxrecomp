/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* A project may set this to watch every guest path the kernel translates (for
 * example to print who opened a file). NULL by default. */
void (*g_xbox_path_hook)(const char *xbox_path) = NULL;

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    /* Partition 1 is the hard disk, and a title's own saved data lives on it
     * under TDATA -- the same place T: points at, and somewhere that has to be
     * writable. Routing it with the rest of partition 1 sent it to the game
     * directory, which is the disc: read-only, and carrying only the empty
     * TDATA\<titleid> stub every disc ships. Def Jam: Fight for NY opens
     * TDATA\45410049\$u\contentmeta.xbx there, gets
     * STATUS_OBJECT_PATH_NOT_FOUND, and could not have created it either.
     *
     * UDATA is where saved games live, so it is writable storage too: the
     * same place U: points at. It used to stay with the game directory, which
     * is the user's dump -- harmless while titles only read TitleMeta.xbx and
     * TitleImage.xbx there, and the first save a title made would have been
     * written into the dump. The dashboard copies a title's UDATA files from
     * the disc to the disk on first run; xbox_PathInit does the same. */
    { "\\Device\\Harddisk0\\Partition1\\TDATA\\", 1, "\\TitleData", "/TitleData" },
    { "\\Device\\Harddisk0\\Partition1\\UDATA\\", 1, "\\UserData",  "/UserData"  },
    { "\\Device\\Harddisk0\\Partition1\\UDATA",     1, "\\UserData",  "/UserData"  },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    /* The rest of the disk. Partition 0 is the whole raw device, 2 holds
     * system data, and 3-5 are the per-title caches behind X:, Y: and Z:.
     * Without these the path layer reported "Unrecognized Xbox path" and
     * NtOpenFile returned STATUS_OBJECT_PATH_NOT_FOUND; DSTEAL_JP probes
     * partition0 at startup and treats the failure as fatal.
     *
     * No trailing separator: the probe opens the device itself, with nothing
     * after it. Listed after Partition1 so that keeps its own mapping. */
    /* Partition2 is C:, the system partition. On a console that is where the
     * dashboard and its own assets live, and the dashboard opens them by
     * device path as well as through Y:, so a path *under* partition2 is an
     * ordinary asset read and belongs in the game dir -- the same place Y:
     * already goes. Only the bare device keeps the system-data image, which
     * is why this rule carries the trailing separator and is listed first. */
    { "\\Device\\Harddisk0\\Partition2\\",    0, NULL,         NULL          },
    { "C:\\",                                 0, NULL,         NULL          },
    { "\\??\\C:\\",                           0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition2",     1, "\\SystemData", "/SystemData" },
    { "\\Device\\Harddisk0\\Partition3",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition4",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition5",     1, "\\Cache",   "/Cache"      },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/*
 * Rewrite a path through a drive letter the title mapped for itself.
 *
 * Titles do their own mounting. Wreckless calls IoCreateSymbolicLink at guest
 * 0x000E9B1D to link "\??\Z:" to "\Device\Harddisk0\Partition1\", then loads
 * every asset through z:\. kernel_io.c has recorded that link since it was
 * written, but nothing ever read the table back, so the static rule below --
 * Z: is the cache partition, which is true of the console in general and wrong
 * for this title -- sent every asset open to the save directory and it failed
 * with ERROR_FILE_NOT_FOUND.
 *
 * Accepts "Z:\rest" and "\??\Z:\rest"; the table is keyed on the "\??\Z:" form
 * the kernel actually registers.
 *
 * Deliberately conservative: the rewritten path is returned only when it
 * matches a static rule. A link whose target this layer has no rule for would
 * otherwise turn a path that translated adequately into one that does not
 * translate at all.
 *
 * Returns 1 and fills `out` on success, 0 to leave the path alone.
 */
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    size_t      tlen, rlen;
    int         i;

    if (!xbox_path || !out || out_size == 0)
        return 0;

    if (match_prefix(xbox_path, "\\??\\") && xbox_path[4] && xbox_path[5] == ':')
        xbox_path += 4;
    if (!xbox_path[0] || xbox_path[1] != ':')
        return 0;
    rest = xbox_path + 2;

    link[0] = '\\';
    link[1] = '?';
    link[2] = '?';
    link[3] = '\\';
    link[4] = (char)toupper((unsigned char)xbox_path[0]);
    link[5] = ':';
    link[6] = '\0';

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_prefix(out, s_rules[i].prefix))
            return 1;           /* the target is somewhere we can place */
    }
    return 0;
}

/* ======================================================================== */
/* Shared by both backends, for the same reason as xbox_LastFileError: the
 * bridge calls it unconditionally, and a _WIN32-only definition breaks every
 * POSIX link. The Win32 backend fills this during translation; the POSIX one
 * does not yet, so it reads empty there. */
static XBOX_THREAD_LOCAL wchar_t s_last_host_path_shared[MAX_PATH];

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path_shared;
}

#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;

/*
 * The raw disk device, \Device\Harddisk0\Partition0.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_DISK_IMAGE_NAME   L"Partition0.img"
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

static void xbox_write_partition_table(const WCHAR *path)
{
    /* name[16], flags, lba_start, lba_size, reserved -- 32 bytes each */
    static const struct { const char *name; ULONG start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    unsigned char sector[512];
    HANDLE h;
    DWORD written;
    LARGE_INTEGER off;
    size_t i;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = sector + 48 + i * 32;   /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        *(ULONG *)(e + 16) = XBOX_PART_IN_USE;
        *(ULONG *)(e + 20) = parts[i].start;
        *(ULONG *)(e + 24) = parts[i].size;
        *(ULONG *)(e + 28) = 0;
    }

    off.QuadPart = XBOX_PART_TABLE_OFFSET;
    if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        WriteFile(h, sector, sizeof(sector), &written, NULL);
    CloseHandle(h);
}

/*
 * Map a partition device onto an image file.
 *
 * Matches "\Device\Harddisk0\PartitionN" with nothing below it, which is how a
 * device is opened. Anything with a path under it -- Partition1\TDATA and the
 * like -- is an ordinary filesystem access and falls through to the rules
 * table, which puts it in a directory.
 *
 * Partition0 is the whole disk and carries the partition table written above.
 * The rest start empty, which is what an unformatted partition looks like, so
 * a title that wants a filesystem there formats one.
 */
static BOOL xbox_partition_device_path(const char *xbox_path, WCHAR *out, DWORD n)
{
    static const char *prefix = "\\Device\\Harddisk0\\Partition";
    int len = match_prefix(xbox_path, prefix);
    int digit;
    const char *rest;

    if (!len)
        return FALSE;
    digit = xbox_path[len];
    if (digit < '0' || digit > '9')
        return FALSE;

    rest = xbox_path + len + 1;
    /* Nothing below it, allowing for a single trailing separator. */
    if (*rest == '\\' || *rest == '/')
        rest++;
    if (*rest != '\0')
        return FALSE;

    swprintf_s(out, n, L"%s\\Partition%c.img", s_save_dir, (WCHAR)digit);
    return TRUE;
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        /* Neutral default: the CWD's "game" subdirectory, which is the
         * layout the game projects use (see templates/new-game). */
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Absolute, because SHCreateDirectoryExW below only accepts a fully
     * qualified path: handed the relative "saves" a host passes, it fails with
     * ERROR_BAD_PATHNAME and creates nothing. The partition images then cannot
     * be created either, and a title that opens \Device\Harddisk0\Partition1\
     * at boot gets STATUS_OBJECT_PATH_NOT_FOUND and quits to the dashboard.
     * It only ever worked where someone had made the directory by hand. */
    {
        WCHAR full[MAX_PATH];
        if (GetFullPathNameW(s_save_dir, MAX_PATH, full, NULL))
            wcscpy_s(s_save_dir, MAX_PATH, full);
    }

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache",
                                       L"SystemData" };
        WCHAR image[MAX_PATH];
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
        /* The dashboard's first-run copy: the disc's UDATA\<title id>\* to
         * the save area's UserData, without replacing anything already there.
         * Two levels is all a disc carries (the title's directory and its
         * TitleMeta.xbx / TitleImage.xbx). */
        {
            WCHAR pat[MAX_PATH], sub[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
            WIN32_FIND_DATAW t, f;
            HANDLE ht, hf;
            swprintf_s(pat, MAX_PATH, L"%s\\UDATA\\*", s_game_dir);
            ht = FindFirstFileW(pat, &t);
            if (ht != INVALID_HANDLE_VALUE) {
                do {
                    if (!(t.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || t.cFileName[0] == L'.')
                        continue;
                    swprintf_s(dir, MAX_PATH, L"%s\\UserData\\%s", s_save_dir, t.cFileName);
                    SHCreateDirectoryExW(NULL, dir, NULL);
                    swprintf_s(sub, MAX_PATH, L"%s\\UDATA\\%s\\*", s_game_dir, t.cFileName);
                    hf = FindFirstFileW(sub, &f);
                    if (hf == INVALID_HANDLE_VALUE)
                        continue;
                    do {
                        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            continue;
                        swprintf_s(src, MAX_PATH, L"%s\\UDATA\\%s\\%s", s_game_dir, t.cFileName, f.cFileName);
                        swprintf_s(dst, MAX_PATH, L"%s\\%s", dir, f.cFileName);
                        CopyFileW(src, dst, TRUE);      /* never over a file already there */
                    } while (FindNextFileW(hf, &f));
                    FindClose(hf);
                } while (FindNextFileW(ht, &t));
                FindClose(ht);
            }
        }
        swprintf_s(image, MAX_PATH, L"%s\\%s", s_save_dir, XBOX_DISK_IMAGE_NAME);
        xbox_write_partition_table(image);
        /* The other partition devices, sized to the same geometry the table
         * above describes, so a title that asks the device how big it is gets
         * an answer consistent with the table it just read. Marked sparse
         * first: the cache partitions are 750 MB each and none of that is
         * touched until something writes to it. */
        {
            static const ULONGLONG part_sectors[6] = {
                0,             /* 0: whole disk, sized below      */
                0x00465400ull, /* 1: E: game and saves            */
                0x000FA000ull, /* 2: C: system                    */
                0x00177000ull, /* 3: X: cache                     */
                0x00177000ull, /* 4: Y: cache                     */
                0x00177000ull, /* 5: Z: cache                     */
            };
            for (int p = 1; p <= 5; p++) {
                HANDLE h;
                DWORD ret;
                LARGE_INTEGER end;

                swprintf_s(image, MAX_PATH, L"%s\\Partition%d.img",
                           s_save_dir, p);
                h = CreateFileW(image, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &ret, NULL);
                end.QuadPart = (LONGLONG)(part_sectors[p] * 512ull);
                if (SetFilePointerEx(h, end, NULL, FILE_BEGIN))
                    SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
}

/* The host path the last translation produced.
 *
 * A caller that wants to act on the file a title just opened -- playing an FMV
 * the host can decode itself, say -- has the guest path but not the host one,
 * and re-deriving it would duplicate every rule above. */
/* Thread-local: several threads open files at once, and a plain static let one
 * thread's translation overwrite another's between the translate and the read.
 * That showed up as the FMV trigger firing on roughly two runs in three. */

static void xbox_remember_host_path(const wchar_t *p)
{
    if (p) wcsncpy_s(s_last_host_path_shared, MAX_PATH, p, _TRUNCATE);
}



/* A clock for unattended runs that starts at a point in the title rather than
 * at process start. The title reaches its title screen anywhere from 100 to
 * 190 s into a run, depending on how start-up goes, so a button script in
 * absolute seconds lands on different screens each time. Anchored to the Nth
 * open of a file -- Def Jam loads screens\feflow.xml once at boot and again
 * as the title screen comes up, so "feflow.xml#2" -- the same script reaches
 * the same screens.
 *
 *     RECOMP_SCRIPT_ANCHOR=feflow.xml#2
 */
static ULONGLONG s_anchor_ft;             /* FILETIME the anchor was reached */
static int s_anchor_state = -1;           /* -1 unread, 0 none, 1 waiting, 2 reached */
static char s_anchor_sub[128];
static int s_anchor_want = 1, s_anchor_seen;

static void anchor_init(void)
{
    const char *s = getenv("RECOMP_SCRIPT_ANCHOR");
    const char *hash;
    size_t i, n;

    s_anchor_state = 0;
    if (!s || !*s)
        return;
    hash = strchr(s, '#');
    n = hash ? (size_t)(hash - s) : strlen(s);
    if (n >= sizeof(s_anchor_sub))
        n = sizeof(s_anchor_sub) - 1;
    for (i = 0; i < n; i++)
        s_anchor_sub[i] = (char)tolower((unsigned char)s[i]);
    s_anchor_sub[n] = 0;
    if (hash && atoi(hash + 1) > 0)
        s_anchor_want = atoi(hash + 1);
    s_anchor_state = 1;
}

/* More anchors, for a script that crosses several screens whose timing varies:
 * xbox_FileOpenSeconds("main.mus#1") is the seconds since the first open of a
 * path containing "main.mus", negative before it. A spec is watched from its
 * first query, so ask before the file can open (the pad and the translator do,
 * from start-up). */
/* A screen-by-screen chain uses one per screen, the captures more. */
#define OPEN_WATCH_MAX 32
static struct { char sub[64]; int want, seen; volatile ULONGLONG ft; } s_open_watch[OPEN_WATCH_MAX];
static volatile LONG s_open_watches;
static CRITICAL_SECTION s_open_watch_lock;
static INIT_ONCE s_open_watch_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK open_watch_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_open_watch_lock);
    return TRUE;
}

static ULONGLONG now_ft(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

double xbox_FileOpenSeconds(const char *spec)
{
    char sub[64];
    const char *hash = strchr(spec, '#');
    size_t i, n = hash ? (size_t)(hash - spec) : strlen(spec);
    int want = (hash && atoi(hash + 1) > 0) ? atoi(hash + 1) : 1;
    LONG k, count;
    ULONGLONG ft = 0;

    if (n >= sizeof sub)
        n = sizeof sub - 1;
    for (i = 0; i < n; i++)
        sub[i] = (char)tolower((unsigned char)spec[i]);
    sub[n] = 0;
    InitOnceExecuteOnce(&s_open_watch_once, open_watch_init, NULL, NULL);
    EnterCriticalSection(&s_open_watch_lock);
    count = s_open_watches;
    for (k = 0; k < count; k++)
        if (s_open_watch[k].want == want && !strcmp(s_open_watch[k].sub, sub))
            break;
    if (k == count && count < OPEN_WATCH_MAX) {
        memcpy(s_open_watch[k].sub, sub, n + 1);
        s_open_watch[k].want = want;
        s_open_watches = count + 1;
    } else if (k == count) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [SCRIPT] more than %d anchors: \"%s\" is never reached\n",
                    OPEN_WATCH_MAX, sub);
    }
    if (k < OPEN_WATCH_MAX)
        ft = s_open_watch[k].ft;
    LeaveCriticalSection(&s_open_watch_lock);
    return ft ? (double)(now_ft() - ft) / 1e7 : -1.0;
}

static void anchor_note_open(const char *xbox_path)
{
    char low[512];
    size_t i;
    LONG k;

    for (i = 0; xbox_path[i] && i < sizeof(low) - 1; i++)
        low[i] = (char)tolower((unsigned char)xbox_path[i]);
    low[i] = 0;
    if (s_open_watches) {
        EnterCriticalSection(&s_open_watch_lock);
        for (k = 0; k < s_open_watches; k++) {
            if (s_open_watch[k].ft || !strstr(low, s_open_watch[k].sub)
                    || ++s_open_watch[k].seen < s_open_watch[k].want)
                continue;
            s_open_watch[k].ft = now_ft();
            fprintf(stderr, "  [SCRIPT] anchor \"%s\" #%d reached\n",
                    s_open_watch[k].sub, s_open_watch[k].want);
        }
        LeaveCriticalSection(&s_open_watch_lock);
    }
    if (s_anchor_state < 0)
        anchor_init();
    if (s_anchor_state != 1)
        return;
    if (!strstr(low, s_anchor_sub) || ++s_anchor_seen < s_anchor_want)
        return;
    s_anchor_ft = now_ft();
    s_anchor_state = 2;
    fprintf(stderr, "  [SCRIPT] anchor \"%s\" #%d reached: script time starts now\n",
            s_anchor_sub, s_anchor_want);
}

/* Anything else worth anchoring a script to -- a title's own events, such as
 * the screen its front end asks for -- is offered here, and matches the same
 * specs as a file path does. */
void xbox_NoteAnchorEvent(const char *text)
{
    if (text)
        anchor_note_open(text);
}

double xbox_ScriptSeconds(void)
{
    FILETIME c, e, k, u, now;
    ULONGLONG from, to;

    if (s_anchor_state < 0)
        anchor_init();
    if (s_anchor_state == 1)
        return -1.0;
    GetSystemTimeAsFileTime(&now);
    to = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    if (s_anchor_state == 2) {
        from = s_anchor_ft;
    } else {
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
            return 0.0;
        from = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    }
    return (double)(to - from) / 1e7;
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    if (xbox_partition_device_path(xbox_path, host_path_buf, buf_size)) {
        fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
        fflush(stderr);
        return TRUE;
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_win;
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    if (g_xbox_path_hook)
        g_xbox_path_hook(xbox_path);
    fprintf(stderr, "  [PATH] %s\n", xbox_path);
    anchor_note_open(xbox_path);
    xbox_KernelTrail(8);
    fflush(stderr);
    {
        WCHAR remainder_wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(s_save_dir, NULL);
            CreateDirectoryW(dir_path, NULL);
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        /* An empty remainder means the title opened the device itself, so
         * the join above leaves a trailing separator -- which CreateFileW
         * rejects even with FILE_FLAG_BACKUP_SEMANTICS, turning an existing
         * directory into STATUS_OBJECT_PATH_NOT_FOUND. */
        {
            size_t n = wcslen(host_path_buf);
            while (n > 1 && (host_path_buf[n - 1] == L'\\'
                             || host_path_buf[n - 1] == L'/'))
                host_path_buf[--n] = L'\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        xbox_remember_host_path(host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
