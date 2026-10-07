#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace vavra {
// Osirus's single-producer/single-consumer ring, with explicit publication on ARM.
template<class T, uint32_t N> struct Ring {
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    T data[N]{};
    std::atomic<uint32_t> read{0}, write{0};
    uint32_t available() const {
        return (write.load(std::memory_order_acquire)+N-read.load(std::memory_order_acquire))%N;
    }
    bool push(const T& item) {
        auto w=write.load(std::memory_order_relaxed), next=(w+1)%N;
        if(next==read.load(std::memory_order_acquire)) return false;
        data[w]=item; write.store(next,std::memory_order_release); return true;
    }
    bool pop(T& item) {
        auto r=read.load(std::memory_order_relaxed);
        if(r==write.load(std::memory_order_acquire)) return false;
        item=data[r]; read.store((r+1)%N,std::memory_order_release); return true;
    }
};
inline bool normalizeRom(std::vector<uint8_t>& bytes) {
    if(bytes.size()!=524288) return false;
    if(!memcmp(bytes.data(),"2.23",4)) return true;
    if(memcmp(bytes.data(),".232",4)) return false;
    for(size_t i=0;i<bytes.size();i+=2) std::swap(bytes[i],bytes[i+1]);
    return true;
}

// ---- State blob helpers -------------------------------------------------
// The host's state is one small JSON object, written by get_param("state") and
// read back by set_param("state"). The parser is deliberately tiny: it only
// ever reads what stateBlob() wrote, finds keys by their quoted name plus the
// colon, and rejects anything that is not exactly the expected shape.
inline std::string toHex(const uint8_t* bytes,size_t count) {
    static const char digits[]="0123456789abcdef";
    std::string out; out.reserve(count*2);
    for(size_t i=0;i<count;++i) { out+=digits[bytes[i]>>4]; out+=digits[bytes[i]&15]; }
    return out;
}
// The value of `"key":<int>`, or `fallback` when the key is absent.
inline int jsonInt(const char* json,const char* key,int fallback) {
    if(!json || !key) return fallback;
    const std::string needle=std::string("\"")+key+"\":";
    const char* at=strstr(json,needle.c_str());
    if(!at) return fallback;
    at+=needle.size();
    if(*at!='-' && (*at<'0' || *at>'9')) return fallback;
    return atoi(at);
}
// `"key":"<hex>"` decoded into out[0..count). False unless the string is
// EXACTLY 2*count hex digits and every byte is 7-bit: sysex data bytes with
// the top bit set would end the message early inside the firmware.
inline bool jsonHex(const char* json,const char* key,uint8_t* out,size_t count) {
    if(!json || !key || !out) return false;
    const std::string needle=std::string("\"")+key+"\":\"";
    const char* at=strstr(json,needle.c_str());
    if(!at) return false;
    at+=needle.size();
    auto nibble=[](char c)->int {
        if(c>='0' && c<='9') return c-'0';
        if(c>='a' && c<='f') return c-'a'+10;
        if(c>='A' && c<='F') return c-'A'+10;
        return -1;
    };
    for(size_t i=0;i<count;++i) {
        const int high=nibble(at[i*2]);
        if(high<0) return false;
        const int low=nibble(at[i*2+1]);
        if(low<0) return false;
        const int value=high*16+low;
        if(value>0x7f) return false;
        out[i]=static_cast<uint8_t>(value);
    }
    return at[count*2]=='"';
}
}
