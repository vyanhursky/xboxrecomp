/* The production pack loader with this host's real PNG codec and SHA-256 (WIC and BCrypt on
 * Windows, stb and the portable one elsewhere), synthetic images and mock D3D8 resources. */
#include "../../src/nv2a/texture_pack.c"
#include <assert.h>
#if defined(_WIN32)
static int make_dir(const char *path) { return CreateDirectoryA(path,NULL); }
static void remove_file(const char *path) { DeleteFileA(path); }
static void remove_dir(const char *path) { RemoveDirectoryA(path); }
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
#if defined(_WIN32)
static void nap_ms(unsigned ms) { Sleep(ms); }
#else
static void nap_ms(unsigned ms) { usleep(ms*1000u); }
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
typedef struct ReplayPolicy { const char *id; int enabled, calls; } ReplayPolicy;
static int replay_source(const char *id,int offset,void *user)
{
    ReplayPolicy *policy=user;
    policy->calls++;
    return policy->enabled && offset==0 && !strcmp(id,policy->id);
}
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
static void sha_hex(const void *data,size_t bytes,size_t piece,char out[65])
{
    Sha sha; uint8_t digest[32]; size_t at; unsigned i;
    assert(sha_start(&sha));
    for(at=0;at<bytes;at+=piece) assert(sha_add(&sha,(const uint8_t *)data+at,bytes-at<piece ? bytes-at : piece));
    assert(sha_finish(&sha,digest));
    for(i=0;i<32;i++) sprintf(out+i*2,"%02x",digest[i]);
}
/* The host SHA-256 against FIPS 180-2 vectors, fed whole and in odd pieces (block buffering),
 * and, where the x86 SHA extensions are compiled in, their block function against the C one. */
