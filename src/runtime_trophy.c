/* UFC-82 (issue #45): local trophies, no PSN.
 *   - unlocked ids and their dates live in <user>/trophies.json (replaced whole: a crash while
 *     writing leaves the old file);
 *   - the game's sce_sys/trophy/trophy00.trp gives the icons (TROPnnn.PNG, plain) and the count;
 *   - names, grades and the title come from its TROP.ESFM, encrypted: only with the user's own
 *     trophy key (bbport.ini trophy_key=, or shadPS4's keys.json in the user folder). None is
 *     distributed; without it the toast says "Trophy unlocked: #N".
 * GetGameInfo/GetTrophyInfo report this state (layouts from shadPS4's np_trophy.h). */
#define _GNU_SOURCE
#include "runtime_trophy.h"
#include "gpu/bbgpu.h"
#include "platform/platform.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UNIX_EPOCH_TICKS UINT64_C(0xdcbffeff2bc000) /* runtime_rtc.c: 1970 in microseconds from year 1 */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p)<<32 | be32(p+4); }

/* ---- TRP container (shadPS4 trp.h): 0x60-byte big-endian header, then entry_num entries of
 * entry_size bytes: name[32], offset u64, length u64, flag u32 (0 PNG, 3 encrypted ESFM). ---- */
int runtime_trp_parse(const uint8_t *data,size_t size,TrpEntry *entries,int max) {
    if (!data || size<0x60 || be32(data)!=TRP_MAGIC) return -1;
    uint32_t count=be32(data+0x10), entry_size=be32(data+0x14);
    if (entry_size<0x40 || count>4096 || (uint64_t)0x60+(uint64_t)count*entry_size>size) return -1;
    int n=0;
    for (uint32_t i=0;i<count && n<max;++i) {
        const uint8_t *e=data+0x60+(size_t)i*entry_size;
        uint64_t offset=be64(e+32), length=be64(e+40);
        if (offset>size || length>size-offset) return -1;
        memcpy(entries[n].name,e,32); entries[n].name[32]=0;
        entries[n].offset=offset; entries[n].length=length; entries[n].flag=be32(e+48);
        ++n;
    }
    return n;
}
static int same_name(const char *a,const char *b) {
    while (*a && tolower((unsigned char)*a)==tolower((unsigned char)*b)) ++a, ++b;
    return !*a && !*b;
}
const TrpEntry *runtime_trp_find(const TrpEntry *entries,int count,const char *name) {
    for (int i=0;i<count;++i) if (same_name(entries[i].name,name)) return &entries[i];
    return NULL;
}

/* ---- State ---- */
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static char json_path[1024];
static uint8_t *trp; static size_t trp_size;
static TrpEntry entries[256]; static int entry_count;
static int trophy_count;                        /* TROPnnn.PNG icons (or trophies in the ESFM) */
static int64_t unlocked_at[TROPHY_MAX];         /* unix seconds, 0 = locked */
static struct { char name[128]; char grade; int8_t hidden; int32_t group; } info[TROPHY_MAX];
static int have_names;                          /* the ESFM was decrypted */
static char title[128];

