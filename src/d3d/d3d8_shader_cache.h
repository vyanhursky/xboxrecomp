#ifndef XBOXRECOMP_D3D11_SHADER_CACHE_H
#define XBOXRECOMP_D3D11_SHADER_CACHE_H
#if defined(_WIN32)
#include <d3dcompiler.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Same ownership/error contract as D3DCompile. Defines/includes bypass disk
 * caching until their complete dependency identity can be represented. */
HRESULT d3d8_compile_cached(LPCVOID source, SIZE_T length, LPCSTR name,
    const D3D_SHADER_MACRO *defines, ID3DInclude *include, LPCSTR entry,
    LPCSTR profile, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors);
typedef struct { unsigned calls, hits, compiles, bad, writes; } D3D8ShaderCacheStats;
void d3d8_shader_cache_stats(D3D8ShaderCacheStats *out);
#ifdef __cplusplus
}
#endif
#endif
#endif
