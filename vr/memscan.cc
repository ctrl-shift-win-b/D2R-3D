// Read-only look at a running D2R.exe. Confirms the 3.3.93847 signatures
// once the image is decrypted, then lists direct callers of the camera
// and room functions. Does not write the process and does not inject.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Sig {
    const char* name;
    uint32_t rva;
    uint8_t bytes[16];
    int n;
};

const Sig kSigs[] = {
    {"CamCopy", 0x5F0710, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0x5D, 0x9A, 0x38, 0x01, 0x48, 0x33, 0xC4, 0x48, 0x89}, 16},
    {"CopyCall", 0x5F16D7, {0xE8, 0x34, 0xF0, 0xFF, 0xFF}, 5},
    {"GetProj", 0x8CE660, {0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x80, 0xB9, 0x60, 0x01, 0x00, 0x00, 0x00}, 16},
    {"ViewRebuild", 0x8CE830, {0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x78, 0xE8, 0x44, 0x0F}, 16},
    {"RayBuild", 0x8CEAC0, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81}, 16},
    {"ShadowFit", 0x952A70, {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x28}, 16},
    {"ActTest", 0x5203A0, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56}, 16},
    {"SightAdd", 0x2365D0, {0x40, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x45, 0x8B, 0xF1, 0x41, 0x8B, 0xD8, 0x0F}, 16},
    {"RoomInit", 0x2C2DF0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42, 0x50, 0x48, 0x8B, 0xDA}, 16},
    {"RoomRelease", 0x2F51A0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}, 16},
    {"TlsfInsert", 0xBB2960, {0x4C, 0x8B, 0x42, 0x08, 0x4C, 0x8B, 0xCA, 0x49, 0x83, 0xE0, 0xFC, 0x4C, 0x8B, 0xD9, 0x49, 0x81}, 16},
};

struct Target {
    const char* name;
    uint32_t rva;
};

// The second group are the functions that called the camera on the menu scan.
// Naming them here asks the next pass who calls those.
const Target kTargets[] = {
    {"CamCopy", 0x5F0710}, {"GetProj", 0x8CE660}, {"ViewRebuild", 0x8CE830},
    {"RayBuild", 0x8CEAC0}, {"ActTest", 0x5203A0}, {"SightAdd", 0x2365D0},
    {"RoomInit", 0x2C2DF0}, {"RoomRelease", 0x2F51A0},
    {"ActCam", 0x5204F6}, {"CopyFn", 0x5F0210}, {"MasterCopy", 0x5F163F},
    {"MouseRay", 0x60E970}, {"Ui8689", 0x8689B5}, {"Big86C6", 0x86C686},
    {"Big874B", 0x874BC0}, {"Big87B9", 0x87B940}, {"AfterRay", 0x8CEDD0},
    {"Mid8CF7", 0x8CF7D2}, {"ShadowCam", 0x94ECE3}, {"Pass9554", 0x955440},
    {"ActCall", 0x51F2C0}, {"ActBig", 0x51AA80}, {"ActWrap", 0x51A940}, {"Thunk87", 0x8B64A0},
    {"PassG", 0x957400}, {"RayWrap", 0x76F3C}, {"Tiny63", 0x63F9C0},
    {"TickCam", 0x7CF90}, {"TickSmall", 0x7D8F0}, {"TickSight", 0x7DA80},
    {"FrameBig", 0x7E550}, {"SightRoot", 0xED1B0}, {"WalkRoot", 0x522430},
    {"SightFar", 0xF7E20}, {"ActWalkA", 0x522960}, {"ActWalkB", 0x522ED7},
    {"Wrap6C", 0x6C4620}, {"WrapA0", 0xA0341},
};

struct RuntimeFn {
    uint32_t begin, end;
};

DWORD FindPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof pe};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"D2R.exe") == 0) pid = pe.th32ProcessID;
    }
    CloseHandle(snap);
    return pid;
}

uintptr_t ModuleBase(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{sizeof me};
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) base = (uintptr_t)me.modBaseAddr;
    CloseHandle(snap);
    return base;
}

bool Read(HANDLE p, uintptr_t addr, void* dst, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(p, (void*)addr, dst, n, &got) && got == n;
}