/* ---- trophies.json ---- */
int runtime_trophy_load_json(const char *path,int64_t *times,int max) {
    memset(times,0,sizeof(*times)*(size_t)max);
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    char text[65536]; size_t n=fread(text,1,sizeof(text)-1,f); text[n]=0; fclose(f);
    int found=0;
    /* {"id": N, ..., "unix": T} objects; anything else in the file is ignored. */
    for (char *p=text;(p=strstr(p,"\"id\""));) {
        p+=4; while (*p==' ' || *p==':') ++p;
        int id=atoi(p);
        char *end=strchr(p,'}'), *t=strstr(p,"\"unix\"");
        if (!t || (end && t>end)) continue;
        t+=6; while (*t==' ' || *t==':') ++t;
        long long when=atoll(t);
        if (id>=0 && id<max) { times[id]=when>0 ? when : 1; ++found; }
    }
    return found;
}
/* ISO 8601 UTC from unix seconds (civil-from-days; no gmtime_r/gmtime_s split between hosts). */
static void utc_date(int64_t t,char *out,size_t size) {
    int64_t days=t/86400, secs=t%86400;
    if (secs<0) { secs+=86400; --days; }
    days+=719468;
    int64_t era=(days>=0 ? days : days-146096)/146097, doe=days-era*146097;
    int64_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365, doy=doe-(365*yoe+yoe/4-yoe/100);
    int64_t mp=(5*doy+2)/153, d=doy-(153*mp+2)/5+1, m=mp<10 ? mp+3 : mp-9, y=yoe+era*400+(m<=2);
    snprintf(out,size,"%04lld-%02d-%02dT%02d:%02d:%02dZ",(long long)y,(int)m,(int)d,(int)(secs/3600),(int)(secs/60%60),(int)(secs%60));
}
int runtime_trophy_save_json(const char *path,const int64_t *times,int max) {
    char temp[1100];
    snprintf(temp,sizeof temp,"%s.tmp",path);
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    int ok=fputs("{\n  \"version\": 1,\n  \"unlocked\": [",f)>=0, first=1;
    for (int id=0;id<max;++id) {
        if (!times[id]) continue;
        char date[64]; utc_date(times[id],date,sizeof date);
        ok=fprintf(f,"%s\n    {\"id\": %d, \"date\": \"%s\", \"unix\": %lld}",first ? "" : ",",id,date,(long long)times[id])>0 && ok;
        first=0;
    }
    ok=fputs(first ? "]\n}\n" : "\n  ]\n}\n",f)>=0 && ok;
    ok=fflush(f)==0 && ok;
    ok=fclose(f)==0 && ok;
    if (!ok || platform_rename(temp,path)) { platform_unlink(temp); return -1; }
    return 0;
}

/* ---- The trophy key and the NP communication id (only for names) ---- */
static int hex_key(const char *s,uint8_t key[16]) {
    while (*s==' ' || *s=='\t' || *s=='"') ++s;
    for (int i=0;i<16;++i) {
        char pair[3]={s[2*i],s[2*i+1],0};
        if (!isxdigit((unsigned char)pair[0]) || !isxdigit((unsigned char)pair[1])) return 0;
        key[i]=(uint8_t)strtoul(pair,NULL,16);
    }
    return 1;
}
/* bbport.ini trophy_key=<32 hex digits>, else shadPS4's keys.json ("ReleaseTrophyKey": a hex
 * string, or the byte array nlohmann writes). */
static int find_key(const char *ini,const char *keys_json,uint8_t key[16]) {
    char line[512];
    FILE *f=ini ? fopen(ini,"r") : NULL;
    if (f) {
        int got=0;
        while (!got && fgets(line,sizeof line,f)) if (!strncmp(line,"trophy_key=",11)) got=hex_key(line+11,key);
        fclose(f);
        if (got) return 1;
    }
    f=keys_json ? fopen(keys_json,"rb") : NULL;
    if (!f) return 0;
    char text[4096]; size_t n=fread(text,1,sizeof(text)-1,f); text[n]=0; fclose(f);
    char *p=strstr(text,"\"ReleaseTrophyKey\"");
    if (!p) return 0;
    p+=18; while (*p==' ' || *p==':' || *p=='\n' || *p=='\r' || *p=='\t') ++p;
    if (*p=='"') return hex_key(p+1,key);
    if (*p!='[') return 0;
    for (int i=0;i<16;++i) {
        ++p; while (*p==' ' || *p=='\n' || *p=='\r' || *p=='\t') ++p;
        if (!isdigit((unsigned char)*p)) return 0;
        key[i]=(uint8_t)strtoul(p,&p,10);
        while (*p==' ' || *p=='\n' || *p=='\r' || *p=='\t') ++p;
        if (*p!=(i==15 ? ']' : ',')) return 0;
    }
    return 1;
}
/* npbind.dat holds the title's NP communication ids ("NPWRnnnnn_00"); the first is trophy00's. */
static int find_np_comm_id(const char *path,char out[16]) {
    FILE *f=path ? fopen(path,"rb") : NULL;
    if (!f) return 0;
    uint8_t buf[8192]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
    for (size_t i=0;i+12<=n;++i) {
        if (memcmp(buf+i,"NPWR",4)) continue;
        int ok=buf[i+9]=='_';
        for (int k=4;k<9 && ok;++k) ok=isdigit(buf[i+k]);
        for (int k=10;k<12 && ok;++k) ok=isdigit(buf[i+k]);
        if (ok) { memset(out,0,16); memcpy(out,buf+i,12); return 1; }
    }
    return 0;
}

