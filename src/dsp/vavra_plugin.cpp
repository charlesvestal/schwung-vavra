/* microQ for Schwung. GPL-3.0.
 * Osirus architecture: the emulator lives in a forked child; the host only
 * exchanges bounded messages and consumes a shared audio ring. */
#include "plugin_api.h"
#include "runtime.h"
#include "presets_os223.h"
#include "vavra_ui.h"
#include "mqLib/device.h"
#include "mqLib/mqmiditypes.h"
#include "mqLib/rom.h"
#include "dsp56kBase/logging.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdexcept>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// Osirus steps its DSP 64 samples at a time and this started at 256, which is
// 5.8 ms of audio in ONE step: a step that hits a cold JIT path took 16-18 ms,
// so a single one of them drained the whole 17 ms queue no matter how deep the
// queue was made. The step size, not the queue depth, is the ceiling on a
// stall.
constexpr int Chunk=256, TargetFillDefault=768;
// The queue trades latency for tolerance of a slow step, and with 4 voices at
// 100% clock roughly one step in ten exceeds its own realtime budget -- so the
// depth is the module's headroom dial, not a constant.
constexpr int TargetFillMin=512, TargetFillMax=6144;
// Warmup, in samples so that a change to Chunk cannot silently shorten it:
// at least 2 s of chord, retriggered every second, until 2 s pass with no step
// slower than StallUs, then a 1 s tail. The limit is the backstop -- it bounds
// how long a device that never settles can hold up the host's load.
constexpr int MinWarmBlocks=88200/Chunk, QuietBlocks=88200/Chunk;
constexpr int RetriggerBlocks=44100/Chunk, WarmupTailBlocks=44100/Chunk;
constexpr int WarmupBlockLimit=(44100*20)/Chunk;
// A step this slow is the JIT compiling, not the emulator running: steady state
// is ~4.6 ms for a 256-sample step and the queue only holds 17 ms.
constexpr uint32_t StallUs=8000;
// Measured on Move. At 100% eight voices underran 137-159 times a run; at 75%
// single-timbral play is clean but MULTI mode breaks past four total voices
// (15, 227, 272 at 4, 6, 8); at 50% every cell measured zero, including eight
// voices across two parts.
//
// It is free, because it does not change the SOUND. Rendered offline at 50,
// 75 and 100 and compared, the differences sit INSIDE the emulator's own
// run-to-run variation: two renders at the SAME clock differ more (rel 0.280,
// corr 0.963) than 75 differs from 100 (0.096, 0.997). mqLib runs the 68k in
// m_ucThread and the DSP in a DSPThread, so their interleaving -- not the
// clock -- is what moves the output, and no A/B of this emulator can be
// bit-exact.
constexpr int DspClockDefault=50;
// OS 2.23 ships 3 banks of 100. Names live in docs/presets-os223.tsv, dumped
// by tools/dump_presets.cpp -- but the module reads the CURRENT name off the
// device's own display rather than from any table, so a different ROM cannot
// make it lie.
constexpr int BankCount=3, PresetsPerBank=100;
// The Multi holds 16 instruments at a fixed 22-byte stride (MultiParameter::
// Inst0..Inst15). Part n answers on MIDI channel n by default.
constexpr int MultiParts=16, MultiInstStride=22;
// MultiParameter::Count -- the payload of a Multi dump, between the 7-byte
// header and the checksum.
constexpr int MultiDataBytes=384;
// The Single dump's payload, indexed the same way the parameter table is.
constexpr int SingleDataBytes=383;
// The gap between a part edit and the Multi dump that carries it.
constexpr int MultiSendDelayMs=300;
constexpr int PartSoundBankOffset=0, PartSoundNumberOffset=1,
              PartChannelOffset_=2, PartVolumeOffset=3;
// Inst<n>MidiChannel encodes MIDI channel c (1-based) as c+1: measured, part 1
// carries 2 and answers on channel 1. 0 and 1 are the global/omni settings.
constexpr int PartChannelOffset=2;
// How long the queue runs deep after a preset change, to swallow its JIT.
constexpr int BoostSeconds=4;
static_assert(sizeof(vavra::g_presetNames)/sizeof(*vavra::g_presetNames)==BankCount*PresetsPerBank,
    "the generated name table must cover every Program Change slot");
