/* Optional content-addressed PNG/DDS packs; no work on the original path when off.
 * v1 ID: XRTEX01 NUL + five LE32 descriptor fields + tight pixels + BGRA palette.
 * Source identities and replacement resources are deliberately separate caches.
 */
#define COBJMACROS
#include "texture_pack.h"
#if defined(_WIN32)
#include <windows.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ENTRIES 65536
#define SOURCE_SLOTS 2048
#define PATH_CAP 1024
typedef struct PackEntry {
    char id[65], path[PATH_CAP];
    unsigned order;
    IDirect3DTexture8 *texture;
    uint64_t bytes, used;
    unsigned levels;
    int channel_mips;
    int failed;
    ULONGLONG retry;
} PackEntry;
static PackEntry *g_entries;
static size_t g_count, g_capacity;
static struct SourceState {
    RecompTextureSource shape;
    char id[65];
    ULONGLONG checked;
    int used, dumped;
} g_sources[SOURCE_SLOTS];
static unsigned g_source_next;
static IWICImagingFactory *g_wic;
static BCRYPT_ALG_HANDLE g_sha;
static int g_initialized, g_active, g_com, g_dump_limit=2000;
static char g_dump[PATH_CAP];
static uint64_t g_budget, g_resident, g_clock, g_hits, g_misses, g_hashed, g_dumps;
static unsigned g_errors,g_loads;

