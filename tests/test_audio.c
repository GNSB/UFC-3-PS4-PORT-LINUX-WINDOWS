/* portheon: libSceAudioOut's master volume, 7.1 fold for stereo devices and soft limiter
 * (src/runtime_audio.c), checked through BB_AUDIO_DUMP on SDL's dummy (stereo) device. */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <unistd.h>
#include "../src/runtime_audio.c"

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static int near(float a, float b) { return fabsf(a-b)<1e-4f; }

int main(void) {
    char dump[]="/tmp/portheon-audio-test-XXXXXX";
    int fd=mkstemp(dump); assert(fd>=0); close(fd); unlink(dump);
    setenv("SDL_AUDIO_DRIVER","dummy",1);
    setenv("BB_AUDIO_DUMP",dump,1);
    setenv("BB_VOLUME","150",1);
    assert(audio_init()==0);
    /* An 8-channel float port (format 5) and a stereo s16 port (format 1). */
    int32_t surround=audio_open(1,0,0,256,48000,5), stereo=audio_open(1,0,0,256,48000,1);
    assert(surround>0 && stereo>0);
    assert(ports[surround & 0xff].out_channels==2); /* the dummy device is stereo: folded here */
    static float in8[256*8];
    for (int f=0;f<256;++f) {
        float *s=in8+f*8;               /* PS4 order: L R C LFE SL SR BL BR */
        s[0]=0.1f; s[1]=0.0f; s[2]=0.2f; s[3]=0.0f; s[4]=0.1f; s[5]=0.0f; s[6]=0.0f; s[7]=0.0f;
        if (f==1) { s[0]=1.0f; s[1]=-1.0f; s[2]=1.0f; } /* loud: limited, not clipped */
    }
    static int16_t in2[256*2];
    for (int f=0;f<256;++f) { in2[f*2]=8192; in2[f*2+1]=-16384; }
    assert(audio_output(surround,in8)==256*8);
    assert(audio_output(stereo,in2)==256*2);
    audio_close(surround); audio_close(stereo);

    char path[256]; float out[256*2];
    snprintf(path,sizeof path,"%s.port%d.2ch.f32",dump,surround & 0xff);
    FILE *f=fopen(path,"rb"); assert(f); assert(fread(out,sizeof(float),256*2,f)==256*2); fclose(f); unlink(path);
    /* L = 1.5 * (0.1 + 0.7071*0.2 + 0.7071*0.1 (SL)) = 0.4682; R = 1.5 * 0.7071*0.2 = 0.2121. */
    assert(near(out[0],1.5f*(0.1f+0.7071f*0.2f+0.7071f*0.1f)));
    assert(near(out[1],1.5f*0.7071f*0.2f));
    assert(out[2]<1.0f && out[2]>0.9f);                 /* 2.67: limited below full scale */
    assert(near(out[3],1.5f*(-1.0f+0.7071f*1.0f)));    /* -0.44: untouched */
    snprintf(path,sizeof path,"%s.port%d.2ch.f32",dump,stereo & 0xff);
    f=fopen(path,"rb"); assert(f); assert(fread(out,sizeof(float),256*2,f)==256*2); fclose(f); unlink(path);
    assert(near(out[0],0.375f));                  /* 0.25 * 1.5 */
    assert(near(out[1],-0.75f));                  /* -0.5 * 1.5, below the limiter knee */
    puts("audio-test: ok");
    return 0;
}