template <class T>
bool ReadT(HANDLE p, uintptr_t addr, T& o) {
    return Read(p, addr, &o, sizeof o);
}

int MatchCount(HANDLE p, uintptr_t base) {
    int n = 0;
    for (const Sig& s : kSigs) {
        uint8_t got[16] = {};
        if (Read(p, base + s.rva, got, s.n) && memcmp(got, s.bytes, s.n) == 0) ++n;
    }
    return n;
}

void ReportSigs(HANDLE p, uintptr_t base) {
    for (const Sig& s : kSigs) {
        uint8_t got[16] = {};
        const bool ok = Read(p, base + s.rva, got, s.n) && memcmp(got, s.bytes, s.n) == 0;
        std::printf("sig %-12s rva 0x%X %s", s.name, s.rva, ok ? "ok" : "MISS");
        if (!ok) {
            std::printf(" got");
            for (int i = 0; i < s.n && i < 8; ++i) std::printf(" %02X", got[i]);
        }
        std::printf("\n");
    }
}

const char* TargetName(uint32_t rva) {
    for (const Target& t : kTargets)
        if (t.rva == rva) return t.name;
    return nullptr;
}

uint32_t FunctionOf(const std::vector<RuntimeFn>& fns, uint32_t rva);

uint32_t FunctionSize(const std::vector<RuntimeFn>& fns, uint32_t begin) {
    size_t lo = 0, hi = fns.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (fns[mid].begin <= begin) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0 || fns[lo - 1].begin != begin || fns[lo - 1].end <= begin) return 0;
    return fns[lo - 1].end - begin;
}

void ScanAbsPtrs(uint32_t secVa, const std::vector<uint8_t>& buf, const std::vector<uint8_t>& pages, uintptr_t imageBase) {
    int hits = 0;
    for (uint32_t i = 0; i + 8 <= buf.size(); i += 8) {
        if (i / 4096 >= pages.size() || !pages[i / 4096]) continue;
        const uint64_t q = *reinterpret_cast<const uint64_t*>(buf.data() + i);
        if (q < imageBase) continue;
        const uint64_t rva64 = q - imageBase;
        if (rva64 > 0x2000000ull) continue;
        const char* name = TargetName(static_cast<uint32_t>(rva64));
        if (!name) continue;
        std::printf("ptr %-12s at 0x%X\n", name, secVa + i);
        if (++hits >= 100) {
            std::printf("ptr list truncated\n");
            return;
        }
    }
    std::printf("ptrs %d in 0x%X\n", hits, secVa);
}

void ScanSlot40(const std::vector<uint8_t>& code, const std::vector<uint8_t>& readable, uint32_t textVa, const std::vector<RuntimeFn>& fns) {
    struct Acc { uint32_t fn; int n; };
    std::vector<Acc> acc;
    int total = 0;
    for (uint32_t i = 0; i + 6 < code.size(); ++i) {
        if (i / 4096 >= readable.size() || !readable[i / 4096]) continue;
        const bool shortCall = code[i] == 0xFF && code[i + 1] >= 0x50 && code[i + 1] <= 0x57 && code[i + 2] == 0x40;
        const bool disp32Call = code[i] == 0xFF && code[i + 1] == 0x90 && code[i + 2] == 0x40 && code[i + 3] == 0 && code[i + 4] == 0 && code[i + 5] == 0;
        if (!shortCall && !disp32Call) continue;
        ++total;
        const uint32_t fn = FunctionOf(fns, textVa + i);
        if (!fn) continue;
        bool found = false;
        for (Acc& a : acc) {
            if (a.fn == fn) { ++a.n; found = true; break; }
        }
        if (!found) acc.push_back(Acc{fn, 1});
    }
    std::sort(acc.begin(), acc.end(), [](const Acc& a, const Acc& b) { return a.n > b.n; });
    std::printf("slot40 calls %d functions %zu\n", total, acc.size());
    const size_t show = acc.size() < 30 ? acc.size() : 30;
    for (size_t i = 0; i < show; ++i)
        std::printf("slot40 fn 0x%X size %u hits %d\n", acc[i].fn, FunctionSize(fns, acc[i].fn), acc[i].n);
}