/* ---- TROP.SFM (XML): title-name and <trophy id="000" hidden="no" ttype="B" gid="1"> with
 * <name>; only what the game and the toast use. ---- */
static void xml_text(const char *from,const char *to,char *out,size_t size) {
    size_t n=0;
    for (const char *p=from;p<to && n+1<size;) {
        static const struct { const char *entity, ch; } entities[]={{"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&quot;",'"'},{"&apos;",'\''}};
        int done=0;
        for (size_t e=0;e<5 && !done;++e) {
            size_t len=strlen(entities[e].entity);
            if (!strncmp(p,entities[e].entity,len)) { out[n++]=entities[e].ch; p+=len; done=1; }
        }
        if (!done) out[n++]=*p++;
    }
    out[n]=0;
}
static const char *attribute(const char *tag,const char *tag_end,const char *name,char *out,size_t size) {
    char pattern[32]; snprintf(pattern,sizeof pattern," %s=\"",name);
    const char *p=strstr(tag,pattern);
    if (!p || p>tag_end) { out[0]=0; return NULL; }
    p+=strlen(pattern);
    const char *q=strchr(p,'"');
    if (!q || q>tag_end) { out[0]=0; return NULL; }
    xml_text(p,q,out,size);
    return out;
}
int runtime_trophy_parse_sfm(const char *xml) {
    const char *t=strstr(xml,"<title-name>");
    if (t) { t+=12; const char *e=strstr(t,"</title-name>"); if (e) xml_text(t,e,title,sizeof title); }
    int count=0;
    for (const char *p=xml;(p=strstr(p,"<trophy "));) {
        const char *tag_end=strchr(p,'>'), *close=strstr(p,"</trophy>");
        if (!tag_end) break;
        char value[16];
        int id=attribute(p,tag_end,"id",value,sizeof value) ? atoi(value) : -1;
        if (id>=0 && id<TROPHY_MAX) {
            info[id].grade=attribute(p,tag_end,"ttype",value,sizeof value) ? value[0] : 0;
            info[id].hidden=attribute(p,tag_end,"hidden",value,sizeof value) && !strcmp(value,"yes");
            info[id].group=attribute(p,tag_end,"gid",value,sizeof value) ? atoi(value) : -1;
            const char *name=strstr(tag_end,"<name>");
            if (name && (!close || name<close)) {
                name+=6; const char *e=strstr(name,"</name>");
                if (e) xml_text(name,e,info[id].name,sizeof info[id].name);
            }
            if (id+1>count) count=id+1;
        }
        p=tag_end;
    }
    return count;
}

static void load_names(const char *ini,const char *keys_json,const char *npbind) {
    uint8_t key[16]; char comm[16];
    const TrpEntry *e=runtime_trp_find(entries,entry_count,"TROP.ESFM");
    if (!e || e->length<=32 || (e->length-16)%16) return;
    if (!find_key(ini,keys_json,key)) { printf("Runtime: trophies: no trophy key (bbport.ini trophy_key), only numbers are shown\n"); return; }
    if (!find_np_comm_id(npbind,comm)) { printf("Runtime: trophies: no NP communication id in sce_sys/npbind.dat, only numbers are shown\n"); return; }
    size_t n=(size_t)e->length-16;
    char *xml=malloc(n+1);
    if (!xml) return;
    int ok=bbgpu_trophy_decrypt(key,comm,trp+e->offset,trp+e->offset+16,n,(uint8_t *)xml);
    xml[n]=0;
    if (ok && strstr(xml,"<trophyconf")) {
        int count=runtime_trophy_parse_sfm(xml);
        if (count>0) { have_names=1; if (count>trophy_count) trophy_count=count; }
        printf("Runtime: trophies: names read for %d trophies (%s)\n",count,title);
    } else printf("Runtime: trophies: the trophy key does not decrypt TROP.ESFM, only numbers are shown\n");
    memset(key,0,sizeof key);
    free(xml);
}

