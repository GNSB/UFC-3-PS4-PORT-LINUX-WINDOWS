/* UFC-82: the .trp parser on a synthetic file, trophies.json across a restart, the game's info
 * structures, and names/platinum through a (stubbed) decrypted TROP.ESFM. No game data. */
#define _GNU_SOURCE
#include "runtime_trophy.h"
#include "platform/platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* bbgpu stubs: the toast is recorded; "decryption" copies (the test's ESFM is plain XML). */
static int toast_id=-2, toast_count; static char toast_name[128]; static const void *toast_png; static uint64_t toast_png_size;
void bbgpu_trophy_toast(int32_t id,const char *name,const void *png,uint64_t png_size) {
    toast_id=id; ++toast_count; snprintf(toast_name,sizeof toast_name,"%s",name ? name : ""); toast_png=png; toast_png_size=png_size;
}
static int decrypt_calls;
int bbgpu_trophy_decrypt(const uint8_t key[16],const char np_comm_id[16],const uint8_t iv[16],const uint8_t *in,uint64_t size,uint8_t *out) {
    (void)iv; ++decrypt_calls;
    assert(key[0]==0x01 && key[15]==0xEF);
    assert(!strcmp(np_comm_id,"NPWR12345_00"));
    memcpy(out,in,size);
    return 1;
}

static void put_be32(uint8_t *p,uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void put_be64(uint8_t *p,uint64_t v) { put_be32(p,(uint32_t)(v>>32)); put_be32(p+4,(uint32_t)v); }

/* Header 0x60, 4 entries of 0x40, then the payloads. */
static const char *names[4]={"TROP000.PNG","TROP001.PNG","TROP002.PNG","TROP.ESFM"};
static size_t build_trp(uint8_t *out,size_t room) {
    static const char xml[]=
        "<?xml version=\"1.0\"?><trophyconf version=\"1.0\"><title-name>Test &amp; Title</title-name>"
        "<trophy id=\"000\" hidden=\"no\" ttype=\"B\" pid=\"002\"><name>Knockout</name><detail>Win</detail></trophy>"
        "<trophy id=\"001\" hidden=\"yes\" ttype=\"B\" pid=\"002\"><name>Submission</name><detail>Tap</detail></trophy>"
        "<trophy id=\"002\" hidden=\"no\" ttype=\"P\"><name>Champion</name><detail>All</detail></trophy>"
        "</trophyconf>";
    memset(out,0,room);
    size_t at=0x60+4*0x40;
    for (int i=0;i<4;++i) {
        uint8_t *e=out+0x60+i*0x40;
        size_t length;
        memcpy(e,names[i],strlen(names[i]));
        if (i<3) { length=16; memcpy(out+at,"\x89PNG\r\n\x1a\nicon",12); out[at+12]=(uint8_t)('0'+i); }
        else {
            length=16+((sizeof(xml)+15)/16)*16; /* IV + whole blocks */
            memcpy(out+at+16,xml,sizeof xml);
        }
        put_be64(e+32,at); put_be64(e+40,length); put_be32(e+48,i<3 ? 0 : 3);
        at+=length;
    }
    put_be32(out,TRP_MAGIC); put_be32(out+4,3); put_be64(out+8,at); put_be32(out+0x10,4); put_be32(out+0x14,0x40);
    assert(at<=room);
    return at;
}

static void write_file(const char *path,const void *data,size_t size) {
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(data,1,size,f)==size); fclose(f);
}
static char *read_file(const char *path) {
    static char text[8192];
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    size_t n=fread(text,1,sizeof text-1,f); text[n]=0; fclose(f);
    return text;
}

