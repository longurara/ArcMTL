"""Run the production fence-wait function against fake clock/MMIO/reset hooks.

This checks timeout/recovery semantics, not whether a real GPU executes a batch.
Requires C++17; use CXX=cl in a Windows developer command prompt.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "kexts/ArcXe/ArcXeExec.cpp").read_text(encoding="utf-8")
start = source.index("IOReturn ArcXeService::waitQueueSeqno(")
function = source[start:source.index("\n/* ====", start)]
header = (ROOT / "kexts/ArcXe/ArcXeService.h").read_text(encoding="utf-8")
start = header.index("struct ArcExecQueue {")
queue = header[start:header.index("\n};", start) + 3]

STUBS = r'''
#include <cassert>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include "arc_uapi.h"
#include "arc_regs_mtl.h"
static_assert(sizeof(ArcXeSyncobjWaitOut) == 96, "syncobj diagnostic ABI must stay compatible");
static_assert(sizeof(ArcXeSyncobjWaitOutExtended) == 280, "extended ABI");
static_assert(offsetof(ArcXeSyncobjWaitOutExtended, detail) + offsetof(ArcXeTimeoutDetail, hw) == 200, "v1 extended prefix");
static_assert(offsetof(ArcXeSyncobjWaitOutExtended, detail) == 96, "legacy prefix");
#define bzero(p, n) std::memset(p, 0, n)
#define AXELOG(...) ((void)0)
#define ARC_MASKED_ENABLE(x) (((x) << 16) | (x))
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotResponding };
struct ArcBo {};
struct IOLock {};
static void (*onLock)() = nullptr;
static int locks = 0, wakes = 0, resets = 0;
static uint64_t clockTick = 0;
static void IOLockLock(IOLock *) { ++locks; if (onLock) { auto fn=onLock; onLock=nullptr; fn(); } }
static void IOLockUnlock(IOLock *) { --locks; }
static uint64_t mach_absolute_time() { return clockTick++; }
static void absolutetime_to_nanoseconds(uint64_t value, uint64_t *out) { *out = value; }
static void IODelay(unsigned) {}
static void IOSleep(unsigned) {}
static void clflushRange(volatile void *, unsigned) {}
'''

SERVICE = r'''
struct ArcXeService {
    IOLock *fEngineLock = nullptr;
    ArcExecQueue *fBcsIdleQueue=nullptr, *fRcsIdleQueue=nullptr;
    ArcExecQueue *fLastBcsQueue=nullptr, *fLastRcsQueue=nullptr;
    bool forceWakeGet() { ++wakes; return true; }
    void forceWakePut() { --wakes; }
    uint32_t engineMmioBase(uint16_t, uint16_t, ArcBo **bo) {
        static ArcBo hwsp; *bo = &hwsp; return 0;
    }
    bool resetEngineHw(uint32_t, ArcBo *) { ++resets; return true; }
    IOReturn waitQueueSeqno(ArcExecQueue *, uint64_t, uint64_t, uint64_t *, bool *,
                           ArcXeWaitQueueOut * = nullptr, bool = true, uint32_t * = nullptr, uint32_t * = nullptr, ArcXeTimeoutDetail * = nullptr);
};
static void readEngineDiagMmio(const ArcXeService *, uint32_t, ArcXeWaitQueueOut *d) { d->ipehr=0x7a000404; }
static void readRcsSubunitDiag(const ArcXeService *, uint32_t,
                             uint32_t &, uint32_t &, uint32_t &, uint32_t &, uint32_t &) {}
static int capturedBeforeReset = -1;
static void captureTimeoutDetail(const ArcXeService *, const ArcExecQueue *,
                                 const ArcXeWaitQueueOut &, ArcXeTimeoutDetail *d) {
    if (d) { d->valid=1; capturedBeforeReset=resets; }
}
'''

CASES = r'''
static uint64_t fencePage[512] = {4};
static uint64_t &completed = fencePage[0];
static ArcExecQueue q = {};
static void completeOnLock() { completed = 5; }
static void faultOnLock() { q.faulted = true; }
int main() {
    ArcXeService svc;
    q.seqnoCpu = &completed; q.lastSubmittedSeqno = 5;
    uint64_t done=0; bool timedOut=false; ArcXeWaitQueueOut diag={};
    // Polling timeouts must not discard work or signal its fence.
    assert(svc.waitQueueSeqno(&q, 5, 0, &done, &timedOut, nullptr, false) == kIOReturnSuccess);
    assert(timedOut && done == 4 && completed == 4 && !q.faulted && resets == 0);
    // A real timeout discards the queue; it must not manufacture completion.
    // Only the begin marker reached this submission; the others are from #4.
    fencePage[16]=5; fencePage[17]=4; fencePage[18]=4; fencePage[8]=4;
    uint32_t progress=0; ArcXeTimeoutDetail detail={};
    assert(svc.waitQueueSeqno(&q, 5, 0, &done, &timedOut, &diag, true, nullptr, &progress, &detail) == kIOReturnSuccess);
    assert(progress == (ARCXE_PROGRESS_VALID | ARCXE_PROGRESS_BEGIN));
    assert(detail.valid && capturedBeforeReset == 0);
    assert(timedOut && diag.timed_out && diag.completed_seqno == 4);
    assert(completed == 4 && q.faulted && resets == 1);
    assert(svc.waitQueueSeqno(&q, 5, 0, &done, &timedOut) == kIOReturnNotResponding);
    assert(resets == 1 && completed == 4);
    // Completion racing with lock acquisition must not reset a newer batch.
    q.faulted = false; onLock = completeOnLock;
    assert(svc.waitQueueSeqno(&q, 5, 0, &done, &timedOut, &diag) == kIOReturnSuccess);
    assert(!timedOut && done == 5 && !q.faulted && resets == 1);
    // A concurrent reset must propagate failure, without another reset.
    completed=4; onLock=faultOnLock;
    assert(svc.waitQueueSeqno(&q, 5, 0, &done, &timedOut, &diag) == kIOReturnNotResponding);
    assert(resets == 1 && completed == 4);
    assert(locks == 0 && wakes == 0);
    // All parser markers can pass while the real completion fence stays #4.
    q.faulted=false;
    fencePage[17]=5; fencePage[18]=5; fencePage[8]=5;
    assert(svc.waitQueueSeqno(&q, 5, 0, &done, &timedOut, &diag, true, nullptr, &progress) == kIOReturnSuccess);
    assert(progress == (ARCXE_PROGRESS_VALID | ARCXE_PROGRESS_BEGIN | ARCXE_PROGRESS_BEFORE_BB |
                        ARCXE_PROGRESS_RETURNED | ARCXE_PROGRESS_INVALIDATED));
    assert(timedOut && done == 4 && completed == 4 && q.faulted && resets == 2);
    assert(locks == 0 && wakes == 0);
    std::puts("PASS: production fence wait, timeout, terminal fault, and reset races");
}
'''

with tempfile.TemporaryDirectory(prefix="arcxe-wait-test-") as directory:
    path = Path(directory)
    cpp = path / "test.cpp"
    binary = path / ("test.exe" if os.name == "nt" else "test")
    cpp.write_text(STUBS + queue + SERVICE + function + CASES, encoding="utf-8")
    compiler = os.environ.get("CXX", "c++")
    if Path(compiler).stem.lower() == "cl":
        args = [compiler, "/nologo", "/std:c++17", "/EHsc", f"/I{ROOT / 'include'}", str(cpp), f"/Fe:{binary}"]
    else:
        args = [compiler, "-std=c++17", "-I", str(ROOT / "include"), str(cpp), "-o", str(binary)]
    subprocess.run(args, check=True, cwd=path)
    subprocess.run([str(binary)], check=True, timeout=10)
