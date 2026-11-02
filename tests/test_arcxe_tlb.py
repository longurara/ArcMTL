"""Execute production global TLB invalidation with fake MMIO and lock assertions."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'kexts/ArcXe/ArcXeService.cpp').read_text(encoding='utf-8')
start = source.index('bool ArcXeService::invalidateGlobalTlb(')
body = source[start:source.index('\n/* ===', start)]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>
#include "arc_regs_mtl.h"
#define AXELOG(...) ((void)0)
struct IOLock {};
static int locks, delay;
static void IOLockLock(IOLock *) { assert(locks++ == 0); }
static void IOLockUnlock(IOLock *) { assert(--locks == 0); }
static void IODelay(unsigned us) { delay += us; }
struct ArcXeService {
    IOLock lock;
    IOLock *fEngineLock = &lock;
    void *fRcsHwspBo = nullptr, *fBcsHwspBo = nullptr;
    bool wake = true;
    int refs = 0;
    uint32_t stuck = 0, selector = 0x80000102;
    std::map<uint32_t, unsigned> polls;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    bool forceWakeGet() { assert(locks == 1); if (wake) ++refs; return wake; }
    void forceWakePut() { assert(refs-- == 1); }
    uint32_t rd32(uint32_t reg) {
        assert(locks == 1 && refs == 1);
        if (reg == ARC_REG_MTL_MCR_SELECTOR) return selector;
        assert(reg == 0xced8 || reg == 0xcee4);
        assert(selector == ARC_MCR_MULTICAST);
        return ++polls[reg] < 3 || reg == stuck ? 1 : 0;
    }
    void wr32(uint32_t reg, uint32_t value) {
        assert(locks == 1 && refs == 1);
        writes.emplace_back(reg,value);
        if (reg == ARC_REG_MTL_MCR_SELECTOR) selector = value;
        else {
            assert(reg == 0xced8 || reg == 0xcee4);
            assert(value == 1 && selector == ARC_MCR_MULTICAST);
            assert(reg == 0xced8 ? fRcsHwspBo != nullptr : fBcsHwspBo != nullptr);
        }
    }
    bool invalidateGlobalTlb(bool = false);
};
'''
CASES = r'''
int main() {
    for (unsigned mask=0; mask<4; ++mask)
    for (bool callerLocked : {false,true})
    for (bool wake : {false,true})
    for (uint32_t stuck : {0u,0xced8u,0xcee4u}) {
        ArcXeService s;
        s.fRcsHwspBo = mask & 1 ? &s : nullptr;
        s.fBcsHwspBo = mask & 2 ? &s : nullptr;
        s.wake = wake; s.stuck = stuck; delay = 0;
        if (callerLocked) IOLockLock(s.fEngineLock);
        const bool ok = s.invalidateGlobalTlb(callerLocked);
        assert(locks == (callerLocked ? 1 : 0));
        if (callerLocked) IOLockUnlock(s.fEngineLock);
        const bool timeout = (stuck == 0xced8 && (mask & 1)) || (stuck == 0xcee4 && (mask & 2));
        assert(ok == (wake && !timeout));
        assert(s.refs == 0 && s.selector == 0x80000102);
        if (!wake) { assert(s.writes.empty() && delay == 0); continue; }
        assert(s.writes.size() == 2 + !!(mask & 1) + !!(mask & 2));
        assert(s.polls[0xced8] == (!(mask & 1) ? 0u : stuck == 0xced8 ? 400u : 3u));
        assert(s.polls[0xcee4] == (!(mask & 2) ? 0u : stuck == 0xcee4 ? 400u : 3u));
        assert(delay <= 4040);
    }
    std::puts("PASS: production TLB active engines, requests, timeout, forcewake, steering and nested-lock paths");
}
'''
with tempfile.TemporaryDirectory(prefix='arcxe-tlb-test-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    binary = path / ('test.exe' if os.name == 'nt' else 'test')
    cpp.write_text(HARNESS + body + CASES, encoding='utf-8')
    compiler = os.environ.get('CXX', 'c++')
    if Path(compiler).stem.lower() == 'cl':
        args = [compiler, '/nologo', '/std:c++17', '/EHsc', f'/I{ROOT / "include"}', str(cpp), f'/Fe:{binary}']
    else:
        args = [compiler, '-std=c++17', '-I', str(ROOT / 'include'), str(cpp), '-o', str(binary)]
    subprocess.run(args, check=True, cwd=path)
    subprocess.run([str(binary)], check=True, timeout=10)
