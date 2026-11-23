/* Real D3DCompile, disk corruption, complete-key and WARP blob checks. */
#define COBJMACROS
#include "d3d8_shader_cache.h"
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char source[] = "float4 main():SV_Target{return 1;} float4 other():SV_Target{return 0;}";
static int failures;
#define CHECK(name, value) do { if (!(value)) { fprintf(stderr, "FAIL: %s\n", name); failures++; } } while (0)

static ID3DBlob *compile(const char *text, const char *name, const char *entry, const char *profile, UINT flags)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = d3d8_compile_cached(text, strlen(text), name, NULL, NULL, entry, profile, flags, 0, &code, &errors);
    CHECK("compile succeeds", SUCCEEDED(hr) && code);
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

static DWORD WINAPI worker(void *unused)
{
    ID3DBlob *b;
    (void)unused;
    b = compile(source, "threaded", "main", "ps_5_0", 0);
    if (b) ID3D10Blob_Release(b);
    return 0;
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAA data;
    HANDLE find, threads[4];
    D3D8ShaderCacheStats a, b;
    ID3DBlob *cold, *warm;
    ID3D11Device *device = NULL;
    ID3D11PixelShader *shader = NULL;
    FILE *file;
    int off = argc > 1 && !strcmp(argv[1], "off"), i;
    GetCurrentDirectoryA(sizeof temp, temp);
    snprintf(dir, sizeof dir, "%s/xrdxbc-test-%lu", temp, GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    _putenv_s("RECOMP_D3D11_SHADER_CACHE", off ? "0" : "1");
    _putenv_s("RECOMP_STREAM2_STATS", "1");
    _putenv_s("RECOMP_D3D11_SHADER_CACHE_DIR", dir);
    cold = compile(source, NULL, "main", "ps_5_0", 0);
    warm = compile(source, NULL, "main", "ps_5_0", 0);
    CHECK("cached blob byte identical", cold && warm && ID3D10Blob_GetBufferSize(cold) == ID3D10Blob_GetBufferSize(warm) &&
        !memcmp(ID3D10Blob_GetBufferPointer(cold), ID3D10Blob_GetBufferPointer(warm), ID3D10Blob_GetBufferSize(cold)));
    d3d8_shader_cache_stats(&a);
    CHECK("cold/warm counters", a.compiles == (off ? 2u : 1u) && a.hits == (off ? 0u : 1u));
    CHECK("WARP device", SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &device, NULL, NULL)));
    if (device && warm) CHECK("cached blob creates real shader", SUCCEEDED(ID3D11Device_CreatePixelShader(device,
        ID3D10Blob_GetBufferPointer(warm), ID3D10Blob_GetBufferSize(warm), NULL, &shader)));
    if (shader) ID3D11PixelShader_Release(shader);
    if (device) ID3D11Device_Release(device);
    if (cold) ID3D10Blob_Release(cold);
    if (warm) ID3D10Blob_Release(warm);
    snprintf(pattern, sizeof pattern, "%s/*.dxbc", dir);
    find = FindFirstFileA(pattern, &data);
    if (!off) {
        CHECK("cache file written", find != INVALID_HANDLE_VALUE);
        if (find != INVALID_HANDLE_VALUE) {
            snprintf(path, sizeof path, "%s/%s", dir, data.cFileName);
            FindClose(find);
            file = fopen(path, "r+b");
            if (file) { fseek(file, -1, SEEK_END); fputc(0xAB, file); fclose(file); }
            warm = compile(source, NULL, "main", "ps_5_0", 0);
            if (warm) ID3D10Blob_Release(warm);
            d3d8_shader_cache_stats(&b);
            CHECK("payload corruption recompiles", b.compiles == a.compiles + 1 && b.bad == a.bad + 1);
            file = fopen(path, "wb");
            if (file) { fputs("XRDXBC01", file); fclose(file); }
            warm = compile(source, NULL, "main", "ps_5_0", 0);
            if (warm) ID3D10Blob_Release(warm);
            d3d8_shader_cache_stats(&a);
            CHECK("truncated header recompiles", a.compiles == b.compiles + 1 && a.bad == b.bad + 1);
        }
        /* NULL name differs from empty; entry/profile/flags/source all key. */
        d3d8_shader_cache_stats(&a);
        warm = compile(source, "", "main", "ps_5_0", 0); if (warm) ID3D10Blob_Release(warm);
        warm = compile(source, NULL, "other", "ps_5_0", 0); if (warm) ID3D10Blob_Release(warm);
        warm = compile(source, NULL, "main", "ps_4_0", 0); if (warm) ID3D10Blob_Release(warm);
        warm = compile(source, NULL, "main", "ps_5_0", D3DCOMPILE_SKIP_OPTIMIZATION); if (warm) ID3D10Blob_Release(warm);
        warm = compile("float4 main():SV_Target{return 0.5;}", NULL, "main", "ps_5_0", 0); if (warm) ID3D10Blob_Release(warm);
        d3d8_shader_cache_stats(&b);
        CHECK("all compile identity fields invalidate", b.compiles == a.compiles + 5);
        a = b;
        for (i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        WaitForMultipleObjects(4, threads, TRUE, INFINITE);
        for (i = 0; i < 4; i++) CloseHandle(threads[i]);
        d3d8_shader_cache_stats(&b);
        CHECK("concurrent lookup publishes once", b.compiles == a.compiles + 1 && b.hits == a.hits + 3);
    } else if (find != INVALID_HANDLE_VALUE) { FindClose(find); CHECK("disabled cache writes nothing", 0); }
    find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do { snprintf(path, sizeof path, "%s/%s", dir, data.cFileName); DeleteFileA(path); } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    RemoveDirectoryA(dir);
    printf("d3d8_shader_cache: %d FAILURE(S)\n", failures);
    return failures ? 1 : 0;
}