int main(void) {
    static uint8_t trp[4096];
    size_t size=build_trp(trp,sizeof trp);

    /* Parser. */
    TrpEntry entries[8];
    assert(runtime_trp_parse(trp,size,entries,8)==4);
    for (int i=0;i<4;++i) assert(!strcmp(entries[i].name,names[i]));
    assert(entries[0].offset==0x160 && entries[0].length==16 && entries[0].flag==0 && entries[3].flag==3);
    assert(!memcmp(trp+entries[2].offset,"\x89PNG",4) && trp[entries[2].offset+12]=='2');
    assert(runtime_trp_find(entries,4,"trop001.png")==&entries[1] && !runtime_trp_find(entries,4,"TROP003.PNG"));
    assert(runtime_trp_parse(trp,0x80,entries,8)==-1);           /* entry table cut off */
    assert(runtime_trp_parse(trp,size-1,entries,8)==-1);         /* last payload cut off */
    trp[0]^=1; assert(runtime_trp_parse(trp,size,entries,8)==-1); trp[0]^=1;
    puts("PASS: TRP parser (synthetic file, truncation, magic)");

    char dir[]="/tmp/bb-trophy-test-XXXXXX";
    assert(mkdtemp(dir));
    char trp_path[256], json[256], json_tmp[260], ini[256], npbind[256], keys[256];
    snprintf(trp_path,sizeof trp_path,"%s/trophy00.trp",dir);
    snprintf(json,sizeof json,"%s/trophies.json",dir);
    snprintf(json_tmp,sizeof json_tmp,"%s.tmp",json);
    snprintf(ini,sizeof ini,"%s/bbport.ini",dir);
    snprintf(npbind,sizeof npbind,"%s/npbind.dat",dir);
    snprintf(keys,sizeof keys,"%s/keys.json",dir);
    write_file(trp_path,trp,size);

    /* Without a key: numbers only; unlocks persist across a restart. */
    runtime_trophy_configure(trp_path,NULL,json,NULL,keys);
    assert(decrypt_calls==0);
    int plat=7;
    assert(runtime_trophy_unlock(1,&plat)==1 && plat==-1);
    assert(toast_id==1 && toast_count==1 && !toast_name[0] && toast_png_size==16 && !memcmp(toast_png,"\x89PNG",4));
    assert(runtime_trophy_unlock(1,&plat)==0 && toast_count==1); /* again: no second toast */
    assert(runtime_trophy_unlock(128,&plat)==-1 && runtime_trophy_unlock(-1,NULL)==-1);
    const char *saved=read_file(json);
    assert(saved && strstr(saved,"\"id\": 1, \"date\": \"") && strstr(saved,"\"unix\": "));
    assert(access(json_tmp,F_OK)!=0);

    struct { uint64_t size; uint32_t groups, total, plat, gold, silver, bronze; char title[128]; char descr[1024]; } game_details={sizeof game_details,0,0,0,0,0,0,{0},{0}};
    struct { uint64_t size; uint32_t unlocked, plat, gold, silver, bronze, progress; } game_data={sizeof game_data,0,0,0,0,0,0};
    assert(sizeof game_details==0x4A0 && sizeof game_data==0x20);
    runtime_trophy_game_info(&game_details,&game_data);
    assert(game_details.total==3 && game_data.unlocked==1 && game_data.progress==33);

    runtime_trophy_configure(trp_path,NULL,json,NULL,keys); /* the next start */
    assert(runtime_trophy_is_unlocked(1) && !runtime_trophy_is_unlocked(0));
    struct { uint64_t size; int32_t id, grade, group; uint8_t hidden, reserved[3]; char name[128]; char descr[1024]; } details={sizeof details,0,0,0,0,{0},{0},{0}};
    struct { uint64_t size; int32_t id; uint8_t unlocked, reserved[3]; uint64_t tick; } data={sizeof data,0,0,{0},0};
    assert(sizeof details==0x498 && sizeof data==0x18);
    assert(runtime_trophy_info(1,&details,&data)==0);
    assert(details.id==1 && !strcmp(details.name,"Trophy #1") && data.id==1 && data.unlocked && data.tick>UINT64_C(0xdcbffeff2bc000));
    assert(runtime_trophy_info(0,NULL,&data)==0 && !data.unlocked && !data.tick);
    puts("PASS: trophies.json persistence (atomic write, restart, game info)");

    /* With the user's key (bbport.ini) and npbind.dat: names, grades, the platinum. */
    remove(json);
    const char *ini_text="show_fps=0\ntrophy_key=0123456789ABCDEF0123456789ABCDEF\n";
    write_file(ini,ini_text,strlen(ini_text));
    static const uint8_t bind[]="\x00\x00\x00\x00\x00\x00\x00\x01xxxxNPWR12345_00\x00\x00\x00";
    write_file(npbind,bind,sizeof bind-1);
    runtime_trophy_configure(trp_path,npbind,json,ini,keys);
    assert(decrypt_calls==1);
    runtime_trophy_game_info(&game_details,&game_data);
    assert(game_details.total==3 && game_details.plat==1 && game_details.bronze==2 && !strcmp(game_details.title,"Test & Title") && game_data.unlocked==0);
    assert(runtime_trophy_unlock(0,&plat)==1 && plat==-1 && toast_id==0 && !strcmp(toast_name,"Knockout"));
    assert(runtime_trophy_unlock(1,&plat)==1 && plat==2 && toast_id==2 && !strcmp(toast_name,"Champion"));
    assert(runtime_trophy_is_unlocked(2));
    runtime_trophy_game_info(&game_details,&game_data);
    assert(game_data.unlocked==3 && game_data.plat==1 && game_data.bronze==2 && game_data.progress==100);
    assert(runtime_trophy_info(1,&details,&data)==0 && details.grade==4 && details.hidden==1 && !strcmp(details.name,"Submission"));
    puts("PASS: trophy names, grades and platinum with a trophy key");

    /* keys.json as shadPS4 writes it (nlohmann: a byte array). */
    remove(ini);
    const char *keys_text="{\n    \"TrophyKeySet\": {\n        \"ReleaseTrophyKey\": [1, 35, 69, 103, 137, 171, 205, 239, 1, 35, 69, 103, 137, 171, 205, 239]\n    }\n}\n";
    write_file(keys,keys_text,strlen(keys_text));
    runtime_trophy_configure(trp_path,npbind,json,ini,keys);
    assert(decrypt_calls==2);
    puts("PASS: trophy key from shadPS4's keys.json");

    remove(json); remove(trp_path); remove(npbind); remove(keys); rmdir(dir);
    return 0;
}
