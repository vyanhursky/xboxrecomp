/* Opt-in persistent DXBC. Independently written for the current renderer.
 * Full compile keys and payload checks reject hash collisions/truncated files.
 * Only successful compiles are published, with a same-directory atomic rename.
 */
#define COBJMACROS
#include "d3d8_shader_cache.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <direct.h>

#define CACHE_LIMIT (16u << 20)
typedef struct { char magic[8]; uint64_t key, payload; uint32_t key_bytes, bytes; } CacheHeader;
static int initialized, enabled, stats;
static wchar_t folder[2048];
static uint64_t compiler_identity;
static unsigned calls, hits, compiles, bad, writes;
static double compile_ms, compile_cpu_ms, io_ms;
static LARGE_INTEGER frequency;
static volatile LONG serial;
static INIT_ONCE cache_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION cache_lock;

static BOOL CALLBACK initialize_lock(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&cache_lock);
    return TRUE;
}

static uint64_t hash_bytes(const void *p, size_t n, uint64_t h)
{
    const unsigned char *s = (const unsigned char *)p;
    while (n--) h = (h ^ *s++) * 1099511628211ull;
    return h;
}

static double ticks_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{ return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)frequency.QuadPart; }

static uint64_t thread_time(void)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static int utf8_path(const char *p, wchar_t *out, int count)
{ return p && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, out, count) > 0; }

static int make_folder(wchar_t *path)
{
    wchar_t absolute[2048];
    size_t i, start = 3, n;
    DWORD length = GetFullPathNameW(path, 2048, absolute, NULL);
    if (!length || length >= 2048) return 0;
    wcscpy(path, absolute);
    n = wcslen(path);
    if (path[0] == L'\\' && path[1] == L'\\') {
        const wchar_t *server = wcschr(path + 2, L'\\');
        const wchar_t *share = server ? wcschr(server + 1, L'\\') : NULL;
        if (!server) return 0;
        start = share ? (size_t)(share - path) + 1 : n;
    }
    for (i = start; i <= n; i++) {
        if (path[i] == L'/' || path[i] == L'\\' || !path[i]) {
            wchar_t old = path[i];
            DWORD attr;
            path[i] = 0;
            if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                fprintf(stderr, "[S2SHADER] mkdir failed %ls error %lu\n", path, GetLastError());
            attr = GetFileAttributesW(path);
            if (attr == INVALID_FILE_ATTRIBUTES) fprintf(stderr, "[S2SHADER] failed directory prefix: %ls\n", path);
            path[i] = old;
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
        }
    }
    return 1;
}

static void initialize(void)
{
    const char *flag = getenv("RECOMP_D3D11_SHADER_CACHE");
    const char *dir = getenv("RECOMP_D3D11_SHADER_CACHE_DIR");
    const char *root = getenv("DEFJAM_DATA");
    char local[2048];
    HMODULE dll = NULL;
    HMODULE imported_compiler;
    FARPROC compiler;
    wchar_t dll_path[2048];
    FILE *file;
    unsigned char block[65536];
    size_t n;
    if (initialized) return;
    initialized = 1;
    stats = getenv("RECOMP_STREAM2_STATS") && *getenv("RECOMP_STREAM2_STATS") == '1';
    QueryPerformanceFrequency(&frequency);
    if (!flag || *flag != '1') return;
    if (!dir || !*dir) {
        if (!root || strlen(root) + 20 >= sizeof local) return;
        snprintf(local, sizeof local, "%s/cache/d3d11", root);
        dir = local;
    }
    if (!utf8_path(dir, folder, (int)(sizeof folder / sizeof folder[0])) || !make_folder(folder)) {
        if (stats) fprintf(stderr, "[S2SHADER] cache directory unavailable: %s (%lu)\n", dir, GetLastError());
        return;
    }
    /* A linked function address can be an import thunk in the executable.
     * Resolve the export first so identity follows the actual compiler image. */
    imported_compiler = GetModuleHandleW(D3DCOMPILER_DLL_W);
    compiler = imported_compiler ? GetProcAddress(imported_compiler, "D3DCompile") : NULL;
    if (!compiler || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)(uintptr_t)compiler, &dll)) {
        if (stats) fprintf(stderr, "[S2SHADER] compiler module unavailable (%lu)\n", GetLastError());
        return;
    }
    n = GetModuleFileNameW(dll, dll_path, (DWORD)(sizeof dll_path / sizeof dll_path[0]));
    if (!n || n >= sizeof dll_path / sizeof dll_path[0]) return;
    file = _wfopen(dll_path, L"rb");
    if (!file) {
        if (stats) fprintf(stderr, "[S2SHADER] compiler image unreadable: %ls\n", dll_path);
        return;
    }
    compiler_identity = 14695981039346656037ull;
    while ((n = fread(block, 1, sizeof block, file)) != 0)
        compiler_identity = hash_bytes(block, n, compiler_identity);
    if (!ferror(file)) enabled = 1;
    fclose(file);
}