void runtime_trophy_configure(const char *trp_path,const char *npbind_path,const char *json,const char *ini,const char *keys_json) {
    pthread_mutex_lock(&lock);
    snprintf(json_path,sizeof json_path,"%s",json);
    free(trp); trp=NULL; trp_size=0; entry_count=0; trophy_count=0; have_names=0; title[0]=0;
    memset(info,0,sizeof info);
    FILE *f=trp_path ? fopen(trp_path,"rb") : NULL;
    if (f) {
        fseek(f,0,SEEK_END); long size=ftell(f); fseek(f,0,SEEK_SET);
        if (size>0 && size<(64<<20) && (trp=malloc((size_t)size)) && fread(trp,1,(size_t)size,f)==(size_t)size) trp_size=(size_t)size;
        fclose(f);
    }
    int n=trp_size ? runtime_trp_parse(trp,trp_size,entries,(int)(sizeof entries/sizeof *entries)) : -1;
    if (n<0) {
        printf("Runtime: trophies: %s %s; unlocks are still saved\n",trp_path ? trp_path : "(no path)",trp_size ? "is not a TRP file" : "not found");
        free(trp); trp=NULL; trp_size=0;
    } else {
        entry_count=n;
        for (int i=0;i<n;++i) {
            int id;
            char tail[8];
            if (sscanf(entries[i].name,"TROP%3d.%3s",&id,tail)==2 && same_name(tail,"PNG") && id>=0 && id<TROPHY_MAX && id+1>trophy_count) trophy_count=id+1;
        }
        load_names(ini,keys_json,npbind_path);
    }
    int found=runtime_trophy_load_json(json_path,unlocked_at,TROPHY_MAX);
    printf("Runtime: trophies: %d in the game, %d unlocked (%s)\n",trophy_count,found,json_path);
    pthread_mutex_unlock(&lock);
}

/* Platinum grade 1, gold 2, silver 3, bronze 4, unknown 0 (OrbisNpTrophyGrade). */
static int grade(int id) {
    switch (info[id].grade) { case 'P': return 1; case 'G': return 2; case 'S': return 3; case 'B': return 4; default: return 0; }
}
static int unlock_locked(int id,int64_t now) {
    if (unlocked_at[id]) return 0;
    unlocked_at[id]=now;
    return 1;
}
static void toast(int id) {
    char name[128]="";
    const void *png=NULL; uint64_t png_size=0;
    char icon[24]; snprintf(icon,sizeof icon,"TROP%03d.PNG",id);
    pthread_mutex_lock(&lock);
    if (have_names) snprintf(name,sizeof name,"%s",info[id].name);
    const TrpEntry *e=runtime_trp_find(entries,entry_count,icon);
    if (e) { png=trp+e->offset; png_size=e->length; } /* trp stays loaded for the whole run */
    pthread_mutex_unlock(&lock);
    bbgpu_trophy_toast(id,name[0] ? name : NULL,png,png_size);
}

int runtime_trophy_unlock(int id,int *platinum) {
    if (platinum) *platinum=-1;
    if (id<0 || id>=TROPHY_MAX) return -1;
    pthread_mutex_lock(&lock);
    int64_t now=(int64_t)time(NULL);
    int fresh=unlock_locked(id,now), plat=-1;
    /* The system unlocks the platinum when every other trophy is (grades: only with the key). */
    if (fresh && have_names && grade(id)!=1) {
        int all=1, candidate=-1;
        for (int t=0;t<trophy_count;++t) {
            if (grade(t)==1) candidate=t;
            else if (grade(t) && !unlocked_at[t]) all=0;
        }
        if (all && candidate>=0 && unlock_locked(candidate,now)) plat=candidate;
    }
    int saved=fresh ? runtime_trophy_save_json(json_path,unlocked_at,TROPHY_MAX) : 0;
    pthread_mutex_unlock(&lock);
    if (!fresh) { printf("Runtime: trophy %d was already unlocked\n",id); return 0; }
    printf("Runtime: trophy %d unlocked (%s %s)\n",id,saved ? "cannot write" : "saved to",json_path);
    toast(id);
    if (plat>=0) { printf("Runtime: platinum trophy %d unlocked\n",plat); toast(plat); }
    if (platinum) *platinum=plat;
    return 1;
}

