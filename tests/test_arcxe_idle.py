"""Exercise production idle parking, submission gate and timeout snapshots with fake MMIO.

No GPU required. Hardware context-save/cache behaviour still needs a macOS run.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'kexts/ArcXe/ArcXeExec.cpp').read_text()
header = (ROOT / 'kexts/ArcXe/ArcXeService.h').read_text()
start = header.index('struct ArcExecQueue {')
queue = header[start:header.index('\n};', start) + 3]
start = source.index('static inline void ringEmitDword(')
helpers = source[start:source.index('\nstatic inline void readEngineDiagMmio(', start)]
start = source.index('IOReturn ArcXeService::submitBatch(')
gate = source[start:source.index('\n/* Called under fEngineLock', start)]
start = source.index('static void captureTimeoutDetail(')
capture = source[start:source.index('\nIOReturn ArcXeService::waitQueueSeqno(', start)]
start = source.index('    /* Bootstrap the parking contexts')
bootstrap = source[start:source.index('\n}\n', start)]
start = source.index('bool ArcXeService::resetEngineHw(')
reset = source[start:source.index('\nbool ArcXeService::initEngines()', start)]
reset = reset.replace('ArcXeService::resetEngineHw(', 'ArcXeService::resetHardware(')

STUBS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include "arc_uapi.h"
#include "arc_regs_mtl.h"
#define bzero(p,n) std::memset(p,0,n)
#define AXELOG(...) ((void)0)
#define ARC_MASKED_ENABLE(x) (((x) << 16) | (x))
#define ARC_MASKED_DISABLE(x) ((x) << 16)
using IOReturn = int;
using SInt32 = int32_t;
static int32_t OSIncrementAtomic(volatile int32_t *p) { return ++*p; }
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotResponding, kIOReturnDeviceError };
struct ArcBo { unsigned size=256; };
struct IOLock {};
static unsigned delayUs=0;
static int locks=0;
static void IODelay(unsigned n) { delayUs+=n; }
static void IOLockLock(IOLock *) { ++locks; }
static void IOLockUnlock(IOLock *) { --locks; }
static void clflushRange(volatile void *, unsigned) {}
'''

SERVICE = r'''
struct ArcXeService {
    IOLock *fEngineLock=nullptr;
    ArcExecQueue *fBcsIdleQueue=nullptr, *fRcsIdleQueue=nullptr;
    ArcExecQueue *fLastBcsQueue=nullptr, *fLastRcsQueue=nullptr;
    bool fStopping=false, wakeOk=true, resetOk=true;
    uint32_t fNextSwCtxId=1;
    int wakes=0, resets=0;
    mutable unsigned loads=0, faultReads=0;
    mutable std::map<uint32_t,uint32_t> regs;
    // 0 success, 1 no idle fence, 2 wrong LRCA, 3 idle ring not drained.
    int mode=0;
    uint32_t rd32(uint32_t reg) const {
        if (reg==ARC_REG_XEHP_RING_FAULT || reg==ARC_REG_XEHP_FAULT_TLB_DATA0 ||
            reg==ARC_REG_XEHP_FAULT_TLB_DATA1) {
            assert(regs[ARC_REG_MTL_MCR_SELECTOR]==0); ++faultReads;
        }
        return regs[reg];
    }
    void wr32(uint32_t reg, uint32_t value) const {
        regs[reg]=value;
        if (reg==ARC_REG_GDRST && resetOk) regs[reg]=0;
        for (uint32_t base : {ARC_RENDER_RING_BASE, ARC_BLT_RING_BASE}) {
            if (reg!=ARC_RING_EXECLIST_CONTROL(base)) continue;
            ++loads;
            ArcExecQueue *idle=base==ARC_RENDER_RING_BASE ? fRcsIdleQueue : fBcsIdleQueue;
            if ((regs[ARC_RING_EXECLIST_SQ_LO(base)] & ~0xfffu)!=idle->pphwspGgttAddr) return;
            if (mode!=1) *idle->seqnoCpu=idle->lastSubmittedSeqno;
            regs[ARC_RING_CURRENT_LRCA(base)]=mode==2 ? 0 : idle->pphwspGgttAddr;
            regs[ARC_RING_HEAD(base)]=idle->ringTail | (mode==3 ? 4u : 0x200000u);
            regs[ARC_RING_TAIL(base)]=idle->ringTail;
        }
    }
    bool forceWakeGet() { if (!wakeOk) return false; ++wakes; return true; }
    void forceWakePut() { --wakes; }
    uint32_t engineMmioBase(uint16_t, uint16_t, ArcBo **bo) { static ArcBo hwsp; *bo=&hwsp; return 0; }
    bool resetEngineHw(uint32_t, ArcBo *) { ++resets; return resetOk; }
    bool resetHardware(uint32_t, ArcBo *);
    void armEngineHw(uint32_t, ArcBo *) {}
    void programMtlMocsTable() {}
    void applyXeLpgWorkarounds() {}
    bool parkEngineOnIdleContextLocked(uint32_t, ArcExecQueue *);
    bool bootstrap();
    IOReturn submitBatch(ArcExecQueue *, uint64_t, uint64_t *);
};
static void readEngineDiagMmio(const ArcXeService *, uint32_t, ArcXeWaitQueueOut *) {}
static void readRcsSubunitDiag(const ArcXeService *, uint32_t,
                              uint32_t &, uint32_t &, uint32_t &, uint32_t &, uint32_t &) {}
struct Fixture {
    ArcXeService svc;
    ArcExecQueue prev={}, idle={};
    ArcBo bo;
    uint32_t ring[64]={}, context[128]={};
    uint32_t prevRing[64]={}, prevContext[128]={};
    uint64_t fence=0, prevFence=4;
    uint32_t base;
    Fixture(uint32_t engine=ARC_RENDER_RING_BASE) : base(engine) {
        idle.lrcBo=idle.fenceBo=&bo; idle.ringCpu=ring; idle.ctxRegsCpu=context;
        idle.seqnoCpu=&fence; idle.ringSize=sizeof(ring); idle.pphwspGgttAddr=0x20000;
        idle.seqnoGgttAddr=0x40000;
        prev.lrcBo=prev.fenceBo=&bo; prev.ringCpu=prevRing; prev.ctxRegsCpu=prevContext;
        prev.seqnoCpu=&prevFence; prev.lastSubmittedSeqno=4; prev.isLoadedOnHw=true;
        prev.mmioBase=base; prev.ringSize=sizeof(prevRing); prev.ringTail=64;
        prev.engineClass=base==ARC_RENDER_RING_BASE ? kArcXeEngineClassRender : kArcXeEngineClassCopy;
        prev.pphwspGgttAddr=0x50000; prev.swCtxId=17;
        prevContext[ARC_CTX_PDP0_LDW_VAL]=0x12345001;
        if (base==ARC_RENDER_RING_BASE) { svc.fRcsIdleQueue=&idle; svc.fLastRcsQueue=&prev; }
        else { svc.fBcsIdleQueue=&idle; svc.fLastBcsQueue=&prev; }
        svc.regs[ARC_RING_HEAD(base)]=0x200040; // wrap counter is not an address
        svc.regs[ARC_RING_TAIL(base)]=64;
        delayUs=0;
    }
};
'''

CASES = r'''
int main() {
    // Production reset reboots idle state only after the hardware reset ACK.
    for (uint32_t base : {ARC_RENDER_RING_BASE, ARC_BLT_RING_BASE}) {
        for (bool resetOk : {false, true}) {
            for (int mode : {0, 1}) {
                Fixture f(base); f.svc.resetOk=resetOk; f.svc.mode=mode;
                f.idle.hasSavedHwState=true; f.idle.isLoadedOnHw=true;
                f.idle.initRingTail=32; f.idle.ringTail=128;
                assert(f.svc.resetHardware(base,&f.bo)==(resetOk && mode==0));
                assert(f.prev.faulted && !f.idle.hasSavedHwState);
                assert(f.idle.isLoadedOnHw==(resetOk && mode==0));
                assert(f.svc.loads==(resetOk ? 1u : 0u));
                assert(f.context[ARC_CTX_RING_HEAD_VAL]==0);
                assert(f.idle.ringTail==(resetOk ? 56u : 32u));
            }
        }
    }
    // Execute the production bootstrap before any user queue can be loaded.
    for (bool wake : {false, true}) {
        for (int mode=0; mode<=3; ++mode) {
            Fixture f; f.svc.wakeOk=wake; f.svc.mode=mode;
            f.svc.fLastRcsQueue=nullptr;
            assert(f.svc.bootstrap()==(wake && mode==0));
            assert(!locks && !f.svc.wakes);
            assert(f.idle.isLoadedOnHw==(wake && mode==0));
        }
    }
    for (uint32_t base : {ARC_RENDER_RING_BASE, ARC_BLT_RING_BASE}) {
        for (unsigned tail=0; tail<256; tail+=8) {
            Fixture f(base); f.idle.ringTail=tail;
            f.idle.lastSubmittedSeqno=0xffffffffull;
            assert(f.svc.parkEngineOnIdleContextLocked(base,&f.prev));
            assert(f.prev.hasSavedHwState && !f.prev.isLoadedOnHw && f.prev.idleParkCount==1);
            assert(f.idle.isLoadedOnHw && f.fence==0x100000000ull && f.svc.loads==1);
            assert(!f.svc.fLastRcsQueue && !f.svc.fLastBcsQueue);
            unsigned start=256-tail<64 ? 0 : tail;
            assert(f.ring[start/4]==(ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_USE_GGTT | ARC_MI_SDI_QWORD_LEN));
            assert(f.ring[start/4+1]==f.idle.seqnoGgttAddr);
            assert(f.ring[start/4+3]==0 && f.ring[start/4+4]==1);
            assert(f.ring[start/4+5]==ARC_MI_USER_INTERRUPT && f.idle.ringTail==start+24);
        }
        // A saved idle context must retain its hardware-written control and
        // HEAD. This fails when every park forcibly sets RESTORE_INHIBIT.
        { Fixture f(base); f.idle.hasSavedHwState=true;
          f.context[ARC_CTX_CONTEXT_CONTROL_VAL]=0xffff0008;
          f.context[ARC_CTX_RING_HEAD_VAL]=0x88;
          assert(f.svc.parkEngineOnIdleContextLocked(base,&f.prev));
          assert(f.context[ARC_CTX_CONTEXT_CONTROL_VAL]==0xffff0008);
          assert(f.context[ARC_CTX_RING_HEAD_VAL]==0x88); }
        for (int mode=1; mode<=3; ++mode) {
            Fixture f(base); f.svc.mode=mode;
            assert(!f.svc.parkEngineOnIdleContextLocked(base,&f.prev));
            assert(f.prev.isLoadedOnHw && !f.prev.hasSavedHwState && !f.prev.idleParkCount);
            assert(!f.idle.isLoadedOnHw && delayUs>=10000 && delayUs<=260000);
            assert((base==ARC_RENDER_RING_BASE ? f.svc.fLastRcsQueue : f.svc.fLastBcsQueue)==&f.prev);
        }
        Fixture f(base); f.svc.regs[ARC_RING_HEAD(base)]=32;
        assert(!f.svc.parkEngineOnIdleContextLocked(base,&f.prev));
        assert(f.svc.loads==0 && !f.idle.lastSubmittedSeqno && !f.idle.ringTail && delayUs==10000);
    }
    { Fixture f; f.svc.fRcsIdleQueue=nullptr;
      assert(!f.svc.parkEngineOnIdleContextLocked(f.base,&f.prev));
      assert(f.svc.fLastRcsQueue==&f.prev && !f.svc.loads); }
    // Real submit: drained RCS parks then fully restores without overwriting PDP;
    // BCS keeps its software context ID and uses lite restore.
    for (uint32_t base : {ARC_RENDER_RING_BASE, ARC_BLT_RING_BASE}) {
        for (int mode=0; mode<=3; ++mode) {
            Fixture f(base); f.svc.mode=mode;
            const bool ok=base==ARC_BLT_RING_BASE || mode==0;
            assert(f.svc.submitBatch(&f.prev,0,nullptr)==(ok ? kIOReturnSuccess : kIOReturnNotResponding));
            assert(!locks && !f.svc.wakes && f.prev.lastSubmittedSeqno==(ok ? 5u : 4u) && f.prevFence==4);
            assert(f.svc.resets==(ok ? 0 : 1) && f.prev.faulted==!ok);
            assert(f.prev.idleParkCount==(base==ARC_RENDER_RING_BASE && mode==0 ? 1u : 0u));
            if (ok) {
                assert(f.prevContext[ARC_CTX_PDP0_LDW_VAL]==0x12345001);
                assert(f.prev.swCtxId==(base==ARC_RENDER_RING_BASE ? 2u : 17u));
                assert(f.svc.loads==(base==ARC_RENDER_RING_BASE ? 2u : 1u));
                assert(f.prev.isLoadedOnHw && !f.idle.isLoadedOnHw);
                assert(f.idle.hasSavedHwState==(base==ARC_RENDER_RING_BASE));
            }
        }
        Fixture f(base); f.svc.regs[ARC_RING_HEAD(base)]=32;
        assert(f.svc.submitBatch(&f.prev,0,nullptr)==kIOReturnNotResponding);
        assert(f.prev.faulted && f.svc.resets==1 && !f.svc.loads && !locks && !f.svc.wakes);
    }
    // Production timeout capture must restore MCR steering and wrap ring reads.
    Fixture f;
    f.svc.regs[ARC_REG_MTL_MCR_SELECTOR]=0x80000102;
    f.svc.regs[ARC_REG_XEHP_RING_FAULT]=0x123;
    f.svc.regs[ARC_REG_XEHP_FAULT_TLB_DATA0]=0x456;
    f.svc.regs[ARC_REG_XEHP_FAULT_TLB_DATA1]=0x789;
    f.svc.regs[ARC_REG_L3ALLOC]=0x200;
    f.svc.regs[ARC_REG_XEHP_L3SQCREG5]=0xe0007f;
    for (unsigned i=0; i<5; ++i) f.svc.regs[ARC_REG_PAT_INDEX(i)]=i+10;
    for (unsigned i=0; i<64; ++i) f.prevRing[i]=i;
    f.prevContext[ARC_CTX_CONTEXT_CONTROL_VAL]=0x111;
    f.prevContext[ARC_CTX_RING_HEAD_VAL]=0x222;
    f.prevContext[ARC_CTX_RING_TAIL_VAL]=0x333;
    f.prev.idleParkCount=9;
    ArcXeWaitQueueOut snap={}; snap.ring_head=0x200008;
    ArcXeTimeoutDetail d={};
    captureTimeoutDetail(&f.svc,&f.prev,snap,nullptr);
    assert(!f.svc.faultReads);
    captureTimeoutDetail(&f.svc,&f.prev,snap,&d);
    assert(d.valid && d.fault_reg==0x123 && d.fault_data0==0x456 && d.fault_data1==0x789);
    assert(f.svc.faultReads==3 && f.svc.regs[ARC_REG_MTL_MCR_SELECTOR]==0x80000102);
    assert(d.ring_offset==232 && d.ring_count==16);
    for (unsigned i=0; i<16; ++i) assert(d.ring_dw[i]==((58+i)%64));
    assert(d.context_control==0x111 && d.context_head==0x222 && d.context_tail==0x333 && d.idle_park_count==9);
    assert(d.hw.version==1 && d.hw.l3_alloc==0x200 && d.hw.l3_sqcreg5==0xe0007f);
    for (unsigned i=0; i<5; ++i) assert(d.hw.pat[i]==i+10);
    f.prev.mmioBase=ARC_BLT_RING_BASE;
    captureTimeoutDetail(&f.svc,&f.prev,snap,&d);
    assert(d.valid && !d.hw.version);
    f.prev.ringCpu=nullptr; f.prev.ctxRegsCpu=nullptr;
    captureTimeoutDetail(&f.svc,&f.prev,snap,&d);
    assert(d.valid && !d.ring_count && !d.context_control);
    std::puts("PASS: production idle parking, wrap, failure gates, reset and fault/ring snapshots");
}
'''

with tempfile.TemporaryDirectory(prefix='arcxe-idle-test-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    binary = path / ('test.exe' if os.name == 'nt' else 'test')
    cpp.write_text(STUBS + queue + SERVICE + helpers + gate + capture + reset +
                   'bool ArcXeService::bootstrap() {\n' + bootstrap + '\n}\n' + CASES)
    compiler = os.environ.get('CXX', 'c++')
    if Path(compiler).stem.lower() == 'cl':
        args = [compiler, '/nologo', '/std:c++17', '/EHsc', f'/I{ROOT / "include"}', str(cpp), f'/Fe:{binary}']
    else:
        args = [compiler, '-std=c++17', '-I', str(ROOT / 'include'), str(cpp), '-o', str(binary)]
    subprocess.run(args, check=True, cwd=path)
    subprocess.run([str(binary)], check=True, timeout=10)