static unsigned char *compile_key(const void *source, size_t length, const char *name,
    const char *entry, const char *profile, UINT f1, UINT f2, size_t *bytes)
{
    uint32_t fields[6];
    unsigned char *key, *at;
    size_t ns = name ? strlen(name) + 1 : 0, es = entry ? strlen(entry) + 1 : 0;
    size_t ps = profile ? strlen(profile) + 1 : 0;
    if (length > CACHE_LIMIT || ns > 4096 || es > 4096 || ps > 4096) return NULL;
    fields[0] = f1; fields[1] = f2; fields[2] = (uint32_t)ns;
    fields[3] = (uint32_t)es; fields[4] = (uint32_t)ps; fields[5] = (uint32_t)length;
    *bytes = sizeof compiler_identity + sizeof fields + ns + es + ps + length;
    key = (unsigned char *)malloc(*bytes);
    if (!key) return NULL;
    at = key;
#define APPEND(p, n) do { if (n) memcpy(at, p, n); at += (n); } while (0)
    APPEND(&compiler_identity, sizeof compiler_identity);
    APPEND(fields, sizeof fields);
    APPEND(name, ns); APPEND(entry, es); APPEND(profile, ps); APPEND(source, length);
#undef APPEND
    return key;
}

static int valid_dxbc(const void *data, size_t bytes)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t version, length, chunks, i, offset, size;
    if (bytes < 36 || memcmp(p, "DXBC", 4)) return 0;
    memcpy(&version, p + 20, 4); memcpy(&length, p + 24, 4); memcpy(&chunks, p + 28, 4);
    if (version != 1 || length != bytes || !chunks || chunks > (bytes - 32) / 4) return 0;
    for (i = 0; i < chunks; i++) {
        memcpy(&offset, p + 32 + i * 4, 4);
        if (offset < 32 + chunks * 4 || offset > bytes - 8) return 0;
        memcpy(&size, p + offset + 4, 4);
        if (size > bytes - offset - 8) return 0;
    }
    return 1;
}

static int load_blob(const wchar_t *path, const unsigned char *key, size_t key_bytes,
                     uint64_t identity, ID3DBlob **code)
{
    CacheHeader h;
    unsigned char *stored = NULL;
    FILE *f = _wfopen(path, L"rb");
    int ok = 0;
    if (!f) return 0;
    memset(&h, 0, sizeof h);
    if (fread(&h, 1, sizeof h, f) != sizeof h || memcmp(h.magic, "XRDXBC01", 8) ||
        h.key != identity || h.key_bytes != key_bytes || h.bytes < 4 || h.bytes > CACHE_LIMIT) goto done;
    stored = (unsigned char *)malloc(key_bytes);
    if (!stored || fread(stored, 1, key_bytes, f) != key_bytes || memcmp(stored, key, key_bytes)) goto done;
    if (FAILED(D3DCreateBlob(h.bytes, code))) goto done;
    if (fread(ID3D10Blob_GetBufferPointer(*code), 1, h.bytes, f) != h.bytes || fgetc(f) != EOF ||
        !valid_dxbc(ID3D10Blob_GetBufferPointer(*code), h.bytes) ||
        hash_bytes(ID3D10Blob_GetBufferPointer(*code), h.bytes, 14695981039346656037ull) != h.payload) {
        ID3D10Blob_Release(*code); *code = NULL; goto done;
    }
    ok = 1;
done:
    if (!ok) bad++;
    free(stored);
    fclose(f);
    return ok;
}