static void warn(const char *what, const char *path)
{
    if (g_errors++ < 32) fprintf(stderr,"[TEXPACK] %s: %s\n",what,path ? path : "");
}
static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s)) ++s;
    end=s+strlen(s);
    while (end>s && isspace((unsigned char)end[-1])) *--end=0;
    return s;
}
static int valid_id(const char *s)
{
    unsigned i;
    if(strlen(s)!=64) return 0;
    for(i=0;i<64;i++) if(!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}
static int valid_name(const char *s, int relative)
{
    const unsigned char *p=(const unsigned char *)s;
    if(!*p || *p=='.' || *p=='/' || strstr(s,"..")) return 0;
    for(;*p;p++) if(!isalnum(*p) && *p!='_' && *p!='-' && *p!='.' && !(relative && *p=='/')) return 0;
    return 1;
}
static int entry_compare(const void *a,const void *b)
{
    const PackEntry *x=a,*y=b;
    int c=strcmp(x->id,y->id);
    return c ? c : (x->order>y->order)-(x->order<y->order);
}
static void read_pack(const char *root, const char *name)
{
    char path[PATH_CAP],line[2048],section[32]="";
    FILE *file;
    size_t start=g_count;
    int schema=0,channel_mips=0,mips_valid=1;
    if(!valid_name(name,0)) { warn("Invalid pack name",name); return; }
    if(snprintf(path,sizeof path,"%s/%s/manifest.ini",root,name)>=(int)sizeof path) return;
    file=fopen(path,"rb");
    if(!file) { warn("Pack manifest unavailable",path); return; }
    while(fgets(line,sizeof line,file)) {
        char *s=trim(line),*equal,*end;
        if(strlen(s)>1900) { warn("Manifest line too long",path); break; }
        if((unsigned char)s[0]==0xef && (unsigned char)s[1]==0xbb && (unsigned char)s[2]==0xbf) s+=3;
        if(!*s || *s==';' || *s=='#') continue;
        if(*s=='[') {
            end=strchr(s,']');
            if(!end || end-s>30) { section[0]=0; continue; }
            *end=0; snprintf(section,sizeof section,"%s",s+1); continue;
        }
        equal=strchr(s,'='); if(!equal) continue;
        *equal++=0; s=trim(s); equal=trim(equal);
        if(!strcmp(section,"pack") && !strcmp(s,"schema")) schema=!strcmp(equal,"1");
        if(!strcmp(section,"pack") && !strcmp(s,"png_mips")) {
            if(!strcmp(equal,"channels")) channel_mips=1;
            else if(!strcmp(equal,"opacity")) channel_mips=0;
            else mips_valid=0;
        }
        if(strcmp(section,"textures")) continue;
        if(!valid_id(s) || !valid_name(equal,1)) { warn("Invalid texture entry",s); continue; }
        end=strrchr(equal,'.');
        if(!end || (_stricmp(end,".png") && _stricmp(end,".dds"))) { warn("Unsupported extension",equal); continue; }
        if(g_count==MAX_ENTRIES) { warn("Pack entry limit",path); break; }
        if(g_count==g_capacity) {
            size_t capacity=g_capacity ? g_capacity*2 : 256;
            PackEntry *grown=realloc(g_entries,capacity*sizeof(*grown));
            if(!grown) break;
            g_entries=grown; g_capacity=capacity;
        }
        memset(&g_entries[g_count],0,sizeof g_entries[g_count]);
        for(end=s;*end;end++) *end=(char)tolower((unsigned char)*end);
        memcpy(g_entries[g_count].id,s,65);
        if(snprintf(g_entries[g_count].path,PATH_CAP,"%s/%s/%s",root,name,equal)>=PATH_CAP) continue;
        g_entries[g_count].order=(unsigned)g_count;
        g_count++;
    }
    fclose(file);
    if(!schema) { g_count=start; warn("Unsupported manifest schema",path); }
    else if(!mips_valid) { g_count=start; warn("Unsupported PNG mip policy",path); }
    else {
        size_t i;
        for(i=start;i<g_count;i++) g_entries[i].channel_mips=channel_mips;
    }
}
static void initialize(void)
{
    char root[PATH_CAP],packs[256],*next,*name;
    const char *e;
    long mb;
    if(g_initialized) return;
    g_initialized=1;
    mb=(e=getenv("RECOMP_TEXTURE_CACHE_MB")) ? strtol(e,NULL,10) : 512;
    if(mb<32) mb=32; if(mb>4096) mb=4096;
    g_budget=(uint64_t)mb*1024*1024;
    e=getenv("RECOMP_TEXTURE_DUMP_DIR"); if(e && *e) snprintf(g_dump,sizeof g_dump,"%s",e);
    if((e=getenv("RECOMP_TEXTURE_DUMP_LIMIT"))) {
        g_dump_limit=atoi(e); if(g_dump_limit<0) g_dump_limit=0; if(g_dump_limit>65536) g_dump_limit=65536;
    }
    e=getenv("RECOMP_TEXTURE_ROOT");
    if(e && *e) snprintf(root,sizeof root,"%s",e);
    else { e=getenv("DEFJAM_DATA"); snprintf(root,sizeof root,"%s/mods",e && *e ? e : "."); }
    e=getenv("RECOMP_TEXTURE_PACKS"); snprintf(packs,sizeof packs,"%s",e ? e : "");
    name=packs;
    while(*name) {
        next=strchr(name,';'); if(next) *next++=0;
        read_pack(root,trim(name));
        if(!next) break; name=next;
    }
    if(g_count) qsort(g_entries,g_count,sizeof(*g_entries),entry_compare);
    g_active=g_count || g_dump[0];
    if(g_active) {
        HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);
        g_com=SUCCEEDED(hr);
        if(FAILED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory,(void **)&g_wic))) warn("WIC unavailable","");
        if(g_dump[0]) SHCreateDirectoryExA(NULL,g_dump,NULL);
        fprintf(stderr,"[TEXPACK] schema=1 entries=%zu cache=%ld MiB dump=%s\n",g_count,mb,g_dump);
    }
}
int texture_pack_active(void) { initialize(); return g_active; }