using Clock=std::chrono::steady_clock;
struct Stereo { int16_t l=0,r=0; };
struct Midi { uint16_t size=0; uint8_t bytes[1024]{}; };
// A firmware parameter write, queued for the child to emit as sysex. Generic
// on purpose: every Multi and Global parameter is addressed the same way, so
// exposing another one later costs a key, not a mechanism.
// A queued firmware write. `multi` selects between a Global parameter change
// and a byte of the Multi, which is not sent as a parameter change at all --
// see the child's sendMulti below.
// kind: 0 = Global parameter, 1 = a byte of the Multi, 2 = a Single parameter.
struct ParamWrite { uint16_t index=0; uint8_t value=0; uint8_t multi=0; };
struct Shared {
    vavra::Ring<Stereo,8192> audio;
    vavra::Ring<Midi,128> midi;
    vavra::Ring<ParamWrite,128> writes;
    std::atomic<int> ready{0}, failed{0}, clock{DspClockDefault}, gain{70}, targetFill{TargetFillDefault};
    // Preset selection is ONE index 0..299 in Program Change order, not a
    // bank plus a number: two params for one choice is two things to keep in
    // step, and the name table is indexed this way anyway.
    std::atomic<int> preset{0}, presetPending{1};
    // Multi mode: 16 parts, each with its own sound, channel and volume. The
    // firmware boots in SINGLE mode on omni, where every channel plays the one
    // sound -- which is why a slot's forward channel appears to do nothing.
    std::atomic<int> multiMode{0}, part{1}, multiReady{0};
    // The UI mode: 0 Play, 1 Edit, 2 Multi. Only Multi changes the firmware.
    std::atomic<int> uiMode{0};
    std::atomic<int> partPreset[MultiParts]{}, partChannel[MultiParts]{}, partVolume[MultiParts]{};
    char lcdName[24]{};
    // What the host last WROTE, verbatim, so a value we could not parse is
    // visible on the device instead of silently becoming option 0.
    char lastPresetWrite[64]{}, lastModeWrite[32]{};
    // The edit buffer's parameter bytes. A page repaint reads eight of these;
    // asking the emulator for each would be eight sysex round trips per frame.
    std::atomic<uint8_t> single[SingleDataBytes]{};
    std::atomic<int> singleReady{0};
    std::atomic<uint32_t> singleWrites{0}, singleQueued{0}, lastSingleIndex{9999}, lastSingleValue{0};
    std::atomic<uint32_t> underruns{0}, midiDrops{0}, blocks{0}, bootMs{0}, cpuMs{0};
    std::atomic<uint32_t> warmupBlocks{0};
    // Producer-side truth: the longest single emulator step, and how
    // empty the queue ever got. An underrun is one of these two, and
    // the consumer cannot tell which.
    std::atomic<uint32_t> maxProcUs{0}, warmupPeak{0};
    // Cumulative, so a per-second delta separates "the emulator ran slow"
    // (procUsTotal approaching wall time) from "the producer was not
    // scheduled" (few slow blocks, yet the queue still emptied).
    std::atomic<uint32_t> slowBlocks{0}, procMsTotal{0}, procUsCarry{0};
    // The first stalls, with the block they landed on: "when" is what separates
    // a cold JIT path at the first note from steady contention.
    std::atomic<uint32_t> stallBlock[16]{}, stallUs[16]{}, stalls{0};
    std::atomic<uint32_t> warmStallBlock[16]{}, warmStallUs[16]{}, warmStalls{0};
    char error[256]{};
};
struct Instance {
    Shared* shm=nullptr;
    pthread_t worker{};
    std::atomic<bool> stop{false};
    std::atomic<int> workerError{0};
    char moduleDir[1024]{};
};
static uint32_t cpuMs() {
    rusage r{}; getrusage(RUSAGE_SELF,&r);
    return (r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000 + (r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000;
}
static void setupWorker(const char* name) {
    sched_param sp{}; sched_setscheduler(0,SCHED_OTHER,&sp);
    cpu_set_t cpus; CPU_ZERO(&cpus);
    CPU_SET(0,&cpus); CPU_SET(1,&cpus); CPU_SET(2,&cpus);
    sched_setaffinity(0,sizeof(cpus),&cpus);
    pthread_setname_np(pthread_self(),name);
}
static synthLib::DeviceCreateParams loadRom(const char* moduleDir) {
    const std::string directory=std::string(moduleDir)+"/roms";
    DIR* dir=opendir(directory.c_str());
    if(!dir) throw std::runtime_error("No microQ ROM directory. Put OS 2.23 .bin or .mid in roms/.");
    std::vector<std::string> names;
    while(auto* entry=readdir(dir)) {
        const char* ext=strrchr(entry->d_name,'.');
        if(ext && (!strcasecmp(ext,".bin") || !strcasecmp(ext,".mid"))) names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names.begin(),names.end());
    for(const auto& name:names) {
        auto path=directory+"/"+name;
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file) continue;
        auto size=file.tellg();
        if(size<=0 || size>524288) continue;
        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.seekg(0); file.read(reinterpret_cast<char*>(data.data()),data.size());
        if(!file) continue;
        if(!strcasecmp(strrchr(name.c_str(),'.'),".mid")) {
            mqLib::ROM rom(path);
            if(!rom.isValid()) continue;
            data=rom.getData();
        }
        if(!vavra::normalizeRom(data)) continue;
        synthLib::DeviceCreateParams params;
        params.hostSamplerate=params.preferredSamplerate=44100;
        params.romData=std::move(data); params.romName=path;
        return params;
    }
    throw std::runtime_error(names.empty()
        ? "No microQ ROM found: add OS 2.23 .bin or .mid to roms/."
        : "ROM files found but rejected: expected 512 KiB OS 2.23 (either byte order), or a valid OS .mid.");
}
static void childMain(Instance* inst) {
    // Firmware boot logs thousands of flash writes; retain errors through SHM.
    Logging::setLogFunc([](const std::string&) {});
    // mc68k has a separate unconditional console logger. This child owns its
    // descriptor table, so redirecting it cannot silence the host process.
    int quiet=open("/dev/null",O_WRONLY);
    if(quiet>=0) { dup2(quiet,STDOUT_FILENO); dup2(quiet,STDERR_FILENO); close(quiet); }
    auto* shm=inst->shm;
    auto start=Clock::now();
    try {
        auto params=loadRom(inst->moduleDir);
        // Set this before construction so the 68k and DSP workers inherit the
        // same requested priority. Raising only the audio reader leaves its
        // producers vulnerable to scheduling stalls.
        sched_param realtime{}; realtime.sched_priority=20;
        sched_setscheduler(0,SCHED_FIFO,&realtime);
        // Construction boots the 68k and DSP. It must never run on the host callback.
        mqLib::Device device(params);
        if(!device.isValid()) throw std::runtime_error("microQ device rejected the firmware.");
        device.setExtraLatencySamples(0);
        int applied=shm->clock.load(); device.setDspClockPercent(applied);
        float input[Chunk]{}, output[6][Chunk]{};
        synthLib::TAudioInputs inputs{}; inputs[0]=input; inputs[1]=input;
        synthLib::TAudioOutputs outputs{};
        for(int c=0;c<6;++c) outputs[c]=output[c];
        std::vector<synthLib::SMidiEvent> midiIn,midiOut;
        midiIn.reserve(128); midiOut.reserve(128);
        // A cold first note compiles new JIT paths, and one of those compiles
        // costs 80 ms against a queue that holds 17 ms. Warm them here, where
        // the host still sees "loading" and every sample is discarded.
        //
        // The length cannot be a constant. A fixed 512 blocks left a storm of
        // 8-16 ms stalls in the first two seconds of real playing, because what
        // has to be compiled is not "a note" but note-on, voice re-allocation
        // and note-off arriving as separate events over seconds. So the warmup
        // plays a retriggering chord and runs until the emulator has been QUIET
        // -- no step slower than StallUs -- for QuietBlocks. It ends when the
        // JIT stops compiling rather than when a counter runs out.
        const uint8_t warmNotes[]={60,64,67,72,76,79,84,88};
        auto warmNotesEvent=[&](uint8_t status,uint8_t velocity){
            for(auto note:warmNotes)
                midiIn.emplace_back(synthLib::MidiEventSource::Host,status,note,velocity);
        };
        int lastStall=0, warmBlock=0;
        for(;warmBlock<WarmupBlockLimit;++warmBlock) {
            midiIn.clear(); midiOut.clear();
            // One chord per RetriggerBlocks, released just before the next, so
            // that note-on, voice stealing and note-off all compile in here.
            const int phase=warmBlock%RetriggerBlocks;
            if(phase==0) warmNotesEvent(0x90,100);
            else if(phase==RetriggerBlocks-8) warmNotesEvent(0x80,0);
            const auto warmStart=Clock::now();
            static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
            const auto warmUs=static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-warmStart).count());
            if(warmUs>StallUs) {
                lastStall=warmBlock;
                const auto slot=shm->warmStalls.fetch_add(1);
                if(slot<16) { shm->warmStallBlock[slot]=warmBlock; shm->warmStallUs[slot]=warmUs; }
            }
            float peak=0;
            for(int c=0;c<2;++c) for(int i=0;i<Chunk;++i) peak=std::max(peak,std::fabs(output[c][i]));
            shm->warmupPeak=std::max(shm->warmupPeak.load(),static_cast<uint32_t>(peak*10000.0f));
            if(warmBlock>=MinWarmBlocks && warmBlock-lastStall>=QuietBlocks) break;
        }
        // Silence the chord and let the release and effect tails run out, so the
        // queue is filled with silence rather than with the warmup's own audio.
        for(int block=0;block<WarmupTailBlocks;++block) {
            midiIn.clear(); midiOut.clear();
            if(!block) { warmNotesEvent(0x80,0);
                midiIn.emplace_back(synthLib::MidiEventSource::Host,0xb0,120,0); }
            static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
        }
        shm->warmupBlocks=warmBlock;
        // Pull the edit buffer's parameters into the shadow. Called at boot and
        // after every preset change: otherwise the grid would keep showing the
        // previous patch's values, which is worse than showing none.
        auto refreshSingle=[&]{
            synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
            request.sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                static_cast<uint8_t>(mqLib::SysexCommand::SingleRequest),
                static_cast<uint8_t>(mqLib::MidiBufferNum::SingleEditBufferSingleMode),
                static_cast<uint8_t>(mqLib::MidiSoundLocation::EditBufferCurrentSingle),0xf7};
            midiIn.clear(); midiIn.push_back(std::move(request));
            for(int block=0;block<2000;++block) {
                midiOut.clear();
                static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
                midiIn.clear();
                bool got=false;
                for(const auto& reply:midiOut) {
                    const auto& sysex=reply.sysex;
                    if(sysex.size()<static_cast<size_t>(mqLib::IdxSingleParamFirst)+SingleDataBytes) continue;
                    if(sysex[4]!=static_cast<uint8_t>(mqLib::SysexCommand::SingleDump)) continue;
                    for(int i=0;i<SingleDataBytes;++i)
                        shm->single[i]=sysex[mqLib::IdxSingleParamFirst+i];
                    shm->singleReady=1;
                    got=true;
                }
                if(got) break;
            }
        };

        // Ask for the Multi the firmware booted with, so edits start from the
        // real thing rather than from an assumption about it.
        std::vector<uint8_t> multiData;
        {
            synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
            request.sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                static_cast<uint8_t>(mqLib::SysexCommand::MultiRequest),
                static_cast<uint8_t>(mqLib::MidiBufferNum::MultiEditBuffer),0x00,0xf7};
            midiIn.clear(); midiIn.push_back(std::move(request));
            for(int block=0;block<2000 && multiData.empty();++block) {
                midiOut.clear();
                static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
                midiIn.clear();
                for(const auto& reply:midiOut) {
                    const auto& sysex=reply.sysex;
                    if(sysex.size()<static_cast<size_t>(mqLib::IdxMultiParamFirst)+MultiDataBytes) continue;
                    if(sysex[4]!=static_cast<uint8_t>(mqLib::SysexCommand::MultiDump)) continue;
                    multiData.assign(sysex.begin()+mqLib::IdxMultiParamFirst,
                                     sysex.begin()+mqLib::IdxMultiParamFirst+MultiDataBytes);
                }
            }
            shm->multiReady=multiData.size()==MultiDataBytes ? 1 : 0;
        }
        refreshSingle();
        shm->bootMs=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
        auto boostUntil=Clock::now();
        auto multiSendAt=Clock::now();
        Clock::time_point singleStale{};
        bool multiPending=false;
        const auto cpuStart=cpuMs();
        for(;;) {
            // The warmup can only compile the patch that was loaded at boot.
            // Selecting another one brings its own cold paths, which stall the
            // emulator on that patch's FIRST notes -- measured as 5-8 dropped
            // blocks, at every DSP clock, so it is not a capacity problem. The
            // queue is run deep for a few seconds after a change instead: the
            // extra audio is produced out of the ~20% steady-state headroom,
            // and latency returns to the user's setting once the JIT is quiet.
            uint32_t targetFill=static_cast<uint32_t>(shm->targetFill.load());
            if(Clock::now()<boostUntil) targetFill=std::min<uint32_t>(targetFill*4,TargetFillMax);
            if(shm->audio.available()>=targetFill) { usleep(500); continue; }
            int requested=shm->clock.load();
            if(requested!=applied) { device.setDspClockPercent(requested); applied=requested; }
            midiIn.clear(); midiOut.clear();
            // Bank select is CC 32 on this firmware. CC 0 is accepted and
            // ignored -- every bank then reads back as A001-A100.
            // Queued firmware writes. A Global parameter goes as a parameter
            // change; a Multi byte does NOT.
            //
            // Writing Inst<n>SoundBank/SoundNumber as MultiParameterChange
            // updates the Multi's DATA -- a read-back confirms the bytes land
            // exactly where intended -- and the parts still do not load those
            // sounds: measured, MIDI channel 1 then played instrument 4's
            // patch. mqLib's own createInitState does not use parameter
            // changes either; it sends a whole Multi. So the module keeps the
            // Multi, edits it, and sends it back as one dump.
            ParamWrite write;
            bool multiDirty=false;
            while(shm->writes.pop(write)) {
                if(write.multi==2) {
                    // Single parameter change. The packet carries a part byte
                    // before the index, which a Multi parameter change does
                    // not -- and in Multi mode that part byte is what makes
                    // the edit land on the selected instrument.
                    const int part=shm->multiMode.load()
                        ? std::clamp(shm->part.load(),1,MultiParts)-1 : 0;
                    synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                    event.sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                        static_cast<uint8_t>(mqLib::SysexCommand::SingleParameterChange),
                        static_cast<uint8_t>(part),
                        static_cast<uint8_t>(write.index>>7),static_cast<uint8_t>(write.index&0x7f),
                        write.value,0xf7};
                    midiIn.push_back(std::move(event));
                    if(write.index<SingleDataBytes) shm->single[write.index]=write.value;
                    shm->singleWrites.fetch_add(1);
                    shm->lastSingleIndex=write.index; shm->lastSingleValue=write.value;
                    continue;
                }
                if(write.multi) {
                    if(write.index<multiData.size()) { multiData[write.index]=write.value; multiDirty=true; }
                    continue;
                }
                synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                event.sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                    static_cast<uint8_t>(mqLib::SysexCommand::GlobalParameterChange),
                    static_cast<uint8_t>(write.index>>7),static_cast<uint8_t>(write.index&0x7f),
                    write.value,0xf7};
                midiIn.push_back(std::move(event));
                boostUntil=Clock::now()+std::chrono::seconds(BoostSeconds);
            }
            // Debounced, and deliberately NOT in the same block as the writes
            // that caused it. Sending the dump in the same process() call as
            // the Single/Multi mode change left every part edit with no
            // effect; the working sequence puts a gap between them. It also
            // coalesces a run of knob edits into one dump.
            if(multiDirty) multiSendAt=Clock::now()+std::chrono::milliseconds(MultiSendDelayMs);
            if(multiPending && Clock::now()>=multiSendAt && multiData.size()==MultiDataBytes) {
                synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                std::vector<uint8_t> sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                    static_cast<uint8_t>(mqLib::SysexCommand::MultiDump),
                    static_cast<uint8_t>(mqLib::MidiBufferNum::DeprecatedMultiBankInternal),0};
                sysex.insert(sysex.end(),multiData.begin(),multiData.end());
                uint8_t checksum=0;
                for(size_t i=4;i<sysex.size();++i) checksum+=sysex[i];
                sysex.push_back(checksum&0x7f);
                sysex.push_back(0xf7);
                event.sysex.assign(sysex.begin(),sysex.end());
                midiIn.push_back(std::move(event));
                multiPending=false;
                boostUntil=Clock::now()+std::chrono::seconds(BoostSeconds);
            }
            if(multiDirty) multiPending=true;
            // Single mode only: a Program Change in Multi would retarget
            // whatever the front panel has selected rather than the part.
            if(shm->presetPending.exchange(0)) {
                boostUntil=Clock::now()+std::chrono::seconds(BoostSeconds);
                const int index=std::clamp(shm->preset.load(),0,BankCount*PresetsPerBank-1);
                midiIn.emplace_back(synthLib::MidiEventSource::Host,0xb0,32,
                    static_cast<uint8_t>(index/PresetsPerBank));
                midiIn.emplace_back(synthLib::MidiEventSource::Host,0xc0,
                    static_cast<uint8_t>(index%PresetsPerBank),0);
                singleStale=Clock::now()+std::chrono::milliseconds(MultiSendDelayMs*2);
            }
            // Re-read the patch once it has settled, so the knobs show what
            // was actually loaded rather than the previous sound's values.
            //
            // The REQUEST is queued here and the reply is picked up by the
            // scan below, on ordinary frames. Draining it in place -- calling
            // process() in a loop until the dump arrives -- produced audio
            // that was never pushed to the queue, so every preset change tore
            // a hole in the stream. The battery caught it as every preset
            // matching its own reference three times worse than before.
            if(singleStale!=Clock::time_point{} && Clock::now()>=singleStale) {
                singleStale={};
                synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
                request.sysex={0xf0,0x3e,static_cast<uint8_t>(mqLib::IdMicroQ),0x7f,
                    static_cast<uint8_t>(mqLib::SysexCommand::SingleRequest),
                    static_cast<uint8_t>(mqLib::MidiBufferNum::SingleEditBufferSingleMode),
                    static_cast<uint8_t>(mqLib::MidiSoundLocation::EditBufferCurrentSingle),0xf7};
                midiIn.push_back(std::move(request));
            }
            Midi message;
            while(shm->midi.pop(message)) {
                synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                if(message.bytes[0]==0xf0) event.sysex.assign(message.bytes,message.bytes+message.size);
                else {
                    event.a=message.bytes[0];
                    event.b=message.size>1 ? message.bytes[1] : 0;
                    event.c=message.size>2 ? message.bytes[2] : 0;
                }
                midiIn.push_back(std::move(event));
            }
            const auto procStart=Clock::now();
            static_cast<synthLib::Device&>(device).process(inputs,outputs,Chunk,midiIn,midiOut);
            // The firmware streams its 2x20 display as SysexCommand::EmuLCD.
            // Row 2 is the current sound's 16-char name, which is the only
            // authority for it: the names are not plain text in the ROM.
            for(const auto& event:midiOut) {
                const auto& sysex=event.sysex;
                if(sysex.size()>=static_cast<size_t>(mqLib::IdxSingleParamFirst)+SingleDataBytes &&
                   sysex[4]==static_cast<uint8_t>(mqLib::SysexCommand::SingleDump)) {
                    for(int i=0;i<SingleDataBytes;++i)
                        shm->single[i]=sysex[mqLib::IdxSingleParamFirst+i];
                    shm->singleReady=1;
                }
                if(sysex.size()<46) continue;
                if(sysex[4]!=static_cast<uint8_t>(mqLib::SysexCommand::EmuLCD)) continue;
                // The whole 20-char row: the name field is inset by two
                // spaces, so copying only 16 from the row start dropped the
                // last two characters of every long name.
                char row[21]{};
                for(int i=0;i<20;++i) row[i]=static_cast<char>(sysex[5+20+i]);
                int end=20; while(end>0 && row[end-1]==' ') row[--end]=0;
                const char* start=row; while(*start==' ') ++start;
                memmove(row,start,strlen(start)+1);
                // Transient firmware messages ("[dumping Sound A001]") land on
                // row 1, never here, so row 2 needs no filtering.
                memcpy(shm->lcdName,row,sizeof(row));
            }
            const auto procUs=static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-procStart).count());
            if(procUs>shm->maxProcUs.load()) shm->maxProcUs=procUs;
            // A block is worth Chunk/44100 of wall; over that is slower than realtime.
            if(procUs>(Chunk*1000000)/44100) ++shm->slowBlocks;
            if(procUs>StallUs) { const auto slot=shm->stalls.fetch_add(1);
                if(slot<16) { shm->stallBlock[slot]=shm->blocks.load(); shm->stallUs[slot]=procUs; } }
            const auto carry=shm->procUsCarry.load()+procUs;
            shm->procMsTotal.fetch_add(carry/1000); shm->procUsCarry=carry%1000;
            float gain=shm->gain.load()*0.01f;
            for(int i=0;i<Chunk;++i) {
                Stereo sample;
                sample.l=static_cast<int16_t>(std::clamp(output[0][i]*gain*32767.0f,-32768.0f,32767.0f));
                sample.r=static_cast<int16_t>(std::clamp(output[1][i]*gain*32767.0f,-32768.0f,32767.0f));
                // A full ring never overwrites unread samples.
                shm->audio.push(sample);
            }
            auto block=shm->blocks.fetch_add(1)+1;
            if((block&255)==0) shm->cpuMs=cpuMs()-cpuStart;
            if(shm->audio.available()>=targetFill) shm->ready.store(1,std::memory_order_release);
        }
    } catch(const std::exception& error) {
        snprintf(shm->error,sizeof(shm->error),"%s",error.what());
        shm->failed.store(1,std::memory_order_release);
    } catch(...) {
        snprintf(shm->error,sizeof(shm->error),"Unknown microQ emulator failure");
        shm->failed.store(1,std::memory_order_release);
    }
}
static void* workerMain(void* context) {
    setupWorker("vavra-loader");
    auto* inst=static_cast<Instance*>(context);
    const pid_t parent=getpid();
    pid_t pid=fork();
    if(pid<0) { inst->workerError=1; return nullptr; }
    if(pid==0) {
        prctl(PR_SET_PDEATHSIG,SIGKILL);
        if(getppid()!=parent) _exit(1);
        pthread_setname_np(pthread_self(),"vavra-emu");
        childMain(inst); _exit(0);
    }
    auto start=Clock::now(), heartbeat=start;
    uint32_t lastBlock=0;
    while(!inst->stop.load()) {
        int status=0;
        if(waitpid(pid,&status,WNOHANG)==pid) {
            if(!inst->shm->failed.load()) inst->workerError=2;
            return nullptr;
        }
        auto now=Clock::now();
        uint32_t block=inst->shm->blocks.load();
        if(block!=lastBlock || inst->shm->audio.available()>=static_cast<uint32_t>(inst->shm->targetFill.load())) { heartbeat=now; lastBlock=block; }
        if(!inst->shm->ready.load() && now-start>std::chrono::seconds(90)) { inst->workerError=3; break; }
        if(inst->shm->ready.load() && now-heartbeat>std::chrono::seconds(10)) { inst->workerError=4; break; }
        usleep(5000);
    }
    kill(pid,SIGKILL); waitpid(pid,nullptr,0);
    return nullptr;
}
static void setParam(void*,const char*,const char*);
// An enum arrives as an INDEX from some callers and as the OPTION LABEL from
// others, and atoi() of a label is 0 -- which silently pins an enum to its
// first option. For `preset` that is a loud sub-bass patch and for `mode` it
// is Single, i.e. exactly "it only ever makes one sound and the mode switch
// does nothing". Accept both, and refuse anything else rather than guess.
static int parseEnum(const char* value,const char* const* options,int count) {
    if(!value || !*value) return -1;
    const char* scan=value;
    while(*scan==' ') ++scan;
    if(*scan=='-' || (*scan>='0' && *scan<='9')) {
        bool digits=true;
        for(const char* c=(*scan=='-'?scan+1:scan);*c;++c)
            if(*c<'0' || *c>'9') { digits=false; break; }
        if(digits) { const int index=atoi(scan); return (index>=0 && index<count) ? index : -1; }
    }
    for(int i=0;i<count;++i) if(!strcasecmp(scan,options[i])) return i;
    return -1;
}
// Play and Edit are two views of the same Single sound; Multi is the
// instrument's own other mode. So the UI mode has three values and the
// FIRMWARE mode has two, and this is the mapping between them.
static const char* const g_modeOptions[]={"Play","Edit","Multi"};
constexpr int UiModeMulti=2;
// The generated table is sorted by key, so this is a binary search.
//
// It was a linear strcmp over 278 entries, which a page repaint pays eight
// times -- about 2200 string compares -- and get_param IS the SPI audio
// callback. Nine compares now.
static const vavra::MqParam* findParam(const char* key) {
    size_t low=0, high=sizeof(vavra::g_mqParams)/sizeof(*vavra::g_mqParams);
    while(low<high) {
        const size_t mid=(low+high)/2;
        const int order=strcmp(key,vavra::g_mqParams[mid].key);
        if(!order) return &vavra::g_mqParams[mid];
        if(order<0) high=mid; else low=mid+1;
    }
    return nullptr;
}
static void* create(const char* directory,const char*) {
    if(!directory || strlen(directory)>=1024) return nullptr;
    auto* inst=new(std::nothrow) Instance;
    if(!inst) return nullptr;
    void* region=mmap(nullptr,sizeof(Shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(region==MAP_FAILED) { delete inst; return nullptr; }
    inst->shm=new(region) Shared;
    // Mirror the multi the firmware boots with (createInitState's "sequencer"
    // multi): part n on channel n, full volume, sounds A1 A2 A3 then A1.
    for(int slot=0;slot<MultiParts;++slot) {
        inst->shm->partChannel[slot]=slot+1;
        inst->shm->partVolume[slot]=127;
        inst->shm->partPreset[slot]=slot<3 ? slot : 0;
    }
    snprintf(inst->moduleDir,sizeof(inst->moduleDir),"%s",directory);
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr,SCHED_OTHER);
    sched_param normal{};
    pthread_attr_setschedparam(&attr,&normal);
    int result=pthread_create(&inst->worker,&attr,workerMain,inst);
    pthread_attr_destroy(&attr);
    if(result) { munmap(region,sizeof(Shared)); delete inst; return nullptr; }
    return inst;
}
static void destroy(void* context) {
    auto* inst=static_cast<Instance*>(context); if(!inst) return;
    inst->stop=true; pthread_join(inst->worker,nullptr);
    inst->shm->~Shared(); munmap(inst->shm,sizeof(Shared)); delete inst;
}
static void onMidi(void* context,const uint8_t* bytes,int size,int) {
    auto* inst=static_cast<Instance*>(context);
    if(!inst || !bytes || size<1 || size>1024 || !inst->shm->ready.load()) return;
    Midi message; message.size=size; memcpy(message.bytes,bytes,size);
    if(!inst->shm->midi.push(message)) ++inst->shm->midiDrops;
}
static void setParam(void* context,const char* key,const char* value) {
    auto* inst=static_cast<Instance*>(context); if(!inst || !key || !value) return;
    if(!strcmp(key,"dsp_clock")) inst->shm->clock=std::clamp(atoi(value),25,100);
    if(!strcmp(key,"gain")) inst->shm->gain=std::clamp(atoi(value),0,100);
    // Buffer in milliseconds: what the user actually trades away is latency.
    if(!strcmp(key,"buffer_ms")) inst->shm->targetFill=std::clamp(atoi(value)*44100/1000,TargetFillMin,TargetFillMax);
    // The child owns the MIDI; these only record the wish and wake it.
    if(!strcmp(key,"preset")) {
        snprintf(inst->shm->lastPresetWrite,sizeof(inst->shm->lastPresetWrite),"%s",value);
        const int index=parseEnum(value,vavra::g_presetNames,BankCount*PresetsPerBank);
        if(index<0) return;
        inst->shm->preset=index;
        if(inst->shm->multiMode.load()) {
            // Address the part HERE. This used to raise a "pending" flag and
            // let the child resolve the part when it drained -- so selecting a
            // sound for part 1 and then moving to part 2 landed BOTH writes on
            // part 2, and the part played a sound it was never given. Every
            // other part key already captured the part at write time; this one
            // has to as well.
            const int slot=std::clamp(inst->shm->part.load(),1,MultiParts)-1;
            inst->shm->partPreset[slot]=index;
            const int base=static_cast<int>(mqLib::MultiParameter::Inst0)+slot*MultiInstStride;
            inst->shm->writes.push({static_cast<uint16_t>(base+PartSoundBankOffset),
                                    static_cast<uint8_t>(index/PresetsPerBank),1});
            inst->shm->writes.push({static_cast<uint16_t>(base+PartSoundNumberOffset),
                                    static_cast<uint8_t>(index%PresetsPerBank),1});
        } else {
            inst->shm->presetPending=1;
        }
    }
    if(!strcmp(key,"mode")) {
        snprintf(inst->shm->lastModeWrite,sizeof(inst->shm->lastModeWrite),"%s",value);
        const int parsed=parseEnum(value,g_modeOptions,3);
        if(parsed<0) return;
        inst->shm->uiMode=parsed;
        const int multi=(parsed==UiModeMulti) ? 1 : 0;
        if(multi==inst->shm->multiMode.load()) return;   // Play <-> Edit is a UI move only
        inst->shm->multiMode=multi;
        inst->shm->writes.push({static_cast<uint16_t>(mqLib::GlobalParameter::SingleMultiMode),
                                static_cast<uint8_t>(multi),0});
    }
    // The part selector is the module's own: it says which part the part keys
    // below address, and writes nothing to the firmware.
    if(!strcmp(key,"part")) {
        const int slot=std::clamp(atoi(value),1,MultiParts);
        inst->shm->part=slot;
        inst->shm->preset=inst->shm->partPreset[slot-1].load();
    }
    // A generated synth parameter: map the shown value onto the wire and queue
    // it for the child, which owns the MIDI.
    if(const auto* mq=findParam(key)) {
        int raw=atoi(value)*mq->scale+mq->offset;
        raw=std::clamp(raw,static_cast<int>(mq->rawMin),static_cast<int>(mq->rawMax));
        inst->shm->writes.push({mq->index,static_cast<uint8_t>(raw),2});
        inst->shm->singleQueued.fetch_add(1);
        return;
    }
    auto partWrite=[&](int field,int raw){
        const int slot=std::clamp(inst->shm->part.load(),1,MultiParts)-1;
        const int offset=static_cast<int>(mqLib::MultiParameter::Inst0)+slot*MultiInstStride+field;
        inst->shm->writes.push({static_cast<uint16_t>(offset),static_cast<uint8_t>(raw),1});
        return slot;
    };
    if(!strcmp(key,"part_channel")) {
        // 0 selects the global channel; 1..16 are MIDI channels, which the
        // firmware stores offset by one (part 1 carries 2 and answers ch 1).
        const int channel=std::clamp(atoi(value),0,MultiParts);
        inst->shm->partChannel[partWrite(PartChannelOffset_,channel ? channel+PartChannelOffset-1 : 0)]=channel;
    }
    if(!strcmp(key,"part_volume")) {
        const int volume=std::clamp(atoi(value),0,127);
        inst->shm->partVolume[partWrite(PartVolumeOffset,volume)]=volume;
    }
}
static int getError(void* context,char* buffer,int size) {
    auto* inst=static_cast<Instance*>(context); if(!inst || !buffer || size<=0) return 0;
    if(inst->shm->failed.load(std::memory_order_acquire)) return snprintf(buffer,size,"%s",inst->shm->error);
    const char* errors[]={"","Could not fork microQ emulator","microQ emulator exited unexpectedly","microQ boot timed out (90 seconds)","microQ audio stalled"};
    int error=inst->workerError.load();
    if(error) return snprintf(buffer,size,"%s",errors[error]);
    buffer[0]=0; return 0;
}
static int getParam(void* context,const char* key,char* buffer,int size) {
    auto* inst=static_cast<Instance*>(context); if(!inst || !key || !buffer || size<=0) return -1;
    auto* s=inst->shm;
    if(!strcmp(key,"name")) return snprintf(buffer,size,"microQ");
    if(!strcmp(key,"loading") || !strcmp(key,"is_loading")) return snprintf(buffer,size,"%d",!s->ready.load() && !s->failed.load() && !inst->workerError.load());
    if(!strcmp(key,"chain_params")) {
        // The host's ceiling (SHADOW_PARAM_VALUE_LEN) REJECTS the whole module
        // when the answer is longer, so the write is bounded and reports
        // failure rather than truncating.
        const int used=snprintf(buffer,size,"[%s]",vavra::g_chainParamsRest);
        if(used>=size) { buffer[0]=0; return -1; }
        return used;
    }
    if(!strcmp(key,"ui_hierarchy")) return snprintf(buffer,size,"%s",vavra::g_uiHierarchy);
    if(!strcmp(key,"loading_status")) return snprintf(buffer,size,"%s",s->ready.load() ? "Ready" : "Booting microQ...");
    if(!strcmp(key,"dsp_clock")) return snprintf(buffer,size,"%d",s->clock.load());
    if(!strcmp(key,"gain")) return snprintf(buffer,size,"%d",s->gain.load());
    if(!strcmp(key,"buffer_ms")) return snprintf(buffer,size,"%d",s->targetFill.load()*1000/44100);
    // The preset page reads these three, one per tick: how many, which one,
    // and its name. No list ever crosses the wire.
    if(!strcmp(key,"preset")) return snprintf(buffer,size,"%d",s->preset.load());
    // The parts, as a list the UI can show. A selector knob that silently
    // re-points the rest of its own page is not a part chooser; this is one.
    // Each row carries the part's channel and the sound it holds.
    if(!strcmp(key,"part_list")) {
        int used=snprintf(buffer,size,"[");
        for(int slot=0;slot<MultiParts && used<size-64;++slot) {
            const int channel=s->partChannel[slot].load();
            const int index=std::clamp(s->partPreset[slot].load(),0,BankCount*PresetsPerBank-1);
            used+=snprintf(buffer+used,size-used,
                "%s{\"index\":%d,\"label\":\"%d ch%d %s\"}",
                slot?",":"",slot+1,slot+1,channel?channel:0,vavra::g_presetNames[index]);
        }
        used+=snprintf(buffer+used,size-used,"]");
        return used<size ? used : -1;
    }
    if(!strcmp(key,"preset_count")) return snprintf(buffer,size,"%d",BankCount*PresetsPerBank);
    if(!strcmp(key,"mode")) return snprintf(buffer,size,"%d",s->uiMode.load());
    if(!strcmp(key,"part")) return snprintf(buffer,size,"%d",s->part.load());
    if(!strcmp(key,"part_channel")) return snprintf(buffer,size,"%d",s->partChannel[std::clamp(s->part.load(),1,MultiParts)-1].load());
    if(!strcmp(key,"part_volume")) return snprintf(buffer,size,"%d",s->partVolume[std::clamp(s->part.load(),1,MultiParts)-1].load());
    // The LIVE name, off the device's own display -- so a ROM whose sounds
    // differ from the compiled-in table still reports the truth.
    if(const auto* mq=findParam(key)) {
        // Before the edit buffer has been read, a value would be a guess.
        if(!s->singleReady.load()) { buffer[0]=0; return 0; }
        const int raw=mq->index<SingleDataBytes ? s->single[mq->index].load() : 0;
        return snprintf(buffer,size,"%d",(raw-mq->offset)/(mq->scale?mq->scale:1));
    }
    if(!strcmp(key,"multi_ready")) return snprintf(buffer,size,"%d",s->multiReady.load());
    // A parameter write is queued by the host thread and applied by the child;
    // reading these back-to-back races them by design, so a zero here is only
    // news if it stays zero.
    if(!strcmp(key,"writes")) return snprintf(buffer,size,
        "preset=<%s> mode=<%s> queued=%u applied=%u last=%u/%u single_ready=%d",
        s->lastPresetWrite,s->lastModeWrite,s->singleQueued.load(),s->singleWrites.load(),
        s->lastSingleIndex.load(),s->lastSingleValue.load(),s->singleReady.load());
    // The name of what is SELECTED, from the compiled-in table. It used to come
    // off the front panel, which is right in Single mode and wrong in Multi --
    // there row 2 shows the MULTI's name ("From TUS with <3"), so the name
    // froze while the sound changed, which reads as "presets do nothing".
    if(!strcmp(key,"preset_name"))
        return snprintf(buffer,size,"%s",vavra::g_presetNames[std::clamp(s->preset.load(),0,BankCount*PresetsPerBank-1)]);
    if(!strcmp(key,"lcd_name")) return snprintf(buffer,size,"%s",s->lcdName);
    if(!strcmp(key,"stalls")) {
        int used=0;
        used+=snprintf(buffer+used,size-used,"warm_stalls=%u",s->warmStalls.load());
        for(uint32_t i=0;i<std::min<uint32_t>(16,s->warmStalls.load()) && used<size-24;++i)
            used+=snprintf(buffer+used,size-used," %u@%u",s->warmStallUs[i].load(),s->warmStallBlock[i].load());
        used+=snprintf(buffer+used,size-used," | stalls=%u",s->stalls.load());
        for(uint32_t i=0;i<std::min<uint32_t>(16,s->stalls.load()) && used<size-24;++i)
            used+=snprintf(buffer+used,size-used," %u@%u",s->stallUs[i].load(),s->stallBlock[i].load());
        return used;
    }
    if(!strcmp(key,"diagnostics")) return snprintf(buffer,size,"boot_ms=%u cpu_ms=%u blocks=%u underruns=%u midi_drops=%u warmup_blocks=%u warmup_peak=%u max_proc_us=%u slow_blocks=%u proc_ms=%u",s->bootMs.load(),s->cpuMs.load(),s->blocks.load(),s->underruns.load(),s->midiDrops.load(),s->warmupBlocks.load(),s->warmupPeak.load(),s->maxProcUs.load(),s->slowBlocks.load(),s->procMsTotal.load());
    buffer[0]=0; return -1;
}
static void render(void* context,int16_t* out,int frames) {
    if(!out || frames<=0) return;
    auto* inst=static_cast<Instance*>(context);
    bool ready=inst && inst->shm->ready.load(std::memory_order_acquire);
    bool missed=false;
    for(int i=0;i<frames;++i) {
        Stereo sample;
        if(ready && !inst->shm->audio.pop(sample)) missed=true;
        out[i*2]=sample.l; out[i*2+1]=sample.r;
    }
    if(missed) ++inst->shm->underruns;
}
plugin_api_v2_t api{2,create,destroy,onMidi,setParam,getParam,getError,render};
}
extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t*) { return &api; }
