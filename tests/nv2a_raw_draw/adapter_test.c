/* Game-free WARP differential: capture the adapter's actual float4 inputs,
 * compare native packed IA + shader decode with the CPU's gathered values.
 * Reuse the same backing address while mutating it between cases. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "nv2a_vsh_cpu.h"
#include "nv2a_vsh_interp.h"
#include "d3d8_raw_input.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11GeometryShader *gs;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define DROP(x) do { if(x) IUnknown_Release((IUnknown *)x); } while(0)

static void start(void)
{
    ID3DBlob *code=NULL,*err=NULL;
    unsigned stride=16;
    /* D3D11 links stage registers: preserve the complete production output
     * signature, including fields not selected by stream-out. */
    const char *hlsl="struct O{float4 p:SV_POSITION;float4 d:COLOR0;float4 s:COLOR1;"
        "float3 t0:TEXCOORD0;float3 t1:TEXCOORD1;float3 t2:TEXCOORD2;float3 t3:TEXCOORD3;"
        "float fog:TEXCOORD4;float4 v:TEXCOORD5;float size:TEXCOORD6;};"
        "[maxvertexcount(1)] void main(point O i[1],inout PointStream<O> s){s.Append(i[0]);}";
    const D3D11_SO_DECLARATION_ENTRY decl={0,"TEXCOORD",5,0,4,0};
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&dev,NULL,&ctx)));
    CHECK(SUCCEEDED(D3DCompile(hlsl,strlen(hlsl),"capture",NULL,NULL,"main","gs_5_0",0,0,&code,&err)));
    CHECK(SUCCEEDED(ID3D11Device_CreateGeometryShaderWithStreamOutput(dev,
        ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),&decl,1,&stride,1,
        D3D11_SO_NO_RASTERIZED_STREAM,NULL,&gs)));
    DROP(code); DROP(err);
}

static void run(const void *input, const Nv2aRawLayout *raw, float output[4])
{
    char hlsl[65536],tail[65536]; uint32_t used;
    ID3DBlob *code=NULL,*err=NULL;
    ID3D11VertexShader *vs=NULL; ID3D11InputLayout *il=NULL;
    ID3D11Buffer *vb=NULL,*cb1=NULL,*cb2=NULL,*so=NULL,*rb=NULL;
    D3D11_BUFFER_DESC bd={0}; D3D11_SUBRESOURCE_DATA data={0};
    D3D11_INPUT_ELEMENT_DESC el={0}; D3D11_MAPPED_SUBRESOURCE map;
    unsigned stride=raw?raw->stride[0]:16,offset=0;
    float frame[8]={1,1,1,0,1,1,1,1};
    CHECK(nv2a_vsh_emit_hlsl_raw(hlsl,sizeof hlsl,&used,raw)>0 && used==8);
    /* Capture the decoded v3 in an otherwise unused output. The production
     * adapter emits this variable; only the observation sink differs. */
    char *at=strstr(hlsl,"o.viewpos = float4(0, 0, 0, 1);"); CHECK(at);
    strcpy(tail,at+strlen("o.viewpos = float4(0, 0, 0, 1);"));
    strcpy(at,"o.viewpos = v3;"); strcat(at,tail);
    /* Inputs include zero/negative w; keep the observation point valid so
     * raster/clip behavior cannot suppress the input-conversion probe. */
    const char *pos="o.pos = float4(ndc * w, R[12].z * Screen.z * w, w);";
    at=strstr(hlsl,pos);CHECK(at);strcpy(tail,at+strlen(pos));
    strcpy(at,"o.pos = float4(0, 0, 0, 1);");strcat(at,tail);
    HRESULT hr=D3DCompile(hlsl,strlen(hlsl),"raw_adapter",NULL,NULL,"main","vs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&err);
    if(FAILED(hr) && err) fprintf(stderr,"%s\n",(const char *)ID3D10Blob_GetBufferPointer(err));
    CHECK(SUCCEEDED(hr));
    if (getenv("RAW_TEST_DUMP") && !raw) {
        ID3DBlob *assembly=NULL;
        puts(hlsl);
        if(SUCCEEDED(D3DDisassemble(ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),0,NULL,&assembly))) {
            puts((const char *)ID3D10Blob_GetBufferPointer(assembly));DROP(assembly);
        }
    }
    CHECK(SUCCEEDED(ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&vs)));
    el.SemanticName="ATTR"; el.SemanticIndex=3;
    el.Format=raw?d3d8_raw_format(raw->type[3],raw->size[3]):DXGI_FORMAT_R32G32B32A32_FLOAT;
    el.AlignedByteOffset=raw?raw->offset[3]:0;
    CHECK(SUCCEEDED(ID3D11Device_CreateInputLayout(dev,&el,1,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),&il)));
    bd.ByteWidth=stride; bd.Usage=D3D11_USAGE_IMMUTABLE; bd.BindFlags=D3D11_BIND_VERTEX_BUFFER; data.pSysMem=input;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&bd,&data,&vb)));
    bd.ByteWidth=192*16; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; data.pSysMem=nv2a_vsh_constants(NULL);
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&bd,&data,&cb1)));
    bd.ByteWidth=sizeof frame; data.pSysMem=frame;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&bd,&data,&cb2)));
    memset(&bd,0,sizeof bd); bd.ByteWidth=16; bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&bd,NULL,&so)));
    bd.Usage=D3D11_USAGE_STAGING; bd.BindFlags=0; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&bd,NULL,&rb)));
    ID3D11DeviceContext_IASetInputLayout(ctx,il);
    ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&vb,&stride,&offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,1,&cb1);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,2,1,&cb2);
    ID3D11DeviceContext_GSSetShader(ctx,gs,NULL,0);
    ID3D11DeviceContext_SOSetTargets(ctx,1,&so,&offset);
    ID3D11DeviceContext_Draw(ctx,1,0);
    ID3D11DeviceContext_SOSetTargets(ctx,0,NULL,NULL);
    ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)rb,(ID3D11Resource *)so);
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)rb,0,D3D11_MAP_READ,0,&map)));
    memcpy(output,map.pData,16); ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)rb,0);
    DROP(rb);DROP(so);DROP(cb2);DROP(cb1);DROP(vb);DROP(il);DROP(vs);DROP(err);DROP(code);
}

