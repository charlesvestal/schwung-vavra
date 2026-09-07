// Real API integration test. Run on Move with a module directory and optional clock/voices.
#include "dsp/plugin_api.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

using Clock = std::chrono::steady_clock;
static double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now()-start).count();
}
static int underrunsOf(plugin_api_v2_t* api, void* inst) {
    char value[512]{};
    if(api->get_param(inst,"diagnostics",value,sizeof(value))<1) return -1;
    const char* field=strstr(value,"underruns=");
    return field ? atoi(field+10) : -1;
}
int main(int argc, char** argv) {
    if(argc < 2) return 2;
#ifdef __linux__
    if(geteuid()==0) {
        cpu_set_t cpu; CPU_ZERO(&cpu); CPU_SET(3,&cpu);
        sched_setaffinity(0,sizeof(cpu),&cpu);
        sched_param priority{}; priority.sched_priority=70;
        if(sched_setscheduler(0,SCHED_FIFO,&priority)) { perror("harness priority"); return 1; }
        printf("harness=core3/FIFO70\n");
    } else {
        // Unprivileged, this harness is SCHED_OTHER on the same cores as the
        // emulator's FIFO 20 threads: it gets descheduled, then bursts its
        // catch-up render_block calls and drains the queue itself. Every
        // underrun it then reports is its own. Say so on every line of output
        // that could be pasted into a result.
        printf("harness=UNPRIVILEGED underruns_measure_this_harness_not_the_module\n");
        fprintf(stderr,"WARNING: not root -- underrun counts are meaningless. Re-run as root.\n");
    }
#endif
    char path[1024];
    snprintf(path,sizeof(path),"%s/dsp.so",argv[1]);
    void* lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!lib) { fprintf(stderr,"FAIL: %s\n",dlerror()); return 1; }
    auto init=reinterpret_cast<plugin_api_v2_t* (*)(const host_api_v1_t*)>(dlsym(lib,"move_plugin_init_v2"));
    if(!init) return 1;
    host_api_v1_t host{}; host.api_version=1; host.sample_rate=44100; host.frames_per_block=128;
    auto* api=init(&host);
    auto start=Clock::now();
    void* inst=api->create_instance(argv[1],nullptr);
    double create=seconds(start);
    printf("create_seconds=%.6f\n",create); fflush(stdout);
    if(!inst || create>0.1) { fprintf(stderr,"FAIL: create_instance returned %p in %.6fs\n",inst,create); return 1; }
    if(argc>2 && !strcmp(argv[2],"cancel")) {
        api->destroy_instance(inst); dlclose(lib);
        printf("cancel_seconds=%.6f\n",seconds(start));
        return seconds(start)<0.5 ? 0 : 1;
    }
    // Sized from the host's own ceiling (SHADOW_PARAM_VALUE_LEN, now 128KB).
    // At 64KB this silently became "FAIL: host chain_params contract" the
    // moment the module outgrew it -- the harness reporting its own buffer as
    // the module's defect.
    static char contract[131072];
    if(api->get_param(inst,"is_loading",contract,sizeof(contract))<1 || strcmp(contract,"1")) {
        fprintf(stderr,"FAIL: host is_loading contract\n"); api->destroy_instance(inst); dlclose(lib); return 1;
    }
    const int contractLen=api->get_param(inst,"chain_params",contract,sizeof(contract));
    // The 300 preset names deliberately do NOT appear here any more: preset
    // selection is a browser page reading one name at a time, not an enum
    // carrying every option in the contract.
    if(contractLen<1 || !strstr(contract,"dsp_clock") || !strstr(contract,"flt1_cutoff") ||
       strstr(contract,"LosAngeles2019")) {
        fprintf(stderr,"FAIL: host chain_params contract (len %d)\n",contractLen);
        api->destroy_instance(inst); dlclose(lib); return 1;
    }
    printf("chain_params_len=%d\n",contractLen);
    if(const char* path=getenv("VAVRA_DUMP_CONTRACT")) {
        if(FILE* f=fopen(path,"w")) { fwrite(contract,1,contractLen,f); fclose(f); }
    }
    // "-" (or empty) leaves the module's own default in place. atoi("") is 0,
    // which the module clamps to its MINIMUM -- so passing a placeholder here
    // silently measured 25% and called it the default.
    if(argc>2 && argv[2][0] && strcmp(argv[2],"-")) api->set_param(inst,"dsp_clock",argv[2]);
    if(argc>4) api->set_param(inst,"buffer_ms",argv[4]);
    // argv[5]: bank:program, sent as Bank Select MSB + Program Change once the
    // emulator is ready, to find out whether the ROM presets are reachable
    // over plain MIDI at all.
    const char* program = argc>5 ? argv[5] : nullptr;
    char value[512]{};
    int16_t audio[256];
    bool ready=false;
    while(seconds(start)<120) {
        api->render_block(inst,audio,128);
        if(api->get_error(inst,value,sizeof(value))>0) {
            fprintf(stderr,"FAIL: %s\n",value);
            api->destroy_instance(inst); dlclose(lib);
            return argc>2 && !strcmp(argv[2],"missing-rom") ? 0 : 1;
        }
        api->get_param(inst,"loading",value,sizeof(value));
        if(!strcmp(value,"0")) { ready=true; break; }
        for(auto sample:audio) if(sample) {
            fprintf(stderr,"FAIL: nonzero sample while loading=%s\n",value);
            api->destroy_instance(inst); dlclose(lib); return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    printf("boot_seconds=%.3f ready=%d\n",seconds(start),ready); fflush(stdout);
    if(!ready) { api->destroy_instance(inst); dlclose(lib); return 1; }
    // Let firmware initialization and the initial audio queue settle before notes.
    // Underruns are localized to the block they appear in: a stall during this
    // silent window is inaudible, one during the note loop is a dropout.
    int seen=underrunsOf(api,inst);
    printf("underruns_at_ready=%d\n",seen); fflush(stdout);
    for(int i=0;i<345;++i) {
        api->render_block(inst,audio,128);
        int now=underrunsOf(api,inst);
        if(now!=seen) { printf("settle_block=%d underruns=%d t=%.3f\n",i,now,seconds(start)); fflush(stdout); seen=now; }
        std::this_thread::sleep_for(std::chrono::microseconds(2902));
    }
    printf("settle_done t=%.3f\n",seconds(start)); fflush(stdout);
    if(program) {
        // bank:preset through the module's own params, 1-based like the device.
        // "label:<text>" writes the option LABEL instead of the index, which
        // is how some callers address an enum.
        if(!strncmp(program,"label:",6)) {
            api->set_param(inst,"preset",program+6);
            printf("program=label %s\n",program+6);
        } else {
            char text[16];
            snprintf(text,sizeof(text),"%d",atoi(program));
            api->set_param(inst,"preset",text);
            printf("program=index %s\n",text);
        }
        // The firmware needs time to swap the edit buffer before it is played.
        for(int i=0;i<172;++i) { api->render_block(inst,audio,128);
            std::this_thread::sleep_for(std::chrono::microseconds(2902)); }
        if(api->get_param(inst,"preset_name",value,sizeof(value))>0) printf("preset_name=%s\n",value);
        if(api->get_param(inst,"writes",value,sizeof(value))>0) printf("writes: %s\n",value);
        // Loading a patch stalls the emulator briefly -- new firmware work and
        // new JIT paths. That is a click when you change sound, not a synth
        // that cannot hold a note, so it is counted on its own and the
        // playing baseline is taken AFTER it.
        const int changeUnderruns=underrunsOf(api,inst)-seen;
        seen=underrunsOf(api,inst);
        printf("preset_change_underruns=%d\n",changeUnderruns);
    }
    const int underrunsBeforeNotes=seen;
    // argv[6] "multi:<part>:<channel>" switches the firmware to Multi mode,
    // points the part keys at <part>, puts that part on <channel>, and plays
    // the notes there -- the whole multitimbral path through the module's own
    // parameters rather than through a probe.
    int noteChannel=0;
    if(argc>6 && !strncmp(argv[6],"multi:",6)) {
        int part=1,channel=1;
        sscanf(argv[6]+6,"%d:%d",&part,&channel);
        char text[16];
        api->set_param(inst,"mode","1");
        snprintf(text,sizeof(text),"%d",part);    api->set_param(inst,"part",text);
        snprintf(text,sizeof(text),"%d",channel); api->set_param(inst,"part_channel",text);
        if(program) { snprintf(text,sizeof(text),"%d",atoi(program)); api->set_param(inst,"preset",text); }
        noteChannel=std::clamp(channel,1,16)-1;
        for(int i=0;i<690;++i) { api->render_block(inst,audio,128);
            std::this_thread::sleep_for(std::chrono::microseconds(2902)); }
        api->get_param(inst,"preset_name",value,sizeof(value));
        printf("multi part=%d channel=%d name=%s\n",part,channel,value);
        seen=underrunsOf(api,inst);
    }
    // Env-driven scenario for the correctness battery:
    //   VAVRA_MODE=single|multi  VAVRA_PRESET=<idx>
    //   VAVRA_PART1=<idx> VAVRA_PART2=<idx> VAVRA_PART2_VOL=<0..127>
    //   VAVRA_PLAY_CH=<1..16>   VAVRA_WAV=<path>
    if(const char* scenarioMode=getenv("VAVRA_MODE")) {
        char text[32];
        auto envInt=[&](const char* name,int fallback){ const char* v=getenv(name); return v&&*v ? atoi(v) : fallback; };
        auto setPart=[&](int part,int presetIndex,int channel,int volume){
            snprintf(text,sizeof(text),"%d",part);        api->set_param(inst,"part",text);
            snprintf(text,sizeof(text),"%d",channel);     api->set_param(inst,"part_channel",text);
            snprintf(text,sizeof(text),"%d",volume);      api->set_param(inst,"part_volume",text);
            snprintf(text,sizeof(text),"%d",presetIndex); api->set_param(inst,"preset",text);
        };
        // Mode is three-valued now: 0 Play, 1 Edit, 2 Multi. Writing "1" for
        // multi selected EDIT, which leaves the firmware in Single -- where
        // every channel plays the one sound, so the multi checks passed while
        // testing nothing.
        const bool multi=!strcmp(scenarioMode,"multi");
        api->set_param(inst,"mode",multi?"2":"0");
        if(multi) {
            setPart(1,envInt("VAVRA_PART1",0),envInt("VAVRA_PART1_CH",1),envInt("VAVRA_PART1_VOL",127));
            setPart(2,envInt("VAVRA_PART2",16),envInt("VAVRA_PART2_CH",2),envInt("VAVRA_PART2_VOL",127));
            // Parts 3 and 4 are opt-in, so a stack of four patches on one
            // channel can be built without disturbing the two-part cases.
            if(getenv("VAVRA_PART3")) setPart(3,envInt("VAVRA_PART3",0),envInt("VAVRA_PART3_CH",3),envInt("VAVRA_PART3_VOL",127));
            if(getenv("VAVRA_PART4")) setPart(4,envInt("VAVRA_PART4",0),envInt("VAVRA_PART4_CH",4),envInt("VAVRA_PART4_VOL",127));
        } else if(getenv("VAVRA_PRESET")) {
            // Only when asked: selecting a preset re-reads the edit buffer a
            // moment later, which would overwrite any parameter written here.
            snprintf(text,sizeof(text),"%d",envInt("VAVRA_PRESET",0));
            api->set_param(inst,"preset",text);
        }
        // VAVRA_SET="key=value,key=value" exercises the generated synth
        // parameters through the same path the knob grid uses.
        if(getenv("VAVRA_PRESET") && getenv("VAVRA_SET"))
            for(int i=0;i<345;++i) { api->render_block(inst,audio,128);
                std::this_thread::sleep_for(std::chrono::microseconds(2902)); }
        if(const char* sets=getenv("VAVRA_SET")) {
            char copy[512]; snprintf(copy,sizeof(copy),"%s",sets);
            for(char* item=strtok(copy,","); item; item=strtok(nullptr,",")) {
                char* eq=strchr(item,'=');
                if(!eq) continue;
                *eq=0;
                api->set_param(inst,item,eq+1);
                printf("set %s=%s\n",item,eq+1);
            }
        }
        if(const char* gets=getenv("VAVRA_GET")) {
            char copy[512]; snprintf(copy,sizeof(copy),"%s",gets);
            for(char* item=strtok(copy,","); item; item=strtok(nullptr,",")) {
                char got[128]{};
                const int n=api->get_param(inst,item,got,sizeof(got));
                printf("get %s -> %s (len %d)\n",item,got,n);
            }
        }
        if(api->get_param(inst,"writes",value,sizeof(value))>0) printf("writes: %s\n",value);
        noteChannel=std::clamp(envInt("VAVRA_PLAY_CH",1),1,16)-1;
        // Let every write land and the patch changes settle before playing.
        for(int i=0;i<1035;++i) { api->render_block(inst,audio,128);
            std::this_thread::sleep_for(std::chrono::microseconds(2902)); }
        api->get_param(inst,"preset_name",value,sizeof(value));
        printf("scenario mode=%s play_ch=%d name=%s\n",scenarioMode,noteChannel+1,value);
        // What the DEVICE says is loaded, off its own display -- the selected
        // name now comes from the table, so only this can catch a selection
        // that never reached the firmware.
        if(api->get_param(inst,"lcd_name",value,sizeof(value))>0) printf("scenario lcd=<%s>\n",value);
        api->get_param(inst,"multi_ready",value,sizeof(value));
        printf("scenario multi_ready=%s\n",value);
        seen=underrunsOf(api,inst);
    }
    const int notes[]={60,64,67,72,76,79,84,88};
    const int voices=argc>3 ? std::clamp(atoi(argv[3]),1,8) : 4;
    for(int i=0;i<voices;++i) { uint8_t msg[]={(uint8_t)(0x90|noteChannel),(uint8_t)notes[i],100}; api->on_midi(inst,msg,3,0); }
    // "sweep" changes the preset every second while notes are held, which is
    // the gesture a user makes on the knob. A module-side bug where only the
    // first write lands shows up here as a flat rms.
    const bool sweep = (argc>6 && !strcmp(argv[6],"sweep")) || (argc>7 && !strcmp(argv[7],"sweep"));
    // VAVRA_HOLD: one attack, then hold -- so a capture can be compared with an
    // offline render of a held chord without the retriggers being the
    // difference between them.
    const bool hold = getenv("VAVRA_HOLD") != nullptr;
    const int sweepPresets[]={0,16,100,200,299};
    double windowSum=0; long windowCount=0; int windowIndex=0;
    // VAVRA_WAV captures exactly what render_block hands the host, so the
    // module's real path (fork, ring, gain, int16) can be compared against a
    // direct offline render of the same engine.
    const char* wavPath=getenv("VAVRA_WAV");
    std::vector<int16_t> capture;
    if(wavPath) capture.reserve(3446*256);
    double sum=0,maxRender=0; int peak=0; long count=0;
    auto next=Clock::now();
    for(int block=0;block<3446;++block) {
        if(block>0 && block%345==0) {
            if(sweep) {
                api->get_param(inst,"preset_name",value,sizeof(value));
                printf("second=%d preset=%d name=%-18s rms=%.1f\n",block/345,
                       sweepPresets[windowIndex%5],value,
                       windowCount?sqrt(windowSum/windowCount):0.0);
                windowSum=0; windowCount=0;
                ++windowIndex;
                char text[16]; snprintf(text,sizeof(text),"%d",sweepPresets[windowIndex%5]);
                api->set_param(inst,"preset",text);
            } else {
                api->get_param(inst,"diagnostics",value,sizeof(value));
                printf("second=%d %s\n",block/345,value);
            }
            fflush(stdout);
        }
        if(block>0 && block%345==0 && !sweep && !hold) {
            for(int i=0;i<voices;++i) { uint8_t msg[]={(uint8_t)(0x80|noteChannel),(uint8_t)notes[i],0}; api->on_midi(inst,msg,3,0); }
        }
        if(block>0 && block%345==10 && !hold) {
            // A held note does not re-voice on a patch change; retrigger so the
            // new sound is actually heard, as it would be when you play again.
            for(int i=0;i<voices;++i) { uint8_t msg[]={(uint8_t)(0x90|noteChannel),(uint8_t)notes[i],100}; api->on_midi(inst,msg,3,0); }
        }
        auto render=Clock::now(); api->render_block(inst,audio,128);
        maxRender=std::max(maxRender,seconds(render));
        int now=underrunsOf(api,inst);
        if(now!=seen) { printf("play_block=%d underruns=%d t=%.3f\n",block,now,seconds(start)); fflush(stdout); seen=now; }
        for(int16_t sample:audio) { peak=std::max(peak,abs((int)sample)); sum+=double(sample)*sample; ++count;
            windowSum+=double(sample)*sample; ++windowCount; }
        if(wavPath) capture.insert(capture.end(),audio,audio+256);
        next+=std::chrono::nanoseconds(2902494);
        std::this_thread::sleep_until(next);
    }
    for(int i=0;i<voices;++i) { uint8_t msg[]={(uint8_t)(0x80|noteChannel),(uint8_t)notes[i],0}; api->on_midi(inst,msg,3,0); }
    api->get_param(inst,"diagnostics",value,sizeof(value));
    const char* underrunField=strstr(value,"underruns=");
    const int underruns=underrunField ? atoi(underrunField+10) : -1;
    const int playUnderruns=underruns-underrunsBeforeNotes;
    printf("voices=%d peak=%d rms=%.2f max_render_us=%.1f play_underruns=%d %s\n",
           voices,peak,sqrt(sum/count),maxRender*1e6,playUnderruns,value);
    if(wavPath && !capture.empty()) {
        if(FILE* wav=fopen(wavPath,"wb")) {
            const uint32_t dataBytes=(uint32_t)(capture.size()*2);
            auto put32=[&](uint32_t v){ fwrite(&v,4,1,wav); };
            auto put16=[&](uint16_t v){ fwrite(&v,2,1,wav); };
            fwrite("RIFF",1,4,wav); put32(36+dataBytes); fwrite("WAVEfmt ",1,8,wav);
            put32(16); put16(1); put16(2); put32(44100); put32(44100*4); put16(4); put16(16);
            fwrite("data",1,4,wav); put32(dataBytes);
            fwrite(capture.data(),2,capture.size(),wav);
            fclose(wav);
            printf("wrote %s (%zu frames)\n",wavPath,capture.size()/2);
        }
    }
    if(api->get_param(inst,"writes",value,sizeof(value))>0) printf("final writes: %s\n",value);
    if(api->get_param(inst,"stalls",value,sizeof(value))>0) printf("%s\n",value);
    start=Clock::now(); api->destroy_instance(inst);
    printf("destroy_seconds=%.6f\n",seconds(start));
    dlclose(lib);
    // Only underruns inside the note window are audible; a stall in the silent
    // settle window costs nothing and is reported rather than failed on.
    return peak>100 && sqrt(sum/count)>10 && maxRender<0.01 && playUnderruns==0 ? 0 : 1;
}
