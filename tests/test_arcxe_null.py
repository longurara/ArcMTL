"""Run production PPGTT bind/unbind and probe emitters against a NULL-PTE memory model.

Checks VM semantics on the host; arc_probe verifies actual NULL writes/reads on RCS.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'kexts/ArcXe/ArcXeMemory.cpp').read_text(encoding='utf-8')
start = source.index('    fScratchLeafPte = arcEncodePpgttPte(')
leaves = source[start:source.index('\n\n', start)]
start = source.index('bool ArcXeService::vmBindBo(')
bind = source[start:].replace('__asm__ __volatile__("mfence" ::: "memory");', 'host_barrier();')
probe = (ROOT / 'tools/arc_probe.c').read_text(encoding='utf-8')
start = probe.index('    const uint64_t batch1_va =')
emitter = probe[start:probe.index('    struct ArcXeExecQueueCreateIn', start)]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>
#include "arc_regs_mtl.h"
using IOByteCount=uint64_t;
using addr64_t=uint64_t;
enum { kIOMemoryMapperNone };
struct IOLock {};
static int locks=0, barriers=0;
static void IOLockLock(IOLock *) { ++locks; }
static void IOLockUnlock(IOLock *) { --locks; }
static void arcClflushEntry(const volatile void *) {}
static void host_barrier() { ++barriers; }
#define __sync_synchronize host_barrier
struct Memory {
    uint64_t base=0x800000, size=0x4000;
    uint64_t getPhysicalSegment(uint64_t off, IOByteCount *len, int) {
        if (len) *len=size-off;
        return base+off;
    }
};
struct ArcBo { Memory *memDesc; uint64_t size; bool readOnly=false; };
struct ArcPtPage {
    uint64_t physAddr, entries[512]={};
    ArcPtPage *children[512]={};
    bool isNullBacked=false;
};
struct ArcVm { ArcPtPage *pml4; IOLock *lock=nullptr; };
struct ArcXeService {
    ArcBo *fScratchPageBo=nullptr;
    ArcPtPage *fScratchPt=nullptr, *fScratchPd=nullptr, *fScratchPdp=nullptr, *fNullPt=nullptr;
    uint64_t fScratchLeafPte=0, fNullLeafPte=0;
    std::map<uint64_t,ArcPtPage *> pages;
    std::map<uint64_t,uint32_t> ram;
    bool tlbOk=true;
    unsigned invalidates=0;
    ArcPtPage *allocPtPage(uint64_t fill) {
        auto *p=new ArcPtPage; p->physAddr=0x100000+pages.size()*4096;
        for (auto &e:p->entries) e=fill;
        pages[p->physAddr]=p; return p;
    }
    void initLeaves() {
'''
SERVICE = r'''
    }
    ArcXeService() {
        initLeaves();
        fScratchPt=allocPtPage(fScratchLeafPte);
        fScratchPd=allocPtPage(arcEncodePpgttPde(fScratchPt->physAddr));
        fScratchPdp=allocPtPage(arcEncodePpgttPde(fScratchPd->physAddr));
        fNullPt=allocPtPage(fNullLeafPte);
    }
    ~ArcXeService() { for (auto &p:pages) delete p.second; }
    bool invalidateGlobalTlb() { assert(!locks); ++invalidates; return tlbOk; }
    bool vmBindBo(ArcVm *, uint64_t, ArcBo *, uint64_t, uint64_t, uint16_t, bool, bool);
    uint64_t &pte(ArcVm &vm,uint64_t va) {
        auto *p=vm.pml4;
        for (int shift=39; shift>12; shift-=9) {
            uint64_t entry=p->entries[(va>>shift)&511]; assert(entry&ARC_PTE_PRESENT);
            p=pages.at(entry&ARC_PTE_ADDR_MASK);
        }
        return p->entries[(va>>12)&511];
    }
    void write(ArcVm &vm,uint64_t va,uint32_t data) {
        auto entry=pte(vm,va); assert(entry&ARC_PTE_PRESENT);
        if (!(entry&ARC_PPGTT_PTE_NULL)) {
            assert(entry&ARC_PTE_RW); ram[(entry&ARC_PTE_ADDR_MASK)+(va&4095)]=data;
        }
    }
    uint32_t read(ArcVm &vm,uint64_t va) {
        auto entry=pte(vm,va); assert(entry&ARC_PTE_PRESENT);
        return (entry&ARC_PPGTT_PTE_NULL) ? 0 : ram[(entry&ARC_PTE_ADDR_MASK)+(va&4095)];
    }
};
static void emit_probe(uint8_t *cpu_ptr, uint64_t base_va, uint32_t batch_off,
                       uint32_t target_off, uint64_t magic, uint64_t null_va) {
'''
CASES = r'''
}
// Interpret only the MI packets used by the real smoke-test emitter.
static void run_probe_batch(ArcXeService &s, ArcVm &vm, const uint32_t *cs) {
    for (unsigned i=0; i<64;) {
        const uint32_t h=cs[i];
        if (h==ARC_MI_BATCH_BUFFER_END) return;
        if (h==ARC_MI_NOOP) { ++i; continue; }
        uint64_t dest=cs[i+1]|(uint64_t(cs[i+2])<<32);
        if ((h>>23)==0x20) {
            assert(h==(ARC_MI_STORE_DATA_IMM|ARC_MI_SDI_QWORD_LEN));
            s.write(vm,dest,cs[i+3]); s.write(vm,dest+4,cs[i+4]);
        } else {
            assert(h==0x17000003); // independent genxml MI_COPY_MEM_MEM encoding
            uint64_t src=cs[i+3]|(uint64_t(cs[i+4])<<32);
            s.write(vm,dest,s.read(vm,src));
        }
        i+=5;
    }
    assert(false); // missing BB_END
}
int main() {
    ArcXeService s;
    ArcVm vm={s.allocPtPage(arcEncodePpgttPde(s.fScratchPdp->physAddr))};
    Memory memory; ArcBo bo={&memory,0x4000};
    const uint64_t base=0x100000000ull, scratch=0x200000000ull, null=0x300000000ull;
    assert(s.fScratchLeafPte==0x21b && s.fNullLeafPte==0x203);
    assert(s.vmBindBo(&vm,base,&bo,0,0x4000,4,false,false));
    assert(s.vmBindBo(&vm,null,nullptr,0,0x200000,0,false,true));
    assert(s.vmBindBo(&vm,null+0x400000,nullptr,0,4096,0,false,true));
    // Production probe must reject the old writable-zero-page implementation.
    alignas(8) uint8_t cpu[16384]={};
    for (uint64_t va : {scratch,null,null+0x400000}) {
        assert(s.pte(vm,va)&ARC_PPGTT_PTE_NULL);
        emit_probe(cpu,base,0x2000,0x3000,0xabcdef1234567890ull,va);
        run_probe_batch(s,vm,(uint32_t *)(cpu+0x2000));
        assert(s.read(vm,base+0x3000)==(0x34567890u^0x11111111u));
        run_probe_batch(s,vm,(uint32_t *)(cpu+0x2100));
        assert(s.read(vm,base+0x3000)==0 && s.read(vm,base+0x3004)==0);
    }
    // Splitting the shared NULL PT must preserve adjacent pages and other slots.
    assert(s.vmBindBo(&vm,null+4096,&bo,4096,4096,3,false,false));
    assert(!(s.pte(vm,null+4096)&ARC_PPGTT_PTE_NULL));
    s.write(vm,null+4096,0x12345678); assert(s.read(vm,null+4096)==0x12345678);
    assert(s.read(vm,null)==0 && s.read(vm,null+8192)==0);
    assert(s.vmBindBo(&vm,null+4096,nullptr,0,4096,0,true,false));
    assert(s.pte(vm,null+4096)==s.fNullLeafPte);
    // Alternate PAT cannot take the shared default-PAT fast path.
    assert(s.vmBindBo(&vm,null+0x800000,nullptr,0,0x200000,1,false,true));
    assert(s.pte(vm,null+0x800000)==0x20b && s.pte(vm,null+0x9ff000)==0x20b);
    // Ordinary unmap restores native scratch semantics.
    assert(s.vmBindBo(&vm,base,nullptr,0,4096,0,true,false));
    assert(s.pte(vm,base)==s.fScratchLeafPte);
    s.write(vm,base,0xffffffff); assert(!s.read(vm,base));
    s.tlbOk=false;
    assert(!s.vmBindBo(&vm,null,nullptr,0,4096,0,false,true));
    assert(!locks && barriers>=s.invalidates);
    // Model the original bug: a shared RAM leaf keeps poison after a completed write.
    s.pte(vm,null)=arcEncodePpgttPte(0x900000,0,true,false);
    emit_probe(cpu,base,0x2000,0x3000,0xabcdef1234567890ull,null);
    run_probe_batch(s,vm,(uint32_t *)(cpu+0x2000));
    run_probe_batch(s,vm,(uint32_t *)(cpu+0x2100));
    assert(s.read(vm,base+0x3000)==0xbaadf00d && s.read(vm,base+0x3004)==0xdeadbeef);
    std::puts("PASS: production NULL PTE init/bind/unmap/split/PAT and hardware probe packets");
}
'''
with tempfile.TemporaryDirectory(prefix='arcxe-null-test-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    binary = path / ('test.exe' if os.name == 'nt' else 'test')
    cpp.write_text(HARNESS + leaves + SERVICE + emitter + CASES + bind, encoding='utf-8')
    compiler = os.environ.get('CXX', 'c++')
    if Path(compiler).stem.lower() == 'cl':
        args = [compiler, '/nologo', '/std:c++17', '/EHsc', f'/I{ROOT / "include"}', str(cpp), f'/Fe:{binary}']
    else:
        args = [compiler, '-std=c++17', '-I', str(ROOT / 'include'), str(cpp), '-o', str(binary)]
    subprocess.run(args, check=True, cwd=path)
    subprocess.run([str(binary)], check=True, timeout=10)
