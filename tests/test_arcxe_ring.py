"""Decode the actual kernel ring emitter on a host without an Intel GPU.

Checks MTL invalidate ordering, diagnostic/fence isolation and packet boundaries
at every aligned ring tail. This does not validate hardware cache behaviour.
Requires C++17; use CXX=cl in a Windows developer command prompt.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "kexts/ArcXe/ArcXeExec.cpp").read_text(encoding="utf-8")
header = (ROOT / "kexts/ArcXe/ArcXeService.h").read_text(encoding="utf-8")
start = header.index("struct ArcExecQueue {")
queue = header[start:header.index("\n};", start) + 3]
start = source.index("static inline void ringEmitDword(")
helpers = source[start:source.index("\nbool ArcXeService::", start)]
start = source.index("    const uint64_t seqno = ++q->lastSubmittedSeqno;")
emitter = source[start:source.index("    q->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL]", start)]

STUBS = r'''
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <vector>
#include "arc_uapi.h"
#include "arc_regs_mtl.h"
#define ARC_MASKED_ENABLE(x) (((x) << 16) | (x))
struct ArcBo {};
struct IOLock {};
'''

CASES = r'''
struct Packet { uint32_t offset, header, words; };
static void check(uint16_t engine, uint32_t initialTail) {
    constexpr uint32_t size = 16384;
    constexpr uint64_t seq = 0x1234567800000005ull;
    constexpr uint64_t batch = 0x000000063fff8000ull;
    uint32_t ring[size / 4];
    std::fill(ring, ring + size / 4, 0xdeadbeef);
    ArcExecQueue q = {};
    q.engineClass=engine; q.ringSize=size; q.ringTail=initialTail; q.ringCpu=ring;
    q.seqnoGgttAddr=0x01822000; q.invalScratchGgtt=q.seqnoGgttAddr+0x40;
    q.lastSubmittedSeqno=seq-1;
    emit(&q, batch);
    assert(q.lastSubmittedSeqno == seq && q.lastBatchGpuVa == batch);
    assert(!(q.ringTail & 7));
    const uint32_t start = size-initialTail < 256 ? 0 : initialTail;
    assert(q.submitRingStart == start);
    if (start != initialTail)
        for (uint32_t i=initialTail/4; i<size/4; ++i) assert(ring[i] == ARC_MI_NOOP);
    const uint32_t length = (q.ringTail-start) & (size-1);
    assert(length > 0 && length <= 256);
    std::vector<Packet> packets;
    unsigned markers=0, fences=0, invalidates=0, debugWrites=0;
    for (uint32_t pos=start; pos<start+length;) {
        const uint32_t h=ring[pos/4];
        uint32_t n=1;
        if ((h & 0xffff0000u) == 0x7a000000u) n=(h & 0xff)+2;
        else if ((h >> 23) == 0x22) n=(h & 0xff)+2; // LRI
        else if ((h >> 23) == 0x31) n=3; // BB_START
        else if ((h >> 23) == 0x20) n=(h & 0xff)+2; // STORE_DATA_IMM
        else if ((h >> 23) == 0x26) n=(h & 0xff)+2; // FLUSH_DW
        else assert(h == ARC_MI_NOOP || h == ARC_MI_USER_INTERRUPT ||
                    h == ARC_MI_PREPARSER_DISABLE(true) || h == ARC_MI_PREPARSER_DISABLE(false) ||
                    h == (ARC_MI_ARB_ON_OFF | ARC_MI_ARB_DISABLE) ||
                    h == (ARC_MI_ARB_ON_OFF | ARC_MI_ARB_ENABLE) || h == ARC_MI_ARB_CHECK);
        assert(pos+n*4 <= size && pos+n*4 <= start+length); // no packet wraps
        packets.push_back({pos,h,n});
        if ((h >> 23) == 0x22) {
            assert(n == 3 && (h & ARC_MI_LRI_CS_MMIO));
            assert(ring[pos/4+1] == 0x0d8);
            // Bit 4 is required by ANV. Bit 6's WA is only for IP12.00..12.10.
            assert(ring[pos/4+2] == 0x00100010u);
            ++debugWrites;
        }
        if ((h >> 23) == 0x20) {
            assert(n == 5 && (h & ARC_MI_SDI_USE_GGTT));
            const uint32_t addr=ring[pos/4+1];
            const uint64_t value=ring[pos/4+3] | (uint64_t(ring[pos/4+4]) << 32);
            assert(ring[pos/4+2] == 0 && value == seq);
            if (addr == q.seqnoGgttAddr) {
                ++fences; assert(pos >= q.submitFlushEnd);
            } else {
                assert(markers < 3 && addr == q.seqnoGgttAddr+0x80+markers*8);
                if (markers == 0) assert(pos == start);
                if (markers == 1) assert(pos+20 == q.submitBatchStart);
                if (markers == 2) assert(pos == q.submitBatchEnd);
                ++markers;
            }
        }
        if (h == (ARC_GFX_OP_PIPE_CONTROL(6) | ARC_PC0_L3_RO_CACHE_INVALIDATE)) {
            ++invalidates;
            // Checked synchronous MMIO invalidation owns page-table updates.
            assert(!(ring[pos/4+1] & ARC_PC_TLB_INVALIDATE));
            assert(ring[pos/4+1] & ARC_PC_CS_STALL);
            assert(ring[pos/4+1] & ARC_PC_QW_WRITE);
            assert(ring[pos/4+1] & ARC_PC_GLOBAL_GTT);
            assert(ring[pos/4+2] == q.invalScratchGgtt);
            assert((ring[pos/4+4] | (uint64_t(ring[pos/4+5]) << 32)) == seq);
            // MTL workaround must be BEFORE preparser-disable and invalidate.
            assert(packets.size() >= 5);
            auto dummy=packets[packets.size()-4];
            assert(dummy.header == ARC_GFX_OP_PIPE_CONTROL(6));
            assert(ring[dummy.offset/4+1] == ARC_PC_DEPTH_CACHE_FLUSH);
            for (unsigned j=2; j<6; ++j) assert(ring[dummy.offset/4+j] == 0);
            assert(packets[packets.size()-3].header == ARC_MI_PREPARSER_DISABLE(true));
            assert(ring[(pos+24)/4] == ARC_MI_PREPARSER_DISABLE(false));
            assert(pos+24 < q.submitBatchStart);
        }
        if (h == ARC_MI_BATCH_BUFFER_START_PPGTT) {
            assert(pos == q.submitBatchStart && pos+12 == q.submitBatchEnd);
            assert((ring[pos/4+1] | (uint64_t(ring[pos/4+2]) << 32)) == batch);
        }
        if ((h & 0xffff0000u) == 0x7a000000u)
            assert(!(ring[pos/4+1] & ARC_PC_DEPTH_STALL)); // WA excludes MTL
        pos += n*4;
    }
    assert(markers == 3 && fences == (engine == kArcXeEngineClassCopy ? 1u : 0u));
    assert(invalidates == (engine == kArcXeEngineClassCopy ? 0u : 1u));
    assert(debugWrites == invalidates);
    assert(q.submitFlushEnd > q.submitBatchEnd);
    // Fence post-sync must follow cache flushes, never diagnostic writes.
    if (invalidates) {
        const auto off=q.submitFlushEnd/4;
        assert(ring[off] == ARC_GFX_OP_PIPE_CONTROL(6));
        assert(ring[off+2] == q.seqnoGgttAddr);
        assert((ring[off+4] | (uint64_t(ring[off+5]) << 32)) == seq);
        assert(length == 232);
    }
}
int main() {
    for (auto engine : {kArcXeEngineClassRender, kArcXeEngineClassCompute, kArcXeEngineClassCopy})
        for (uint32_t tail=0; tail<16384; tail+=8) check(engine, tail);
    std::puts("PASS: production ring packets, MTL invalidate workaround, isolated progress/fence, all wrap positions");
}
'''

with tempfile.TemporaryDirectory(prefix="arcxe-ring-test-") as directory:
    path = Path(directory)
    cpp = path / "test.cpp"
    binary = path / ("test.exe" if os.name == "nt" else "test")
    cpp.write_text(STUBS + queue + helpers +
                   "\nstatic void emit(ArcExecQueue *q, uint64_t batchGpuVa) {\n" +
                   emitter + "\n}\n" + CASES, encoding="utf-8")
    compiler = os.environ.get("CXX", "c++")
    if Path(compiler).stem.lower() == "cl":
        args = [compiler, "/nologo", "/std:c++17", "/EHsc", f"/I{ROOT / 'include'}", str(cpp), f"/Fe:{binary}"]
    else:
        args = [compiler, "-std=c++17", "-I", str(ROOT / "include"), str(cpp), "-o", str(binary)]
    subprocess.run(args, check=True, cwd=path)
    subprocess.run([str(binary)], check=True, timeout=30)
