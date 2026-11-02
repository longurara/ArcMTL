"""Run production MTL MMIO setup and LRC initialization against fake registers.

Checks generation-specific CS_DEBUG_MODE2 writes and temporary MCR steering.
Requires C++17; use CXX=cl in a Windows developer command prompt.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "kexts/ArcXe/ArcXeExec.cpp").read_text(encoding="utf-8")
service = (ROOT / "kexts/ArcXe/ArcXeService.cpp").read_text(encoding="utf-8")
header = (ROOT / "kexts/ArcXe/ArcXeService.h").read_text(encoding="utf-8")
start = header.index("struct ArcExecQueue {")
queue = header[start:header.index("\n};", start) + 3]
start = source.index("void ArcXeService::armEngineHw(")
arm = source[start:source.index("\nbool ArcXeService::resetEngineHw", start)]
start = service.index("void ArcXeService::applyXeLpgWorkarounds(")
workarounds = service[start:service.index("\nbool ArcXeService::invalidateGlobalTlb", start)]
start = source.index("    static const struct { uint32_t instr; uint16_t numDw; } kXeHpgSvgState[]")
init = source[start:source.index("    /* Flush all 76 KiB", start)]

STUBS = r'''
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "arc_uapi.h"
#include "arc_regs_mtl.h"
#define ARC_MASKED_ENABLE(x) (((x) << 16) | (x))
#define ARC_MASKED_DISABLE(x) ((x) << 16)
#define AXELOG(...) ((void)0)
struct ArcBo { uint32_t ggttOffset = 0x1800000; };
struct IOLock {};
static int locks=0;
static void IOLockLock(IOLock *) { assert(locks++ == 0); }
static void IOLockUnlock(IOLock *) { assert(--locks == 0); }
'''

SERVICE = r'''
struct ArcXeService {
    IOLock *fEngineLock = nullptr;
    bool wake = true, checkingSteering = false;
    std::map<uint32_t, uint32_t> regs;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    uint32_t rd32(uint32_t r) {
        if (checkingSteering && (r == ARC_REG_MTL_MCR_SELECTOR || r == ARC_REG_XEHP_L3SQCREG5))
            assert(locks == 1);
        return regs[r];
    }
    void wr32(uint32_t r, uint32_t v) {
        if (checkingSteering && r == ARC_REG_MTL_MCR_SELECTOR) assert(locks == 1);
        regs[r]=v; writes.emplace_back(r,v);
    }
    bool forceWakeGet() { return wake; }
    void armEngineHw(uint32_t, ArcBo *);
    void applyXeLpgWorkarounds();
    void init(ArcExecQueue *q, bool isRcs, bool isIdleKernelCtx) {
        const uint32_t base=isRcs ? ARC_RENDER_RING_BASE : ARC_BLT_RING_BASE;
        auto reg=q->ctxRegsCpu;
'''

CASES = r'''
static void checkDebugWrites(const ArcXeService &s, unsigned expected) {
    unsigned found=0;
    for (auto w:s.writes) if (w.first == 0x20d8) {
        assert(w.second == 0x00100010u); ++found;
    }
    assert(found == expected);
}
static void checkInit(uint32_t selector, bool wake, bool render, bool idle) {
    ArcXeService s;
    s.wake=wake; s.checkingSteering=true;
    s.regs[ARC_REG_MTL_MCR_SELECTOR]=selector;
    s.regs[ARC_REG_XEHP_L3SQCREG5]=0x002003aa;
    uint32_t ring[4096]={}, ctx[128]={};
    ArcExecQueue q={}; q.ringCpu=ring; q.ctxRegsCpu=ctx;
    s.init(&q, render, idle);
    assert(locks == 0 && s.regs[ARC_REG_MTL_MCR_SELECTOR] == selector);
    if (wake && render) {
        assert(s.writes.size() == 2);
        assert(s.writes[0].first == ARC_REG_MTL_MCR_SELECTOR && s.writes[0].second == 0);
        assert(s.writes[1].first == ARC_REG_MTL_MCR_SELECTOR && s.writes[1].second == selector);
    } else assert(s.writes.empty());
    assert(q.isIdleKernelCtx == idle && q.holdsForceWake == wake);
    assert(q.initRingTail == q.ringTail && !(q.ringTail & 7));
    unsigned debug=0, tuning=0, rcc=0;
    for (uint32_t i=0; i<q.initRingTail/4;) {
        const auto h=ring[i];
        if ((h >> 23) == 0x22) {
            const unsigned words=(h & 0xff)+2;
            for (unsigned j=1; j<words; j+=2) {
                if ((h & ARC_MI_LRI_CS_MMIO) && ring[i+j] == 0xd8) {
                    assert(ring[i+j+1] == 0x00100010u); ++debug;
                }
                if (ring[i+j] == ARC_REG_XEHP_L3SQCREG5) {
                    assert(ring[i+j+1] == 0x0020007f); ++tuning;
                }
                if (ring[i+j] == ARC_REG_COMMON_SLICE_CHICKEN3) {
                    assert(ring[i+j+1] == 0x20002000u); ++rcc;
                }
            }
            i+=words;
        } else if ((h >> 29) == 3) i+=(h & 0xff)+2;
        else { assert(h == ARC_MI_NOOP || h == (ARC_MI_ARB_ON_OFF | ARC_MI_ARB_ENABLE)); ++i; }
        assert(i <= q.initRingTail/4);
    }
    assert(debug == (render ? 1u:0u) && tuning == debug && rcc == debug);
}
int main() {
    ArcXeService s; ArcBo hwsp;
    s.armEngineHw(ARC_RENDER_RING_BASE, &hwsp);
    checkDebugWrites(s,1);
    s.applyXeLpgWorkarounds();
    checkDebugWrites(s,2);
    for (uint32_t selector : {0u, 0x80000000u, 0x80000102u})
        for (bool wake : {false,true})
            for (bool render : {false,true})
                for (bool idle : {false,true}) checkInit(selector,wake,render,idle);
    std::puts("PASS: production MTL arm/workarounds/LRC init, bit4 only, MCR selector and lock restoration");
}
'''

with tempfile.TemporaryDirectory(prefix="arcxe-init-test-") as directory:
    path = Path(directory)
    cpp = path / "test.cpp"
    binary = path / ("test.exe" if os.name == "nt" else "test")
    cpp.write_text(STUBS + queue + SERVICE + init + "\n}\n};\n" + arm + workarounds + CASES,
                   encoding="utf-8")
    compiler = os.environ.get("CXX", "c++")
    if Path(compiler).stem.lower() == "cl":
        args = [compiler, "/nologo", "/std:c++17", "/EHsc", f"/I{ROOT / 'include'}", str(cpp), f"/Fe:{binary}"]
    else:
        args = [compiler, "-std=c++17", "-I", str(ROOT / "include"), str(cpp), "-o", str(binary)]
    subprocess.run(args, check=True, cwd=path)
    subprocess.run([str(binary)], check=True, timeout=10)
