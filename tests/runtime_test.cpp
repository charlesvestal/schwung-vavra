#include "dsp/runtime.h"
#include "dsp/vavra_ui.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>

int main() {
    vavra::Ring<int, 8> ring;
    int value = -1;
    assert(!ring.pop(value));
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 7; ++i) assert(ring.push(round * 7 + i));
        assert(!ring.push(-1));
        for (int i = 0; i < 7; ++i) {
            assert(ring.pop(value));
            assert(value == round * 7 + i);
        }
        assert(!ring.pop(value));
    }
    std::vector<uint8_t> rom(524288);
    rom[0]='2'; rom[1]='.'; rom[2]='2'; rom[3]='3';
    auto original = rom;
    assert(vavra::normalizeRom(rom));
    assert(rom == original);
    for (size_t i=0; i<rom.size(); i+=2) std::swap(rom[i],rom[i+1]);
    assert(vavra::normalizeRom(rom));
    assert(rom == original);
    rom[0]=0;
    assert(!vavra::normalizeRom(rom));
    rom.resize(4);
    assert(!vavra::normalizeRom(rom));
    vavra::Ring<uint32_t,1024> concurrent;
    std::thread producer([&] {
        for(uint32_t i=0;i<500000;++i) while(!concurrent.push(i)) std::this_thread::yield();
    });
    for(uint32_t i=0;i<500000;++i) {
        uint32_t next;
        while(!concurrent.pop(next)) std::this_thread::yield();
        assert(next==i);
    }
    producer.join();
    // The plugin BINARY SEARCHES the generated parameter table, so an entry
    // out of order does not fail loudly -- it makes some keys unfindable, and
    // those knobs quietly stop working.
    {
        const size_t count = sizeof(vavra::g_mqParams) / sizeof(*vavra::g_mqParams);
        for (size_t i = 1; i < count; ++i)
            assert(strcmp(vavra::g_mqParams[i - 1].key, vavra::g_mqParams[i].key) < 0 &&
                   "vavra_ui.h parameter table must be sorted by key");
        for (size_t i = 0; i < count; ++i) {
            size_t low = 0, high = count;
            bool found = false;
            while (low < high) {
                const size_t mid = (low + high) / 2;
                const int order = strcmp(vavra::g_mqParams[i].key, vavra::g_mqParams[mid].key);
                if (!order) { found = true; break; }
                if (order < 0) high = mid; else low = mid + 1;
            }
            assert(found && "every parameter must be reachable by binary search");
        }
        printf("PASS: %zu parameters sorted and reachable by binary search\n", count);
    }
    // State blob helpers: round trip, and every way a damaged blob must fail.
    {
        uint8_t bytes[383];
        for (int i = 0; i < 383; ++i) bytes[i] = static_cast<uint8_t>(i % 128);
        const std::string hex = vavra::toHex(bytes, sizeof(bytes));
        assert(hex.size() == 766);
        const std::string json = "{\"v\":1,\"preset\":42,\"mode\":2,\"gain\":150,\"single\":\"" + hex + "\",\"multi\":\"00\"}";
        assert(vavra::jsonInt(json.c_str(), "v", 0) == 1);
        assert(vavra::jsonInt(json.c_str(), "preset", -1) == 42);
        assert(vavra::jsonInt(json.c_str(), "mode", -1) == 2);
        assert(vavra::jsonInt(json.c_str(), "gain", -1) == 150);
        assert(vavra::jsonInt(json.c_str(), "absent", -7) == -7);
        // "preset" must not be satisfied by a longer key that starts with it.
        assert(vavra::jsonInt("{\"preset_name\":5}", "preset", -1) == -1);
        uint8_t back[383]{};
        assert(vavra::jsonHex(json.c_str(), "single", back, 383));
        assert(!memcmp(bytes, back, 383));
        assert(!vavra::jsonHex(json.c_str(), "single", back, 382));     // wrong length
        assert(!vavra::jsonHex(json.c_str(), "single", back, 384));     // wrong length
        assert(!vavra::jsonHex(json.c_str(), "missing", back, 383));
        assert(!vavra::jsonHex("{\"single\":\"zz\"}", "single", back, 1));   // not hex
        assert(!vavra::jsonHex("{\"single\":\"80\"}", "single", back, 1));   // 8-bit sysex byte
        assert(!vavra::jsonHex("{\"single\":\"7f", "single", back, 1));       // truncated
        assert(vavra::jsonHex("{\"single\":\"7F\"}", "single", back, 1) && back[0] == 0x7f);
        puts("PASS: state blob round trip and rejection of damaged blobs");
    }
    puts("PASS: ring wrap/full/empty, concurrent publication, and ROM normalization");
}