static void decode(const unsigned char *p,uint32_t t,uint32_t n,float out[4])
{
    out[0]=out[1]=out[2]=0;out[3]=1;
    if(t==0) { for(unsigned c=0;c<4;++c) out[c]=p[c==0?2:c==2?0:c]/255.0f; }
    else if(t==6) {
        uint32_t w;memcpy(&w,p,4);
        out[0]=(float)((int32_t)(w<<21)>>21)/1023.0f;
        out[1]=(float)((int32_t)(w<<10)>>21)/1023.0f;
        out[2]=(float)((int32_t)w>>22)/511.0f;
    } else for(unsigned c=0;c<n;++c) {
        if(t==2) memcpy(out+c,p+c*4,4);
        else if(t==4) out[c]=p[c]/255.0f;
        else { int16_t s;memcpy(&s,p+c*2,2);out[c]=t==1?s/32767.0f:(float)s; }
    }
}
int main(void)
{
    /* MOV oPos,v3, final. Encoding matches the independently tested adapter. */
    uint32_t ins[4]={0,(1u<<21)|(3u<<9)|0x1bu,2u<<26,(15u<<12)|(1u<<11)|1u};
    uint32_t rng=0x72839u,cases=0;
    unsigned char bytes[32]; float expected[4],reference[4],actual[4];
    nv2a_vsh_set_instruction(0,ins);start();
    for(uint32_t t=0;t<=6;++t) if(t!=3) for(uint32_t n=1;n<=4;++n) for(unsigned trial=0;trial<12;++trial) {
        Nv2aRawLayout raw={0};raw.used=8;raw.streams=1;raw.stride[0]=32;raw.type[3]=t;raw.size[3]=n;raw.offset[3]=4;
        for(unsigned b=0;b<sizeof bytes;++b) { rng=rng*1664525u+1013904223u;bytes[b]=(unsigned char)(rng>>24); }
        if(t==2) for(unsigned c=0;c<n;++c) { float f=(float)((int)(rng>>16)-32768)/17.0f;rng=rng*1664525u+1013904223u;memcpy(bytes+4+c*4,&f,4); }
        if(trial==0) memset(bytes+4,0,16);
        if(trial==1) memset(bytes+4,0xff,16);
        if(trial==2 && (t==1||t==5)) { int16_t low=-32768;for(unsigned c=0;c<n;++c)memcpy(bytes+4+c*2,&low,2); }
        if(t==2 && trial==1) for(unsigned c=0;c<n;++c) {float f=1.0f;memcpy(bytes+4+c*4,&f,4);}
        decode(bytes+4,t,n,expected);run(expected,NULL,reference);run(bytes,&raw,actual);
        Nv2aRawDraw snapshot={0};float fallback[4];snapshot.layout=raw;snapshot.vertices=bytes;snapshot.nverts=1;
        nv2a_raw_gather(&snapshot,fallback);CHECK(!memcmp(fallback,expected,16));
        for(unsigned c=0;c<4;++c) {
            if(reference[c]!=expected[c]) {
                fprintf(stderr,"reference type %u size %u trial %u component %u: %.9g vs %.9g (raw %.9g)\n",
                    t,n,trial,c,reference[c],expected[c],actual[c]);return 1;
            }
            if(fabsf(actual[c]-expected[c])>2e-7f*fmaxf(1.0f,fabsf(expected[c]))) {
                fprintf(stderr,"type %u size %u trial %u component %u: %.9g vs %.9g\n",t,n,trial,c,actual[c],expected[c]);return 1;
            }
        }
        ++cases;
    }
    printf("raw GPU adapter: %u scalar/native cases passed\n",cases);
    ID3D11DeviceContext_ClearState(ctx);DROP(gs);DROP(ctx);DROP(dev);return 0;
}