static void check_sha256(void)
{
    static uint8_t million[1000000];
    char out[65];
    size_t pieces[]={1000000,1,63,64,65,4096+7};
    unsigned i;
    sha_hex("abc",3,3,out);
    assert(!strcmp(out,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha_hex("",0,1,out);
    assert(!strcmp(out,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",56,56,out);
    assert(!strcmp(out,"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    memset(million,'a',sizeof million);
    for(i=0;i<sizeof pieces/sizeof pieces[0];i++) {
        sha_hex(million,sizeof million,pieces[i],out);
        assert(!strcmp(out,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }
#if defined(TEXTURE_PACK_SHA_NI)
    if(sha_ni_available()) {
        uint32_t ni[8]={1,2,3,4,5,6,7,8},c[8]={1,2,3,4,5,6,7,8};
        for(i=0;i<sizeof million;i++) million[i]=(uint8_t)(i*2654435761u>>13);
        sha_blocks_ni(ni,million,sizeof million/64);
        for(i=0;i<sizeof million/64;i++) sha_block_c(c,million+i*64);
        assert(!memcmp(ni,c,sizeof ni));
        printf("texture-pack fixture: SHA-256 with the x86 SHA extensions\n");
    } else printf("texture-pack fixture: this CPU has no SHA extensions; C rounds only\n");
#endif
}
/* The cheap re-check between identity hashes sees what the identity sees, and nothing else. */
static void check_quick(void)
{
    static uint8_t rows[8*32],palette[64];
    RecompTextureSource s={1,11,16,8,16,rows,sizeof rows,32,16,8,palette,16};
    uint64_t a,b,again; unsigned i;
    for(i=0;i<sizeof rows;i++) rows[i]=(uint8_t)(i*7);
    for(i=0;i<sizeof palette;i++) palette[i]=(uint8_t)i;
    assert(texture_pack_quick(&s,&a) && texture_pack_quick(&s,&again) && a==again);
    rows[3*32+20]^=1;                       /* row padding: outside the identity too */
    assert(texture_pack_quick(&s,&b) && b==a);
    rows[3*32+5]^=1;                        /* a texel */
    assert(texture_pack_quick(&s,&b) && b!=a);
    rows[3*32+5]^=1; assert(texture_pack_quick(&s,&b) && b==a);
    palette[63]^=0x80;                      /* the palette */
    assert(texture_pack_quick(&s,&b) && b!=a);
    palette[63]^=0x80;
    rows[7*32+15]^=0x40;                    /* the last texel byte of the last row */  
    assert(texture_pack_quick(&s,&b) && b!=a);
    assert(same_layout(&s,&s));
    { RecompTextureSource moved=s; moved.data=rows+32; assert(!same_layout(&s,&moved)); }
    {
        void *blocked=no_access_page();
        RecompTextureSource hidden={1,6,2,2,2,blocked,16,0,0,0,NULL,0};
        assert(blocked && !texture_pack_quick(&hidden,&b));
        free_page(blocked);
    }
}
int main(void)
{
    check_sha256();
    check_quick();
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
#if !defined(_WIN32)
    {
        /* Ranges across pages: readable, readable then protected, protected in the middle.
         * Pages are the host's (16 KiB on Apple Silicon). */
        size_t pg=(size_t)sysconf(_SC_PAGESIZE);
        uint8_t *pages=mmap(NULL,4*pg,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        assert(pages!=MAP_FAILED);
        memset(pages,7,4*pg);
        assert(mprotect(pages+2*pg,pg,PROT_NONE)==0);
        assert(readable(pages+100,2*pg-100) && readable(pages+pg-1,2));
        assert(!readable(pages+pg,pg+1) && !readable(pages+100,4*pg-100) && !readable(pages+2*pg,1));
        assert(readable(pages+3*pg,pg) && !readable(pages+pg,3*pg));
        assert(munmap(pages,4*pg)==0);
    }
#endif
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
    /* A list longer than the old 256-character buffer reads every pack in it; one longer than the
     * loader holds reads the whole names that fit. */
    {
        char list[2048]="",name[64],pack_id[65];
        unsigned count=7;
        for(i=0;i<count;i++) {
            snprintf(name,sizeof name,"faithful-hd-0123456789a%u-opacity-with-a-longer-name",i);
            snprintf(path,sizeof path,"%s/%s",root,name); assert(make_dir(path));
            snprintf(manifest,sizeof manifest,"%s/manifest.ini",path);
            memcpy(pack_id,id,65); pack_id[0]="0123456789abcdef"[i];
            file=fopen(manifest,"wb"); assert(file);
            fprintf(file,"[pack]\nschema=1\n[textures]\n%s=textures/p%u.png\n",pack_id,i); fclose(file);
            if(i) strcat(list,";");
            strcat(list,name);
        }
        assert(strlen(list)>256);
        g_count=0; g_errors=0; read_packs(root,list);
        assert(g_count==count && g_errors==0);
        for(i=0;i<count;i++) assert(strstr(g_entries[i].path,"textures/p") && g_entries[i].path[strlen(g_entries[i].path)-5]==(char)('0'+i));
        while(strlen(list)<PACK_LIST_CAP+40) strcat(list,";faithful-hd-0123456789a0-opacity-with-a-longer-name");
        g_count=0; read_packs(root,list);
        assert(g_count>count && g_errors==0);   /* the cut name is dropped, not read as a bad name */
        g_count=0;
        for(i=0;i<count;i++) {
            snprintf(path,sizeof path,"%s/faithful-hd-0123456789a%u-opacity-with-a-longer-name/manifest.ini",root,i); remove_file(path);
            snprintf(path,sizeof path,"%s/faithful-hd-0123456789a%u-opacity-with-a-longer-name",root,i); remove_dir(path);
        }
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
    /* [wide]: one anchor's slabs in order with an inner slab, a stretch overlay, a line with a
     * malformed item kept only up to it; slab entries are never returned by lookup(). */
    {
        static const char third[]="abababababababababababababababababababababababababababababababab";
        static const char overlay[]="cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
        int dt[16]; PackEntry *slab;
        snprintf(path,sizeof path,"%s/wide",root); make_dir(path);
        snprintf(path,sizeof path,"%s/wide/slabs",root); make_dir(path);
        snprintf(manifest,sizeof manifest,"%s/wide/manifest.ini",root);
        file=fopen(manifest,"wb"); assert(file);
        fprintf(file,"[pack]\nschema=1\n[wide]\n%s=-4:slabs/m4.png;inner:slabs/in.png;4:slabs/p4.png\n%s=stretch\n%s=1:slabs/ok.png;2:bad+name.png\n",
                id,overlay,third);
        fclose(file);
        read_pack(root,"wide"); qsort(g_entries,g_count,sizeof(*g_entries),entry_compare); index_wide();
        assert(g_count==6 && g_wide_count==2 && g_stretch_count==1);
        assert(strstr(lookup(id)->path,"second.png") && !lookup(overlay));
        memcpy(g_stage_id[0],id,65);
        assert(texture_pack_wide_slabs(0,dt,16)==3 && dt[0]==-4 && dt[1]==0 && dt[2]==4 && !texture_pack_wide_stretch(0));
        assert(wide_anchor(0)->inner && strstr(wide_anchor(0)->inner->path,"in.png"));
        memcpy(g_stage_id[0],overlay,65);
        assert(texture_pack_wide_stretch(0) && !texture_pack_wide_slabs(0,dt,16));
        memcpy(g_stage_id[0],third,65);
        assert(texture_pack_wide_slabs(0,dt,16)==1 && dt[0]==1);
        /* A slab loads with the anchor's shape (2x here) and binds; a wrongly sized one is rejected. */
        memcpy(g_stage_id[0],id,65);
        g_stage_shape[0]=(RecompTextureSource){1,6,1,1,1,pixel,4,0,0,0,NULL,0};
        snprintf(path,sizeof path,"%s/wide/slabs/m4.png",root); assert(write_png(path,pixel,2,2));
        snprintf(path,sizeof path,"%s/wide/slabs/in.png",root); assert(write_png(path,pixel,2,2));
        snprintf(path,sizeof path,"%s/wide/slabs/p4.png",root); assert(write_png(path,pixel,2,1));
        g_sync_load=1; g_budget=1u<<20;
        assert(texture_pack_wide_bind(&device,0,0)==1 && bound[0]);
        {
            IDirect3DTexture8 *source;
            IDirect3DBaseTexture8 *side=bound[0];
            ReplayPolicy policy={id,1,0};
            unsigned before=creates;
            assert(SUCCEEDED(create_texture(&device,1,1,1,0,D3DFMT_A8R8G8B8,0,&source)));
            /* Selection must bind the saved source, including a caller's original-texture
             * fallback, without decoding the zero-offset manifest image. */
            texture_pack_set_wide_replay_filter(replay_source,&policy);
            assert(texture_pack_wide_bind_source(&device,0,1,(IDirect3DBaseTexture8 *)source));
            assert(bound[0]==(IDirect3DBaseTexture8 *)source && creates==before+1);
            assert(texture_pack_wide_bind_source(&device,0,0,(IDirect3DBaseTexture8 *)source));
            assert(bound[0]==side);  /* nonzero offsets retain their manifest slab */
            assert(texture_pack_wide_bind_source(&device,0,1,(IDirect3DBaseTexture8 *)source));
            assert(bound[0]==(IDirect3DBaseTexture8 *)source); /* restore after a different slab */
            assert(!texture_pack_wide_bind_source(&device,0,1,NULL));
            assert(bound[0]==(IDirect3DBaseTexture8 *)source);
            policy.enabled=0;
            assert(texture_pack_wide_bind_source(&device,0,1,(IDirect3DBaseTexture8 *)source));
            assert(bound[0]!=(IDirect3DBaseTexture8 *)source && bound[0]!=side);
            policy.id=third; policy.enabled=1;
            assert(texture_pack_wide_bind_source(&device,0,1,(IDirect3DBaseTexture8 *)source));
            assert(bound[0]!=(IDirect3DBaseTexture8 *)source); /* unrelated identities retain their slab */
            before=policy.calls;
            assert(!texture_pack_wide_bind_source(&device,4,0,(IDirect3DBaseTexture8 *)source));
            assert(!texture_pack_wide_bind_source(&device,0,99,(IDirect3DBaseTexture8 *)source));
            assert(!texture_pack_wide_bind_source(NULL,0,0,(IDirect3DBaseTexture8 *)source));
            assert(policy.calls==before);
            texture_pack_set_wide_replay_filter(NULL,NULL);
            assert(texture_pack_wide_bind_source(&device,0,1,(IDirect3DBaseTexture8 *)source));
            assert(bound[0]!=(IDirect3DBaseTexture8 *)source);
            assert(texture_pack_wide_bind_source(&device,0,0,(IDirect3DBaseTexture8 *)source));
            source->lpVtbl->Release(source);
        }
        assert(texture_pack_wide_bind(&device,0,2)==0);
        slab=&g_entries[wide_anchor(0)->first+2]; assert(slab->failed);
        slab=&g_entries[wide_anchor(0)->first]; set_texture(&device,0,NULL); release_entry(&device,slab);
        g_sync_load=0; g_stage_id[0][0]=0;
        snprintf(path,sizeof path,"%s/wide/slabs/m4.png",root); remove_file(path);
        snprintf(path,sizeof path,"%s/wide/slabs/in.png",root); remove_file(path);
        snprintf(path,sizeof path,"%s/wide/slabs/p4.png",root); remove_file(path);
        remove_file(manifest);
        snprintf(path,sizeof path,"%s/wide/slabs",root); remove_dir(path);
        snprintf(path,sizeof path,"%s/wide",root); remove_dir(path);
    }
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
    /* The background decoder: pending first, then the same texture the synchronous path builds;
     * a missing file is rejected for good; a decode nobody collects is dropped. */
    {
        int result, spins; MockTexture *async_texture;
        g_budget=1u<<20;
        snprintf(path,sizeof path,"%s/alpha.png",root); assert(write_png(path,pixel,2,2));
        snprintf(g_entries[0].path,PATH_CAP,"%s",path);
        source=(RecompTextureSource){1,6,2,2,2,pixel,16,0,0,0,NULL,0};
        assert(upload_png_async(&device,&g_entries[0],&source,0)==-2 && g_entries[0].job);
        for(spins=0;spins<2000;spins++) {
            result=upload_png_async(&device,&g_entries[0],&source,0);
            if(result!=-2) break;
            nap_ms(1);
        }
        assert(spins<2000 && result==1 && !g_entries[0].job);
        async_texture=(MockTexture *)g_entries[0].texture;
        assert(async_texture->levels==2 && !memcmp(async_texture->mips[0],pixel,16));
        assert(async_texture->mips[1][0]==0 && async_texture->mips[1][2]==255 && async_texture->mips[1][3]==64);
        release_entry(&device,&g_entries[0]);
        snprintf(g_entries[0].path,PATH_CAP,"%s/missing.png",root);
        assert(upload_png_async(&device,&g_entries[0],&source,0)==-2);
        for(spins=0;spins<2000;spins++) {
            result=upload_png_async(&device,&g_entries[0],&source,0);
            if(result!=-2) break;
            nap_ms(1);
        }
        assert(spins<2000 && result==0 && !g_entries[0].texture && !g_entries[0].job);
        snprintf(g_entries[0].path,PATH_CAP,"%s",path);
        assert(upload_png_async(&device,&g_entries[0],&source,0)==-2);
        nap_ms(200);                                  /* decoded, never collected */
        jobs_reap(now_ms()+LOAD_STALE_MS+1);
        assert(!g_entries[0].job);
        assert(upload_png_async(&device,&g_entries[0],&source,0)==-2);   /* asked for again */
        for(spins=0;spins<2000 && upload_png_async(&device,&g_entries[0],&source,0)==-2;spins++) nap_ms(1);
        release_entry(&device,&g_entries[0]);
    }
    texture_pack_set_wide_replay_filter(replay_source, NULL);
    texture_pack_shutdown(&device);
    assert(!g_wide_replay_filter && !g_wide_replay_user);
    {
        uint8_t hd[8*8*4];unsigned n;
        RecompTextureSource live={0};
        _putenv_s("RECOMP_TEXTURE_PACKS","");_putenv_s("RECOMP_TEXTURE_DUMP_DIR","");
        for(n=0;n<64;n++){hd[n*4]=10;hd[n*4+1]=20;hd[n*4+2]=30;hd[n*4+3]=(uint8_t)(n&1?219:133);}
        assert(!texture_pack_active());
        assert(!texture_pack_memory_register(0,123,6,4,4,hd,8,8));
        assert(!texture_pack_memory_register(1,123,6,4,4,hd,9,8));
        assert(texture_pack_memory_register(1,123,6,4,4,hd,8,8));
        assert(!texture_pack_memory_register(2,123,6,4,4,hd,8,8));
        assert(texture_pack_active());
        live.address=124;live.format=6;live.width=live.height=live.row_texels=4;
        assert(!texture_pack_bind(&device,0,&live,NULL,NULL)); /* Identical content, different source. */
        live.address=123;live.format=0x12;
        assert(!texture_pack_bind(&device,0,&live,NULL,NULL));
        live.format=6;
        assert(texture_pack_bind(&device,0,&live,NULL,NULL));
        texture=(MockTexture *)bound[0];assert(texture->width==8 && texture->height==8);
        assert(!memcmp(texture->mips[0],hd,sizeof hd));
        assert(texture->mips[1][0]==10 && texture->mips[1][3]==176);
        texture_pack_memory_clear(2);assert(texture_pack_bind(&device,0,&live,NULL,NULL));
        texture_pack_memory_clear(1);assert(!texture_pack_bind(&device,0,&live,NULL,NULL));
        assert(!g_memory_bytes && !g_memory_count && !bound[0]);
        /* Repeated unload/reuse before another draw cannot exhaust the 32 slots. */
        for(n=0;n<64;n++) {
            assert(texture_pack_memory_register(1,123,6,4,4,hd,8,8));
            texture_pack_memory_clear(1);
        }
        assert(texture_pack_memory_register(3,123,6,4,4,hd,8,8));
        assert(texture_pack_bind(&device,0,&live,NULL,NULL));
        texture_pack_shutdown(&device);
        assert(!g_memory_count && !g_memory_bytes && !bound[0]);
    }

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
    printf("texture-pack fixture passed: hash, palette, stride, PNG alpha/mips, background decoding, DDS bounds, long pack lists, precedence, wide slabs, eviction (%u resources)\n",creates);
    return 0;
}
