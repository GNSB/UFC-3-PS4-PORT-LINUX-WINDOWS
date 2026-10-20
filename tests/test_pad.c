#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

/* The window thread's event pump, as during a restart of the gamepad subsystem (UFC-62). */
static volatile int pumping;
static void *pump(void *arg) {
    (void)arg;
    while (pumping) { SDL_UpdateJoysticks(); SDL_UpdateGamepads(); }
    return NULL;
}

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
static int rescan;
int bbgpu_pad_rescan_take(void) { const int r=rescan; rescan=0; return r; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    /* bbport.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[]="/tmp/bbport-pad-config-XXXXXX";
    int config_fd=mkstemp(config);
    assert(config_fd>=0);
    const char controls[]="upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\n";
    assert(write(config_fd,controls,sizeof(controls)-1)==(ssize_t)(sizeof(controls)-1));
    close(config_fd);
    setenv("BB_CONFIG",config,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==2);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==3);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1]==SDL_SCANCODE_SPACE);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    /* UFC-62: a gamepad unplugged and plugged back (as a new SDL instance id) ten times. The
     * handle stays valid and reads succeed while it is gone, input comes back with it, the loss is
     * logged and no automatic re-enumeration runs meanwhile (SDL's hot-plug brings it back). */
    {
        PadLink l={0};
        assert(link_update(&l,1,"a")==LINK_FIRST && link_update(&l,1,"a")==LINK_SAME);
        assert(link_update(&l,0,NULL)==LINK_LOST && !strcmp(l.name,"a") && link_update(&l,0,NULL)==LINK_SAME);
        assert(link_update(&l,1,"b")==LINK_BACK && !strcmp(l.name,"b"));
    }
    for (int round=0;round<10;++round) {
        rescan_after=0; pad_rescan_attempts=0; /* an automatic re-enumeration would be due */
        SDL_JoystickID back=SDL_AttachVirtualJoystick(&desc);
        assert(back!=0);
        SDL_Joystick *j=SDL_OpenJoystick(back);
        assert(j && SDL_SetJoystickVirtualButton(j,SDL_GAMEPAD_BUTTON_EAST,true));
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS) && data.connected==1);
        assert(gamepad && SDL_GetGamepadID(gamepad)==back && links[0].present);
        SDL_CloseJoystick(j);
        assert(SDL_DetachVirtualJoystick(back));
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        const unsigned before=pad_reinits;
        assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_CROSS) && data.connected==1);
        assert(!gamepad && !links[0].present && pad_reinits==before);
        assert(pad_read(1,&data,1)==1 && pad_ok_handle(1)==0);
    }
    /* "Rescan controllers" restarts the subsystem while another thread pumps it: every gamepad
     * held here (the active one and an idle one) is dropped first, nothing dangles. */
    {
        SDL_JoystickID a=SDL_AttachVirtualJoystick(&desc), b=SDL_AttachVirtualJoystick(&desc);
        assert(a && b);
        SDL_UpdateJoysticks();
        assert(pad_read_state(1,&data)==0 && gamepad);
        usleep(110000); /* the idle pads are opened ten times a second */
        assert(pad_read_state(1,&data)==0);
        int idle=0; for (int i=0;i<other_count;++i) idle+=others[i]!=NULL;
        assert(idle==1);
        pthread_t thread; pumping=1;
        assert(pthread_create(&thread,NULL,pump,NULL)==0);
        for (int round=0;round<20;++round) {
            const unsigned before=pad_reinits;
            rescan=1;
            assert(pad_read_state(1,&data)==0 && pad_reinits==before+1 && !gamepad);
            for (int i=0;i<other_count;++i) assert(!others[i]);
        }
        pumping=0;
        pthread_join(thread,NULL);
    }
    SDL_Quit();
    unlink(path);
    unlink(config);
    /* Stick calibration (bbport 0.5, PR 71; per port here): a genuine pad reads straight through
     * the 5/127 dead zone; a clone biased to +16380 on all four axes gets that as its neutral and
     * its half-span as the travel, so a full push still reaches full deflection. */
    {
        PadCal c; cal_reset(&c);
        assert(c.have_center==0 && cal_enabled==1 && pad_deadzone==5 && pad_deadzone_outer==127);
        int16_t zero[4]={0,0,0,0};
        for (int i=0;i<PAD_CAL_QUIET_SAMPLES+1;++i) cal_sample(&c,zero);
        assert(!c.have_center); /* SDL's pre-report zeros are not a neutral yet */
        c.since-=PAD_CAL_TIMEOUT_US+1;
        for (int i=0;i<PAD_CAL_QUIET_SAMPLES;++i) cal_sample(&c,zero);
        assert(c.have_center && c.center[0]==0 && c.base[0]==0);
        assert(stick_axis(&c,0,0)==128 && stick_axis(&c,0,32767)==255 && stick_axis(&c,0,-32768)==0);
        assert(stick_axis(&c,0,600)==128);                /* inside the dead zone */
        assert(stick_axis(&c,0,16384)>128+50 && stick_axis(&c,0,16384)<128+70);
        PadCal clone; cal_reset(&clone);
        int16_t biased[4]={16380,16380,16380,16380};
        for (int i=0;i<PAD_CAL_QUIET_SAMPLES;++i) cal_sample(&clone,biased);
        assert(clone.have_center && clone.center[0]==16380 && clone.base[0]==16380);
        assert(stick_axis(&clone,0,16380)==128 && stick_axis(&clone,0,32760)==255 && stick_axis(&clone,0,0)==0);
    }
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, controls, stick calibration, disconnect/reconnect");
}
