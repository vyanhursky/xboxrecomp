/* The production pack loader with this host's real PNG codec and SHA-256 (WIC and BCrypt on
 * Windows, stb and the portable one elsewhere), synthetic images and mock D3D8 resources. */
#include "../../src/nv2a/texture_pack.c"
#include <assert.h>
#if defined(_WIN32)
static int make_dir(const char *path) { return make_dir(path); }
static void remove_file(const char *path) { remove_file(path); }
static void remove_dir(const char *path) { remove_dir(path); }
static void *no_access_page(void) { return VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS); }
static void free_page(void *page) { assert(VirtualFree(page,0,MEM_RELEASE)); }
static void temp_root(char *root,size_t size)
{
    char base[1024]; GetTempPathA(sizeof base,base);
    snprintf(root,size,"%stexture-pack-fixture-%lu",base,GetCurrentProcessId());
}
#else
static int make_dir(const char *path) { return mkdir(path,0777)==0; }
static void remove_file(const char *path) { unlink(path); }
static void remove_dir(const char *path) { rmdir(path); }
static void *no_access_page(void)
{
    void *page=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    return page==MAP_FAILED ? NULL : page;
}
static void free_page(void *page) { assert(munmap(page,4096)==0); }
static void temp_root(char *root,size_t size)
{
    const char *base=getenv("TMPDIR");
    snprintf(root,size,"%s/texture-pack-fixture-%ld",base && *base ? base : "/tmp",(long)getpid());
}
#define _putenv_s(name,value) setenv(name,value,1)
#endif
typedef struct MockTexture {
    IDirect3DTexture8 iface;
    unsigned refs, levels, uploads;
    uint32_t width, height, block;
    uint8_t *mips[15];
} MockTexture;
static IDirect3DTexture8Vtbl texture_vtable;
static IDirect3DDevice8Vtbl device_vtable;
static IDirect3DDevice8 device={&device_vtable};
static IDirect3DBaseTexture8 *bound[4];
static unsigned creates, destroys, fail_creates;
static ULONG __stdcall addref(IDirect3DTexture8 *iface) { return ++((MockTexture *)iface)->refs; }
static ULONG __stdcall release(IDirect3DTexture8 *iface)
{
    MockTexture *t=(MockTexture *)iface; unsigned i;
    if(--t->refs) return t->refs;
    for(i=0;i<t->levels;i++) free(t->mips[i]);
    destroys++; free(t); return 0;
}
static HRESULT __stdcall lock_rect(IDirect3DTexture8 *iface,UINT level,D3DLOCKED_RECT *out,const RECT *rect,DWORD flags)
{
    MockTexture *t=(MockTexture *)iface; uint32_t w=t->width>>level;
    (void)rect; (void)flags;
    if(level>=t->levels) return E_FAIL;
    if(!w) w=1;
    out->Pitch=t->block ? ((w+3)/4)*t->block : w*4;
    out->pBits=t->mips[level]; return S_OK;
}
static HRESULT __stdcall unlock_rect(IDirect3DTexture8 *iface,UINT level)
{ (void)level; ((MockTexture *)iface)->uploads++; return S_OK; }
static HRESULT __stdcall set_texture(IDirect3DDevice8 *dev,DWORD stage,IDirect3DBaseTexture8 *texture)
{
    (void)dev;
    if(texture) texture->lpVtbl->AddRef(texture);
    if(bound[stage]) bound[stage]->lpVtbl->Release(bound[stage]);
    bound[stage]=texture; return S_OK;
}
static HRESULT __stdcall get_texture(IDirect3DDevice8 *dev,DWORD stage,IDirect3DBaseTexture8 **out)
{ (void)dev; *out=bound[stage]; if(*out) (*out)->lpVtbl->AddRef(*out); return S_OK; }
static HRESULT __stdcall create_texture(IDirect3DDevice8 *dev,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT fmt,D3DPOOL pool,IDirect3DTexture8 **out)
{
    MockTexture *t; unsigned i;
    if(fail_creates) { fail_creates--; *out=NULL; return E_OUTOFMEMORY; }
    t=calloc(1,sizeof(*t));
    (void)dev; (void)usage; (void)pool;
    assert(t && levels<=15);
    t->iface.lpVtbl=&texture_vtable; t->refs=1; t->levels=levels; t->width=w; t->height=h;
    t->block=fmt==D3DFMT_DXT1 ? 8 : (fmt==D3DFMT_DXT3 || fmt==D3DFMT_DXT5) ? 16 : 0;
    for(i=0;i<levels;i++) {
        uint32_t lw=w>>i?w>>i:1,lh=h>>i?h>>i:1;
        t->mips[i]=calloc(t->block ? ((lw+3)/4)*((lh+3)/4)*t->block : lw*lh*4,1);
        assert(t->mips[i]);
    }
    creates++; *out=&t->iface; return S_OK;
}
static void put32(uint8_t *p,uint32_t n) { unsigned i; for(i=0;i<4;i++) p[i]=(uint8_t)(n>>(8*i)); }
int main(void)
{
    uint8_t raw[128],palette[16],changed[128],pixel[16]={0,0,255,255, 255,0,0,0, 255,0,0,0, 255,0,0,0};
    char id[65],other[65],root[1024],path[1024],manifest[1024];
    RecompTextureSource source={1,11,16,8,16,raw,128,0,0,0,palette,4};
    PackEntry entry={0}; FILE *file; unsigned i;
    MockTexture *texture;
    texture_vtable.AddRef=addref; texture_vtable.Release=release;
    texture_vtable.LockRect=lock_rect; texture_vtable.UnlockRect=unlock_rect;
    device_vtable.CreateTexture=create_texture; device_vtable.SetTexture=set_texture; device_vtable.GetTexture=get_texture;
    for(i=0;i<128;i++) raw[i]=(uint8_t)i;
    for(i=0;i<16;i++) palette[i]=(uint8_t)i;
    assert(texture_pack_hash(&source,id));
    /* Cross-language vector computed independently in the Python fixture. */
    assert(!strcmp(id,"ac558aaf6df46f5ec44093564bbca980e2dc08555af1095551b2faf57d4b1009"));
    source.address=99; assert(texture_pack_hash(&source,other) && !strcmp(id,other));
    memcpy(changed,raw,128); changed[127]^=1; source.data=changed;
    assert(texture_pack_hash(&source,other) && strcmp(id,other));
    source.data=raw; palette[15]^=1; assert(texture_pack_hash(&source,other) && strcmp(id,other));
    palette[15]^=1;
    source.stride=16; source.row_bytes=8; source.rows=8;
    assert(texture_pack_hash(&source,id)); changed[15]^=1; source.data=changed;
    changed[127]=raw[127]; assert(texture_pack_hash(&source,other) && !strcmp(id,other));
    source.bytes=1; assert(!texture_pack_hash(&source,other));
    {
        void *blocked=no_access_page();
        RecompTextureSource inaccessible={1,6,2,2,2,blocked,16,0,0,0,NULL,0};
        assert(blocked && !texture_pack_hash(&inaccessible,other));
        inaccessible.data=pixel; inaccessible.palette=blocked; inaccessible.palette_entries=1;
        assert(!texture_pack_hash(&inaccessible,other));
        free_page(blocked);
    }
    assert(!valid_name("../escape.png",1) && !valid_name("C:/escape.png",1) && !valid_name("a\\b.png",1));
    assert(valid_name("textures/clean.png",1));
    {
        unsigned char slots[SOURCE_SLOTS]={0}; unsigned unique=0;
        RecompTextureSource aligned=source;
        aligned.palette=NULL;
        for(i=0;i<128;i++) {
            unsigned slot;
            aligned.address=0x02000000u+i*4096; slot=source_slot(&aligned);
            if(!slots[slot]) { slots[slot]=1; unique++; }
        }
        /* Previously all 128 page-aligned sources competed for the same 4 slots. */
        assert(unique>96);
    }
    temp_root(root,sizeof root); assert(make_dir(root));
    _putenv_s("RECOMP_TEXTURE_DUMP_DIR",""); _putenv_s("RECOMP_TEXTURE_PACKS","");
    initialize();
#if defined(_WIN32)
    assert(SUCCEEDED(CoInitializeEx(NULL,COINIT_MULTITHREADED)));
    assert(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&g_wic)));