static void store_blob(const wchar_t *path, const unsigned char *key, size_t key_bytes,
                       uint64_t identity, ID3DBlob *code)
{
    wchar_t temp[2304];
    CacheHeader h;
    size_t bytes = ID3D10Blob_GetBufferSize(code);
    FILE *f;
    int ok;
    if (bytes < 4 || bytes > CACHE_LIMIT || memcmp(ID3D10Blob_GetBufferPointer(code), "DXBC", 4)) return;
    if (_snwprintf(temp, sizeof temp / sizeof temp[0], L"%ls.%lu.%lu.%ld.tmp", path,
        GetCurrentProcessId(), GetCurrentThreadId(), InterlockedIncrement(&serial)) < 0) return;
    f = _wfopen(temp, L"wb");
    if (!f) return;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "XRDXBC01", 8);
    h.key = identity; h.key_bytes = (uint32_t)key_bytes; h.bytes = (uint32_t)bytes;
    h.payload = hash_bytes(ID3D10Blob_GetBufferPointer(code), bytes, 14695981039346656037ull);
    ok = fwrite(&h, 1, sizeof h, f) == sizeof h && fwrite(key, 1, key_bytes, f) == key_bytes &&
         fwrite(ID3D10Blob_GetBufferPointer(code), 1, bytes, f) == bytes;
    ok = fclose(f) == 0 && ok;
    if (ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) writes++;
    else DeleteFileW(temp);
}

void d3d8_shader_cache_stats(D3D8ShaderCacheStats *out)
{
    if (!out) return;
    InitOnceExecuteOnce(&cache_once, initialize_lock, NULL, NULL);
    EnterCriticalSection(&cache_lock);
    out->calls = calls; out->hits = hits; out->compiles = compiles;
    out->bad = bad; out->writes = writes;
    LeaveCriticalSection(&cache_lock);
}

HRESULT d3d8_compile_cached(LPCVOID source, SIZE_T length, LPCSTR name,
    const D3D_SHADER_MACRO *defines, ID3DInclude *include, LPCSTR entry,
    LPCSTR profile, UINT f1, UINT f2, ID3DBlob **code, ID3DBlob **errors)
{
    unsigned char *key = NULL;
    size_t key_bytes = 0;
    uint64_t identity = 0, cpu = 0;
    LARGE_INTEGER a = {0}, b = {0};
    wchar_t path[2200];
    HRESULT hr = S_OK;
    const char *cache_flag = getenv("RECOMP_D3D11_SHADER_CACHE");
    const char *stats_flag = getenv("RECOMP_STREAM2_STATS");
    if ((!cache_flag || *cache_flag != '1') && (!stats_flag || *stats_flag != '1'))
        return D3DCompile(source, length, name, defines, include, entry, profile, f1, f2, code, errors);
    InitOnceExecuteOnce(&cache_once, initialize_lock, NULL, NULL);
    EnterCriticalSection(&cache_lock);
    initialize();
    calls++;
    if (code) *code = NULL;
    if (errors) *errors = NULL;
    if (enabled && source && code && !defines && !include)
        key = compile_key(source, length, name, entry, profile, f1, f2, &key_bytes);
    if (key) {
        identity = hash_bytes(key, key_bytes, 14695981039346656037ull);
        _snwprintf(path, sizeof path / sizeof path[0], L"%ls/%016llx.dxbc", folder, identity);
        if (stats) QueryPerformanceCounter(&a);
        if (load_blob(path, key, key_bytes, identity, code)) {
            hits++;
            if (stats) { QueryPerformanceCounter(&b); io_ms += ticks_ms(a, b); }
            goto done;
        }
        if (stats) { QueryPerformanceCounter(&b); io_ms += ticks_ms(a, b); }
    }
    if (stats) { QueryPerformanceCounter(&a); cpu = thread_time(); }
    compiles++;
    hr = D3DCompile(source, length, name, defines, include, entry, profile, f1, f2, code, errors);
    if (stats) {
        QueryPerformanceCounter(&b); compile_ms += ticks_ms(a, b);
        compile_cpu_ms += (double)(thread_time() - cpu) / 10000.0;
    }
    if (SUCCEEDED(hr) && key && code && *code) {
        if (stats) QueryPerformanceCounter(&a);
        store_blob(path, key, key_bytes, identity, *code);
        if (stats) { QueryPerformanceCounter(&b); io_ms += ticks_ms(a, b); }
    }
done:
    free(key);
    if (stats) fprintf(stderr, "[S2SHADER] enabled=%d calls=%u hits=%u compiles=%u compile_ms=%.3f compile_cpu_ms=%.3f io_ms=%.3f bad=%u writes=%u\n",
        enabled, calls, hits, compiles, compile_ms, compile_cpu_ms, io_ms, bad, writes);
    LeaveCriticalSection(&cache_lock);
    return hr;
}