uint32_t FunctionOf(const std::vector<RuntimeFn>& fns, uint32_t rva) {
    size_t lo = 0, hi = fns.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (fns[mid].begin <= rva) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return 0;
    const RuntimeFn& f = fns[lo - 1];
    return rva < f.end ? f.begin : 0;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    DWORD pid = 0;
    for (int i = 0; i < 90 && !pid; ++i) {
        pid = FindPid();
        if (!pid) Sleep(2000);
    }
    if (!pid) {
        std::printf("no D2R.exe after 180s\n");
        return 2;
    }
    std::printf("pid %lu\n", pid);

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        std::printf("OpenProcess failed %lu\n", GetLastError());
        return 3;
    }
    uintptr_t base = 0;
    int matched = 0;
    int held = 0;
    const int sigCount = (int)(sizeof kSigs / sizeof kSigs[0]);
    for (int i = 0; i < 60; ++i) {
        base = ModuleBase(pid);
        if (base) matched = MatchCount(proc, base);
        std::printf("poll %d base 0x%llX matched %d/%d\n", i, (unsigned long long)base, matched, sigCount);
        if (matched >= sigCount) break;
        if (matched >= 8 && ++held >= 8) break;
        Sleep(2000);
    }
    if (!base) {
        std::printf("no module base\n");
        CloseHandle(proc);
        return 4;
    }
    ReportSigs(proc, base);
    if (matched < 8) {
        std::printf("image not decrypted enough to map callers\n");
        CloseHandle(proc);
        return 5;
    }

    uint32_t e_lfanew = 0;
    if (!ReadT(proc, base + 0x3C, e_lfanew)) {
        std::printf("pe header unreadable\n");
        CloseHandle(proc);
        return 6;
    }
    const uintptr_t nt = base + e_lfanew;
    uint16_t magic = 0, nsect = 0, optsize = 0;
    ReadT(proc, nt + 4 + 20, magic);  // optional magic is at start of optional header: nt+24
    ReadT(proc, nt + 24, magic);
    ReadT(proc, nt + 4 + 2, nsect);
    ReadT(proc, nt + 4 + 16, optsize);
    std::printf("magic 0x%X sections %u opt %u\n", magic, nsect, optsize);

    struct Sec {
        char name[9];
        uint32_t va, vsize;
    };
    Sec text{}, rdata{}, data{};
    const uintptr_t secTable = nt + 24 + optsize;
    for (uint16_t i = 0; i < nsect; ++i) {
        uint8_t raw[40] = {};
        if (!Read(proc, secTable + i * 40, raw, 40)) continue;
        char name[9] = {};
        memcpy(name, raw, 8);
        const uint32_t vsize = *(uint32_t*)(raw + 8);
        const uint32_t va = *(uint32_t*)(raw + 12);
        std::printf("section %-8s va 0x%X size 0x%X\n", name, va, vsize);
        if (strcmp(name, ".text") == 0) text = Sec{ {}, va, vsize };
        else if (strcmp(name, ".rdata") == 0) rdata = Sec{ {}, va, vsize };
        else if (strcmp(name, ".data") == 0) data = Sec{ {}, va, vsize };
    }
    if (!text.vsize || text.vsize > 64u * 1024u * 1024u) {
        std::printf("no usable .text\n");
        CloseHandle(proc);
        return 7;
    }

    uint32_t excRva = 0, excSize = 0;
    // DataDirectory[3] is the exception directory. Optional header data dirs start at offset 112 for PE32+.
    if (magic == 0x20B) {
        ReadT(proc, nt + 24 + 112 + 3 * 8, excRva);
        ReadT(proc, nt + 24 + 112 + 3 * 8 + 4, excSize);
    }
    std::printf("pdata rva 0x%X size 0x%X\n", excRva, excSize);
    std::vector<RuntimeFn> fns;
    if (excRva && excSize && excSize < 16u * 1024u * 1024u && excSize % 12 == 0) {
        std::vector<uint8_t> pdata(excSize);
        if (Read(proc, base + excRva, pdata.data(), pdata.size())) {
            fns.reserve(excSize / 12);
            for (uint32_t i = 0; i + 12 <= excSize; i += 12) {
                RuntimeFn f;
                f.begin = *(uint32_t*)(pdata.data() + i);
                f.end = *(uint32_t*)(pdata.data() + i + 4);
                fns.push_back(f);
            }
        }
    }
    std::printf("functions %zu\n", fns.size());

    std::vector<uint8_t> code(text.vsize, 0);
    std::vector<uint8_t> readable((text.vsize + 4095) / 4096, 0);
    uint32_t badPages = 0;
    for (uint32_t off = 0; off < text.vsize; off += 4096) {
        const uint32_t n = std::min<uint32_t>(4096, text.vsize - off);
        if ((off / 4096) % 512 == 0)
            std::printf("text page %u / %u\n", off / 4096, (text.vsize + 4095) / 4096);
        if (Read(proc, base + text.va + off, code.data() + off, n)) readable[off / 4096] = 1;
        else ++badPages;
    }
    std::printf("text pages unreadable %u\n", badPages);
    {
        bool in = false;
        uint32_t start = 0;
        int spans = 0;
        std::printf("unreadable");
        for (uint32_t p = 0; p <= readable.size(); ++p) {
            const bool bad = p < readable.size() && !readable[p];
            if (bad && !in) { start = p; in = true; }
            if (!bad && in) {
                if (p - start >= 8 && spans < 24) {
                    std::printf(" 0x%X-0x%X", text.va + start * 4096, text.va + p * 4096);
                    ++spans;
                }
                in = false;
            }
        }
        std::printf(" spans %d\n", spans);
    }

    auto readSpan = [&](uint32_t va, uint32_t size, const char* label) {
        struct Out { std::vector<uint8_t> bytes, pages; };
        Out o;
        if (!va || !size || size > 64u * 1024u * 1024u) {
            std::printf("%s skipped\n", label);
            return o;
        }
        o.bytes.assign(size, 0);
        o.pages.assign((size + 4095) / 4096, 0);
        uint32_t bad = 0;
        for (uint32_t off = 0; off < size; off += 4096) {
            const uint32_t n = std::min<uint32_t>(4096, size - off);
            if (Read(proc, base + va + off, o.bytes.data() + off, n)) o.pages[off / 4096] = 1;
            else ++bad;
        }
        std::printf("%s pages unreadable %u of %zu\n", label, bad, o.pages.size());
        return o;
    };
    const auto rd = readSpan(rdata.va, rdata.vsize, "rdata");
    const auto dd = readSpan(data.va, data.vsize, "data");
    CloseHandle(proc);
    ScanAbsPtrs(rdata.va, rd.bytes, rd.pages, base);
    ScanAbsPtrs(data.va, dd.bytes, dd.pages, base);

    int calls = 0;
    int shown = 0;
    int perTarget[sizeof kTargets / sizeof kTargets[0]] = {};
    for (uint32_t i = 0; i + 5 < text.vsize; ++i) {
        if (code[i] != 0xE8 && code[i] != 0xE9) continue;
        if (!readable[i / 4096]) continue;
        if ((i + 4) / 4096 != i / 4096 && !readable[(i + 4) / 4096]) continue;
        const int32_t rel = *(int32_t*)(code.data() + i + 1);
        const uint32_t site = text.va + i;
        const uint32_t next = site + 5;
        const uint32_t target = next + (uint32_t)rel;
        const char* name = TargetName(target);
        if (!name) continue;
        for (size_t t = 0; t < sizeof kTargets / sizeof kTargets[0]; ++t)
            if (kTargets[t].rva == target) ++perTarget[t];
        const uint32_t fn = FunctionOf(fns, site);
        if (shown < 2000) {
            std::printf("%s %-12s from 0x%X fn 0x%X size %u\n", code[i] == 0xE8 ? "call" : "jmp ", name, site, fn, fn ? FunctionSize(fns, fn) : 0);
            ++shown;
        }
        ++calls;
    }
    std::printf("direct calls %d shown %d\n", calls, shown);
    ScanSlot40(code, readable, text.va, fns);
    for (size_t t = 0; t < sizeof kTargets / sizeof kTargets[0]; ++t)
        std::printf("count %-12s %d\n", kTargets[t].name, perTarget[t]);
    if (matched < (int)(sizeof kSigs / sizeof kSigs[0]))
        std::printf("room pages still encrypted at the menu\n");
    return 0;
}