#endif
    snprintf(path,sizeof path,"%s/alpha.png",root); assert(write_png(path,pixel,2,2));
    source=(RecompTextureSource){1,6,2,2,2,pixel,16,0,0,0,NULL,0};
    snprintf(entry.path,sizeof entry.path,"%s",path);
    assert(upload_png(&device,&entry,&source));
    texture=(MockTexture *)entry.texture;
    assert(texture->levels==2 && texture->uploads==2);
    assert(!memcmp(texture->mips[0],pixel,16));
    assert(texture->mips[1][0]==0 && texture->mips[1][2]==255 && texture->mips[1][3]==64);
    for(i=0;i<4;i++) set_texture(&device,i,(IDirect3DBaseTexture8 *)entry.texture);
    release_entry(&device,&entry); for(i=0;i<4;i++) assert(!bound[i]);
    assert(g_resident==0);
    /* Material/data alpha must not suppress RGB when constructing lower mips. */
    entry.channel_mips=1;
    assert(upload_png(&device,&entry,&source));
    texture=(MockTexture *)entry.texture;
    assert(!memcmp(texture->mips[0],pixel,16));
    assert(texture->mips[1][0]==191 && texture->mips[1][1]==0 && texture->mips[1][2]==64 && texture->mips[1][3]==64);
    release_entry(&device,&entry);
    entry.channel_mips=0;
    {
        uint8_t odd[12]={0,0,0,255, 0,0,0,255, 255,0,0,255};
        assert(write_png(path,odd,3,1));
        source.width=source.row_texels=3; source.height=1;
        fail_creates=1; assert(upload_png(&device,&entry,&source)==-1 && !entry.texture);
        assert(upload_png(&device,&entry,&source)==1);
        texture=(MockTexture *)entry.texture;
        assert(texture->levels==2 && texture->mips[1][0]==85 && texture->mips[1][3]==255);
        release_entry(&device,&entry);
        assert(write_png(path,pixel,2,2)); source.width=source.height=source.row_texels=2;
    }
    {
        uint8_t material[16]={0,0,255,255, 255,0,0,1, 0,255,0,128, 255,255,255,64};
        assert(write_png(path,material,2,2));
        assert(upload_png(&device,&entry,&source));
        texture=(MockTexture *)entry.texture;
        assert(texture->mips[1][0]==37 && texture->mips[1][1]==109 && texture->mips[1][2]==182 && texture->mips[1][3]==112);
        release_entry(&device,&entry); entry.channel_mips=1;
        assert(upload_png(&device,&entry,&source));
        texture=(MockTexture *)entry.texture;
        assert(texture->mips[1][0]==128 && texture->mips[1][1]==128 && texture->mips[1][2]==128 && texture->mips[1][3]==112);
        release_entry(&device,&entry); entry.channel_mips=0;
        assert(write_png(path,pixel,2,2));
    }
    source.row_texels=4; assert(upload_png(&device,&entry,&source));
    texture=(MockTexture *)entry.texture;
    assert(texture->width==4 && texture->height==2 && !memcmp(texture->mips[0]+8,pixel+4,4));
    release_entry(&device,&entry);
    source.width=3; assert(!upload_png(&device,&entry,&source));
    /* DDS with complete BC1 mip chain and rejected truncation. */
    {
        uint8_t dds[152]={0};
        memcpy(dds,"DDS ",4); put32(dds+4,124); put32(dds+12,4); put32(dds+16,4);
        put32(dds+28,3); put32(dds+76,32); put32(dds+80,4); memcpy(dds+84,"DXT1",4);
        snprintf(entry.path,sizeof entry.path,"%s/test.dds",root);
        file=fopen(entry.path,"wb"); assert(file); fwrite(dds,1,sizeof dds,file); fclose(file);
        source.width=source.height=source.row_texels=4;
        fail_creates=1; assert(upload_dds(&device,&entry,&source)==-1 && !entry.texture);
        assert(upload_dds(&device,&entry,&source));
        assert(((MockTexture *)entry.texture)->uploads==3);
        release_entry(&device,&entry);
        file=fopen(entry.path,"wb"); fwrite(dds,1,151,file); fclose(file);
        assert(!upload_dds(&device,&entry,&source));
        remove_file(entry.path);
    }
    snprintf(path,sizeof path,"%s/one",root); make_dir(path);
    snprintf(manifest,sizeof manifest,"%s/one/manifest.ini",root);
    file=fopen(manifest,"wb"); assert(file);
    fprintf(file,"[pack]\nschema=1\n[textures]\n%s=textures/first.png\n",id); fclose(file);
    read_pack(root,"one");
    snprintf(path,sizeof path,"%s/two",root); make_dir(path);
    snprintf(manifest,sizeof manifest,"%s/two/manifest.ini",root);
    file=fopen(manifest,"wb"); fprintf(file,"[pack]\nschema=1\n[textures]\n%s=textures/second.png\n[pack]\npng_mips=channels\n",id); fclose(file);
    read_pack(root,"two"); qsort(g_entries,g_count,sizeof(*g_entries),entry_compare);
    assert(g_count==2 && strstr(lookup(id)->path,"second.png"));
    assert(lookup(id)->channel_mips==1 && g_entries[0].channel_mips==0);
    file=fopen(manifest,"wb"); fprintf(file,"[pack]\nschema=1\npng_mips=invalid\n[textures]\n%s=textures/rejected.png\n",id); fclose(file);
    read_pack(root,"two"); assert(g_count==2 && strstr(lookup(id)->path,"second.png"));
    /* LRU eviction must unbind all stages and release every retained resource. */
    g_budget=64; g_active=1;
    snprintf(g_entries[0].path,PATH_CAP,"%s/alpha.png",root);
    source.width=source.height=source.row_texels=2;
    assert(upload_png(&device,&g_entries[0],&source));
    for(i=0;i<4;i++) set_texture(&device,i,(IDirect3DBaseTexture8 *)g_entries[0].texture);
    assert(!make_room(&device,64));
    for(i=0;i<4;i++) { assert(bound[i]); set_texture(&device,i,NULL); }
    assert(make_room(&device,64));
    for(i=0;i<4;i++) assert(!bound[i]);
    assert(!g_entries[0].texture && g_resident==0);
    texture_pack_shutdown(&device);
#if defined(_WIN32)
    CoUninitialize();
#endif
    assert(creates==destroys);
    snprintf(path,sizeof path,"%s/alpha.png",root); remove_file(path);
    for(i=0;i<2;i++) {
        snprintf(path,sizeof path,"%s/%s/manifest.ini",root,i?"two":"one"); remove_file(path);
        snprintf(path,sizeof path,"%s/%s",root,i?"two":"one"); remove_dir(path);
    }
    remove_dir(root);
    printf("texture-pack fixture passed: hash, palette, stride, PNG alpha/mips, DDS bounds, precedence, eviction (%u resources)\n",creates);
    return 0;
}