int runtime_trophy_is_unlocked(int id) {
    if (id<0 || id>=TROPHY_MAX) return 0;
    pthread_mutex_lock(&lock);
    int on=unlocked_at[id]!=0;
    pthread_mutex_unlock(&lock);
    return on;
}

/* The game passes each structure with its size first; fields past that size are not written. */
static uint64_t struct_size(void *p) { uint64_t size; memcpy(&size,p,8); return size<4096 ? size : 0; }
static void put32(void *base,uint64_t size,size_t at,uint32_t v) { if (at+4<=size) memcpy((char *)base+at,&v,4); }
static void put_text(void *base,uint64_t size,size_t at,size_t room,const char *text) {
    if (at+room>size) return;
    snprintf((char *)base+at,room,"%s",text);
}

/* OrbisNpTrophyGameDetails (0x4A0): size, num_groups, num_trophies, num_platinum/gold/silver/
 * bronze, title[128], description[1024]. OrbisNpTrophyGameData (0x20): size,
 * unlocked_trophies, unlocked_platinum/gold/silver/bronze, progress_percentage. */
void runtime_trophy_game_info(void *details,void *data) {
    pthread_mutex_lock(&lock);
    uint32_t by_grade[5]={0}, unlocked_by_grade[5]={0}, unlocked=0;
    for (int id=0;id<trophy_count;++id) {
        ++by_grade[grade(id)];
        if (unlocked_at[id]) { ++unlocked; ++unlocked_by_grade[grade(id)]; }
    }
    for (int id=trophy_count;id<TROPHY_MAX;++id) if (unlocked_at[id]) ++unlocked; /* beyond a missing .trp */
    uint32_t total=trophy_count>0 ? (uint32_t)trophy_count : 0;
    if (details) {
        uint64_t size=struct_size(details);
        if (size>8) memset((char *)details+8,0,size-8);
        /* num_groups stays 0: the base game only (GetGroupInfo answers signed out). */
        put32(details,size,12,total);
        for (int g=1;g<=4;++g) put32(details,size,12+4*(size_t)g,by_grade[g]);
        put_text(details,size,32,128,title);
    }
    if (data) {
        uint64_t size=struct_size(data);
        if (size>8) memset((char *)data+8,0,size-8);
        put32(data,size,8,unlocked);
        for (int g=1;g<=4;++g) put32(data,size,8+4*(size_t)g,unlocked_by_grade[g]);
        put32(data,size,28,total ? (unlocked>total ? 100 : unlocked*100/total) : 0);
    }
    pthread_mutex_unlock(&lock);
}

/* OrbisNpTrophyDetails (0x498): size, trophy_id, trophy_grade, group_id, hidden (bool + 3),
 * name[128], description[1024]. OrbisNpTrophyData (0x18): size, trophy_id, unlocked (bool + 3),
 * timestamp (RtcTick, microseconds from year 1). */
int runtime_trophy_info(int id,void *details,void *data) {
    if (id<0 || id>=TROPHY_MAX) return -1;
    pthread_mutex_lock(&lock);
    if (details) {
        uint64_t size=struct_size(details);
        if (size>8) memset((char *)details+8,0,size-8);
        put32(details,size,8,(uint32_t)id);
        put32(details,size,12,(uint32_t)grade(id));
        put32(details,size,16,(uint32_t)(have_names ? info[id].group : -1));
        if (size>20) ((uint8_t *)details)[20]=(uint8_t)info[id].hidden;
        char fallback[32]; snprintf(fallback,sizeof fallback,"Trophy #%d",id);
        put_text(details,size,24,128,have_names && info[id].name[0] ? info[id].name : fallback);
    }
    if (data) {
        uint64_t size=struct_size(data);
        if (size>8) memset((char *)data+8,0,size-8);
        put32(data,size,8,(uint32_t)id);
        if (size>12) ((uint8_t *)data)[12]=unlocked_at[id]!=0;
        if (size>=24 && unlocked_at[id]) { uint64_t tick=UNIX_EPOCH_TICKS+(uint64_t)unlocked_at[id]*1000000u; memcpy((char *)data+16,&tick,8); }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