static int readable(const void *data,uint64_t bytes)
{
    MEMORY_BASIC_INFORMATION region;
    uintptr_t begin=(uintptr_t)data,end=begin+(uintptr_t)bytes,at=begin,next;
    if(!data || !bytes || bytes>64u*1024u*1024u || end<begin) return 0;
    while(at<end) {
        if(!VirtualQuery((const void *)at,&region,sizeof region) || region.State!=MEM_COMMIT ||
           (region.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return 0;
        next=(uintptr_t)region.BaseAddress+region.RegionSize;
        if(next<=at) return 0;
        at=next;
    }
    return 1;
}
static int source_readable(const RecompTextureSource *s)
{
    return readable(s->data,s->bytes) && (!s->palette_entries || readable(s->palette,s->palette_entries*4u));
}

int texture_pack_hash(const RecompTextureSource *s,char out[65])
{
    BCRYPT_HASH_HANDLE hash=NULL;
    uint8_t header[28]={'X','R','T','E','X','0','1',0},result[32];
    uint32_t fields[5]={s->format,s->width,s->height,s->palette_entries,s->row_texels};
    unsigned i,j;
    NTSTATUS status;
    if(!s->data || !s->width || !s->height || s->width>4096 || s->height>4096 ||
       s->row_texels<s->width || s->row_texels>16384 || !s->bytes || s->bytes>64u*1024u*1024u || s->palette_entries>256 ||
       (s->palette_entries && !s->palette)) return 0;
    if(s->stride && (!s->rows || !s->row_bytes || s->row_bytes>s->stride ||
       (uint64_t)(s->rows-1)*s->stride+s->row_bytes>s->bytes)) return 0;
    /* A cached replacement bind does not read guest bytes. Validate on refresh. */
    if(!source_readable(s)) return 0;
    if(!g_sha && BCryptOpenAlgorithmProvider(&g_sha,BCRYPT_SHA256_ALGORITHM,NULL,0)<0) return 0;
    for(i=0;i<5;i++) for(j=0;j<4;j++) header[8+i*4+j]=(uint8_t)(fields[i]>>(8*j));
    if(BCryptCreateHash(g_sha,&hash,NULL,0,NULL,0,0)<0) return 0;
    status=BCryptHashData(hash,header,sizeof header,0);
    if(s->stride) {
        if(!s->rows || s->row_bytes>s->stride) status=(NTSTATUS)0xc000000d;
        for(i=0;status>=0 && i<s->rows;i++) status=BCryptHashData(hash,(PUCHAR)s->data+(size_t)i*s->stride,s->row_bytes,0);
        g_hashed+=(uint64_t)s->row_bytes*s->rows;
    } else { if(status>=0) status=BCryptHashData(hash,(PUCHAR)s->data,s->bytes,0); g_hashed+=s->bytes; }
    if(status>=0 && s->palette_entries) status=BCryptHashData(hash,(PUCHAR)s->palette,s->palette_entries*4,0);
    if(status>=0) status=BCryptFinishHash(hash,result,sizeof result,0);
    BCryptDestroyHash(hash);
    if(status<0) return 0;
    for(i=0;i<32;i++) sprintf(out+i*2,"%02x",result[i]);
    return 1;
}

static int wide_path(const char *path,WCHAR out[PATH_CAP])
{ return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,out,PATH_CAP)>0; }
static int write_png(const char *path,const uint8_t *bgra,uint32_t w,uint32_t h)
{
    IWICStream *stream=NULL; IWICBitmapEncoder *encoder=NULL; IWICBitmapFrameEncode *frame=NULL;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    WCHAR wide[PATH_CAP]; HRESULT hr=E_FAIL;
    if(!g_wic || !wide_path(path,wide)) return 0;
    hr=IWICImagingFactory_CreateStream(g_wic,&stream);
    if(SUCCEEDED(hr)) hr=IWICStream_InitializeFromFilename(stream,wide,GENERIC_WRITE);
    if(SUCCEEDED(hr)) hr=IWICImagingFactory_CreateEncoder(g_wic,&GUID_ContainerFormatPng,NULL,&encoder);
    if(SUCCEEDED(hr)) hr=IWICBitmapEncoder_Initialize(encoder,(IStream *)stream,WICBitmapEncoderNoCache);
    if(SUCCEEDED(hr)) hr=IWICBitmapEncoder_CreateNewFrame(encoder,&frame,NULL);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Initialize(frame,NULL);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetSize(frame,w,h);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetPixelFormat(frame,&format);
    if(SUCCEEDED(hr) && !IsEqualGUID(&format,&GUID_WICPixelFormat32bppBGRA)) hr=E_FAIL;
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_WritePixels(frame,h,w*4,w*h*4,(BYTE *)bgra);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Commit(frame);
    if(SUCCEEDED(hr)) hr=IWICBitmapEncoder_Commit(encoder);
    if(frame) IWICBitmapFrameEncode_Release(frame);
    if(encoder) IWICBitmapEncoder_Release(encoder);
    if(stream) IWICStream_Release(stream);
    return SUCCEEDED(hr);
}
static uint8_t *read_png(const char *path,uint32_t *w,uint32_t *h,int *retry)
{
    IWICBitmapDecoder *decoder=NULL; IWICBitmapFrameDecode *frame=NULL; IWICFormatConverter *converter=NULL;
    WCHAR wide[PATH_CAP]; uint8_t *data=NULL; HRESULT hr=E_FAIL;
    if(!g_wic || !wide_path(path,wide)) return NULL;
    hr=IWICImagingFactory_CreateDecoderFromFilename(g_wic,wide,NULL,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder);
    if(SUCCEEDED(hr)) hr=IWICBitmapDecoder_GetFrame(decoder,0,&frame);
    if(SUCCEEDED(hr)) hr=IWICBitmapFrameDecode_GetSize(frame,w,h);
    if(SUCCEEDED(hr) && (!*w || !*h || *w>16384 || *h>16384 || (uint64_t)*w**h*4>g_budget/4)) hr=E_INVALIDARG;
    if(SUCCEEDED(hr)) hr=IWICImagingFactory_CreateFormatConverter(g_wic,&converter);
    if(SUCCEEDED(hr)) hr=IWICFormatConverter_Initialize(converter,(IWICBitmapSource *)frame,&GUID_WICPixelFormat32bppBGRA,
                                  WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    if(SUCCEEDED(hr)) { data=malloc((size_t)*w**h*4); if(!data) hr=E_OUTOFMEMORY; }
    if(SUCCEEDED(hr)) hr=IWICFormatConverter_CopyPixels(converter,NULL,*w*4,*w**h*4,data);
    if(converter) IWICFormatConverter_Release(converter);
    if(frame) IWICBitmapFrameDecode_Release(frame);
    if(decoder) IWICBitmapDecoder_Release(decoder);
    if(FAILED(hr)) { free(data); data=NULL; *retry=(hr==E_OUTOFMEMORY); }
    return data;
}
static void release_entry(IDirect3DDevice8 *dev,PackEntry *e)
{
    unsigned stage;
    if(!e->texture) return;
    for(stage=0;stage<4;stage++) {
        IDirect3DBaseTexture8 *current=NULL;
        if(SUCCEEDED(dev->lpVtbl->GetTexture(dev,stage,&current)) && current) {
            current->lpVtbl->Release(current);
            if(current==(IDirect3DBaseTexture8 *)e->texture) dev->lpVtbl->SetTexture(dev,stage,NULL);
        }
    }
    e->texture->lpVtbl->Release(e->texture); e->texture=NULL;
    g_resident-=e->bytes; e->bytes=0;
}
static int make_room(IDirect3DDevice8 *dev,uint64_t need)
{
    IDirect3DBaseTexture8 *bound[4]={0};
    unsigned stage;
    if(need>g_budget) return 0;
    for(stage=0;stage<4;stage++) {
        if(SUCCEEDED(dev->lpVtbl->GetTexture(dev,stage,&bound[stage])) && bound[stage])
            bound[stage]->lpVtbl->Release(bound[stage]);
    }
    while(g_resident+need>g_budget) {
        PackEntry *old=NULL; size_t i;
        for(i=0;i<g_count;i++) {
            int in_use=0;
            for(stage=0;stage<4;stage++) if(bound[stage]==(IDirect3DBaseTexture8 *)g_entries[i].texture) in_use=1;
            if(g_entries[i].texture && !in_use && (!old || g_entries[i].used<old->used)) old=&g_entries[i];
        }
        if(!old) return 0;
        release_entry(dev,old);
    }
    return 1;
}
static int valid_scale(uint32_t w,uint32_t h,const RecompTextureSource *s)
{
    uint32_t scale=w/s->width;
    return scale>=1 && scale<=8 && w==s->width*scale && h==s->height*scale;
}
static int upload_png(IDirect3DDevice8 *dev,PackEntry *e,const RecompTextureSource *source)
{
    uint32_t w=0,h=0,physical_w,levels=1,dim,y,x,level;
    int retry=0;
    uint8_t *data=read_png(e->path,&w,&h,&retry),*padded=NULL;
    IDirect3DTexture8 *texture=NULL; uint64_t memory=0;
    if(!data) return retry ? -1 : 0;
    if(!valid_scale(w,h,source)) { free(data); return 0; }
    physical_w=source->row_texels*(w/source->width);
    if(physical_w<w || physical_w>16384 || (uint64_t)physical_w*h*4>g_budget/4) { free(data); return 0; }
    if(physical_w!=w) {
        padded=malloc((size_t)physical_w*h*4);
        if(!padded) { free(data); return -1; }
        for(y=0;y<h;y++) {
            memcpy(padded+(size_t)y*physical_w*4,data+(size_t)y*w*4,w*4);
            for(x=w;x<physical_w;x++) memcpy(padded+((size_t)y*physical_w+x)*4,data+((size_t)y*w+w-1)*4,4);
        }
        free(data); data=padded; w=physical_w;
    }
    for(dim=w>h?w:h;dim>1;dim>>=1) levels++;
    for(level=0;level<levels;level++) memory+=(uint64_t)(w>>level?w>>level:1)*(h>>level?h>>level:1)*8;
    if(memory>g_budget) { free(data); return 0; }
    if(!make_room(dev,memory) || FAILED(dev->lpVtbl->CreateTexture(dev,w,h,levels,0,D3DFMT_LIN_A8R8G8B8,0,&texture))) { free(data); return -1; }
    for(level=0;level<levels;level++) {
        D3DLOCKED_RECT lock={0}; uint32_t nw=w>1?w/2:1,nh=h>1?h/2:1;
        uint8_t *next=NULL;
        if(FAILED(texture->lpVtbl->LockRect(texture,level,&lock,NULL,0)) || !lock.pBits || lock.Pitch<(INT)(w*4)) goto fail;
        for(y=0;y<h;y++) memcpy((uint8_t *)lock.pBits+(size_t)y*lock.Pitch,data+(size_t)y*w*4,w*4);
        if(FAILED(texture->lpVtbl->UnlockRect(texture,level))) goto fail;
        if(level+1<levels) {
            next=malloc((size_t)nw*nh*4); if(!next) goto fail;
            for(y=0;y<nh;y++) for(x=0;x<nw;x++) {
                unsigned c,alpha=0,colour[3]={0},samples=0;
                uint32_t sx,sy,x0=x*w/nw,x1=(x+1)*w/nw,y0=y*h/nh,y1=(y+1)*h/nh;
                /* Include odd final rows/columns instead of discarding their
                 * artwork. Power-of-two textures retain the original 2x2 box. */
                for(sy=y0;sy<y1;sy++) for(sx=x0;sx<x1;sx++) {
                    const uint8_t *p=data+((size_t)sy*w+sx)*4;
                    samples++;
                    alpha+=p[3]; for(c=0;c<3;c++) colour[c]+=p[c]*(e->channel_mips ? 1 : p[3]);
                }
                for(c=0;c<3;c++) next[((size_t)y*nw+x)*4+c]=(uint8_t)(e->channel_mips ? (colour[c]+samples/2)/samples : alpha ? (colour[c]+alpha/2)/alpha : 0);
                next[((size_t)y*nw+x)*4+3]=(uint8_t)((alpha+samples/2)/samples);
            }
            free(data); data=next; w=nw; h=nh;
        }
    }
    free(data); e->texture=texture; e->bytes=memory; e->levels=levels; g_resident+=memory; return 1;
fail:
    free(data); if(texture) texture->lpVtbl->Release(texture); return -1;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static int upload_dds(IDirect3DDevice8 *dev,PackEntry *e,const RecompTextureSource *source)
{
    FILE *file=fopen(e->path,"rb"); uint8_t header[128],*data=NULL;
    uint32_t w,h,levels,fourcc,block,level,y;
    D3DFORMAT fmt; uint64_t total=0;
    IDirect3DTexture8 *texture=NULL; int retry=0;
    if(!file) return 0;
    if(fread(header,1,128,file)!=128 || memcmp(header,"DDS ",4) || le32(header+4)!=124 || le32(header+76)!=32) goto fail;
    h=le32(header+12); w=le32(header+16); levels=le32(header+28); fourcc=le32(header+84);
    if(!levels) levels=1;
    if(!w || !h || w>16384 || h>16384 || levels>15 || !valid_scale(w,h,source) || source->row_texels!=source->width || le32(header+112) || !(le32(header+80)&4) || le32(header+24)>1) goto fail;
    if(fourcc==0x31545844) { fmt=D3DFMT_DXT1; block=8; }
    else if(fourcc==0x33545844) { fmt=D3DFMT_DXT3; block=16; }
    else if(fourcc==0x35545844) { fmt=D3DFMT_DXT5; block=16; }
    else goto fail;
    for(level=0;level<levels;level++) {
        uint32_t lw=w>>level?w>>level:1,lh=h>>level?h>>level:1;
        if(level && (w>>(level-1))<=1 && (h>>(level-1))<=1) goto fail;
        total+=(uint64_t)((lw+3)/4)*((lh+3)/4)*block;
    }
    if(total>g_budget/4) goto fail;
    if(!make_room(dev,total*2)) { fclose(file); return -1; }
    data=malloc((size_t)total); if(!data) { retry=1; goto fail; }
    if(fread(data,1,(size_t)total,file)!=total || fgetc(file)!=EOF) goto fail;
    retry=1; /* Device/allocation failures can clear; malformed files cannot. */
    if(FAILED(dev->lpVtbl->CreateTexture(dev,w,h,levels,0,fmt,0,&texture))) goto fail;
    { uint64_t offset=0;
      for(level=0;level<levels;level++) {
        D3DLOCKED_RECT lock={0}; uint32_t lw=w>>level?w>>level:1,lh=h>>level?h>>level:1;
        uint32_t row=((lw+3)/4)*block,rows=(lh+3)/4;
        if(FAILED(texture->lpVtbl->LockRect(texture,level,&lock,NULL,0)) || !lock.pBits || lock.Pitch<(INT)row) goto fail;
        for(y=0;y<rows;y++) memcpy((uint8_t *)lock.pBits+(size_t)y*lock.Pitch,data+offset+(size_t)y*row,row);
        if(FAILED(texture->lpVtbl->UnlockRect(texture,level))) goto fail;
        offset+=(uint64_t)row*rows;
      }
    }
    fclose(file); free(data); e->texture=texture; e->bytes=total*2; e->levels=levels; g_resident+=e->bytes; return 1;
fail:
    fclose(file); free(data); if(texture) texture->lpVtbl->Release(texture); return retry ? -1 : 0;
}
static PackEntry *lookup(const char *id)
{
    size_t a=0,b=g_count;
    while(a<b) { size_t mid=a+(b-a)/2; if(strcmp(g_entries[mid].id,id)<=0) a=mid+1; else b=mid; }
    return a && !strcmp(g_entries[a-1].id,id) ? &g_entries[a-1] : NULL;
}
static unsigned source_slot(const RecompTextureSource *s)
{
    uintptr_t palette=(uintptr_t)s->palette;
    uint32_t key=s->address ^ s->format*0x9e3779b9u ^ (uint32_t)palette;
    /* Guest allocation alignment must not discard all useful address bits. */
#if UINTPTR_MAX > 0xffffffffu
    key^=(uint32_t)(palette>>32);
#endif
    key^=key>>16; key*=0x7feb352du;
    key^=key>>15; key*=0x846ca68bu; key^=key>>16;
    return key & (SOURCE_SLOTS-1);
}
int texture_pack_bind(IDirect3DDevice8 *dev,unsigned stage,const RecompTextureSource *s,RecompTextureSample sample,void *user)
{
    struct SourceState *state=NULL; char id[65]; PackEntry *entry;
    unsigned i, slot=source_slot(s);
    ULONGLONG now=GetTickCount64();
    if(!texture_pack_active() || !dev || stage>3) return 0;
    for(i=0;i<4;i++) {
        struct SourceState *c=&g_sources[(slot+i)&(SOURCE_SLOTS-1)];
        if(c->used && c->shape.address==s->address && c->shape.format==s->format && c->shape.width==s->width &&
           c->shape.height==s->height && c->shape.row_texels==s->row_texels && c->shape.palette==s->palette && c->shape.palette_entries==s->palette_entries) { state=c; break; }
    }
    if(state && now-state->checked<8) memcpy(id,state->id,65);
    else {
        if(!texture_pack_hash(s,id)) return 0;
        if(!state) {
            state=&g_sources[slot];
            for(i=0;i<4;i++) {
                struct SourceState *candidate=&g_sources[(slot+i)&(SOURCE_SLOTS-1)];
                if(!candidate->used) { state=candidate; break; }
                if(candidate->checked<state->checked) state=candidate;
            }
            state->dumped=0;
        }
        if(state->used && strcmp(state->id,id)) state->dumped=0;
        state->shape=*s; state->checked=now; memcpy(state->id,id,65); state->used=1;
    }
    if(!state->dumped && g_dump[0] && g_dumps<(uint64_t)g_dump_limit && sample && source_readable(s)) {
        state->dumped=1;
        char path[PATH_CAP]; snprintf(path,sizeof path,"%s/%s.png",g_dump,id);
        if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES) {
            uint32_t *pixels=malloc((size_t)s->width*s->height*4),x,y; int ok=1;
            if(pixels) {
                for(y=0;y<s->height && ok;y++) for(x=0;x<s->width;x++) if(!sample(user,x,y,&pixels[(size_t)y*s->width+x])) { ok=0; break; }
                if(ok && write_png(path,(const uint8_t *)pixels,s->width,s->height)) {
                    char meta[PATH_CAP]; FILE *log;
                    char decoded_id[65]="";
                    RecompTextureSource decoded={0};
                    decoded.format=0x12; decoded.width=decoded.row_texels=s->width; decoded.height=s->height;
                    decoded.data=pixels; decoded.bytes=s->width*s->height*4;
                    texture_pack_hash(&decoded,decoded_id);
                    snprintf(meta,sizeof meta,"%s/textures.jsonl",g_dump); log=fopen(meta,"ab");
                    if(log) { fprintf(log,"{\"id\":\"%s\",\"decoded_id\":\"%s\",\"format\":%u,\"width\":%u,\"height\":%u,\"row_texels\":%u,\"palette_entries\":%u,\"stage\":%u}\n",id,decoded_id,s->format,s->width,s->height,s->row_texels,s->palette_entries,stage); fclose(log); }
                    ++g_dumps;
                } else warn("RGBA dump failed",path);
                free(pixels);
            }
        }
    }
    entry=lookup(id); if(!entry) { ++g_misses; return 0; }
    if(entry->failed || now<entry->retry) return 0;
    if(!entry->texture) {
        const char *extension=strrchr(entry->path,'.');
        int loaded=extension && !_stricmp(extension,".dds") ? upload_dds(dev,entry,s) : upload_png(dev,entry,s);
        if(loaded<0) { entry->retry=now+1000; return 0; }
        if(!loaded) { entry->failed=1; warn("Replacement rejected; using original",entry->path); return 0; }
        if(g_loads++<32) fprintf(stderr,"[TEXPACK] loaded %s (%u mips, %llu bytes)\n",entry->id,entry->levels,(unsigned long long)entry->bytes);
    }
    entry->used=++g_clock; ++g_hits;
    if(FAILED(dev->lpVtbl->SetTexture(dev,stage,(IDirect3DBaseTexture8 *)entry->texture))) return 0;
    /* SetTexture leaves the title's combiners, addressing and filtering intact. */
    return 1;
}
void texture_pack_shutdown(IDirect3DDevice8 *dev)
{
    size_t i;
    if(g_active) fprintf(stderr,"[TEXPACK] hits=%llu misses=%llu dumps=%llu hashed=%llu resident=%llu\n",
        (unsigned long long)g_hits,(unsigned long long)g_misses,(unsigned long long)g_dumps,(unsigned long long)g_hashed,(unsigned long long)g_resident);
    if(dev) for(i=0;i<g_count;i++) release_entry(dev,&g_entries[i]);
    free(g_entries); g_entries=NULL; g_count=g_capacity=0;
    memset(g_sources,0,sizeof g_sources);
    if(g_wic) IWICImagingFactory_Release(g_wic); g_wic=NULL;
    if(g_sha) BCryptCloseAlgorithmProvider(g_sha,0); g_sha=NULL;
    if(g_com) CoUninitialize();
    g_initialized=g_active=g_com=0; g_dump[0]=0;
    g_resident=g_hits=g_misses=g_dumps=g_hashed=g_clock=0; g_source_next=0; g_loads=0;
}
#else
int texture_pack_active(void) { return 0; }
int texture_pack_hash(const RecompTextureSource *s,char out[65]) { (void)s; (void)out; return 0; }
int texture_pack_bind(IDirect3DDevice8 *d,unsigned st,const RecompTextureSource *s,RecompTextureSample cb,void *u)
{ (void)d;(void)st;(void)s;(void)cb;(void)u;return 0; }
void texture_pack_shutdown(IDirect3DDevice8 *d) { (void)d; }
#endif
