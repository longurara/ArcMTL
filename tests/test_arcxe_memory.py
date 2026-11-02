"""Run production BO allocation/zeroing with an IOKit memory-entry model.

The descriptor's backing cache mode is chosen at allocation time, as in XNU
IOBufferMemoryDescriptor::initWithPhysicalMask. Mapping a WC BO whose original
backing was WB is rejected by this model. Real visibility is tested by arc_probe.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'kexts/ArcXe/ArcXeMemory.cpp').read_text()
start = source.index('ArcBo *ArcXeService::createKernelBo(')
create = source[start:source.index('\nArcBo *ArcXeService::importTaskMemoryBo(', start)]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "arc_uapi.h"
#include "arc_regs_mtl.h"
using IOOptionBits=uint32_t;
using mach_vm_size_t=uint64_t;
enum { kIODirectionInOut=3, kIOMemoryPhysicallyContiguous=0x10,
       kIOMemoryPageable=0x20, kIOMemoryKernelUserShared=0x10000,
       kIOMapDefaultCache=0, kIOMapWriteCombineCache=0x400, kIOMapCacheMask=0xf00,
       kIOReturnSuccess=0, kernel_task=1 };
static int failAt=0, descriptors=0, maps=0, bos=0, flushes=0, ggtts=0;
static uint32_t lastAlloc=0, lastMap=0;
#define AXELOG(...) ((void)0)
#define bzero(p,n) std::memset(p,0,n)
struct IOMemoryMap {
    void *address;
    IOMemoryMap(void *p):address(p) { ++maps; }
    uintptr_t getVirtualAddress() { return (uintptr_t)address; }
    void release() { --maps; delete this; }
};
struct IOBufferMemoryDescriptor {
    uint32_t options;
    std::vector<uint8_t> bytes;
    bool prepared=false;
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(int task, uint32_t options,
                                                           uint64_t size, uint64_t mask) {
        assert(task==kernel_task && mask==ARC_DMA_PHYS_MASK_46BIT);
        assert(!(options & kIOMemoryPageable));
        lastAlloc=options;
        if (failAt==1) return nullptr;
        auto *d=new IOBufferMemoryDescriptor;
        d->options=options; d->bytes.resize(size,0xa5); ++descriptors; return d;
    }
    int prepare() { if (failAt==2) return -1; prepared=true; return 0; }
    void complete() { assert(prepared); prepared=false; }
    void release() { assert(!prepared); --descriptors; delete this; }
    IOMemoryMap *map(uint32_t mode) {
        lastMap=mode;
        // Initialization must use the same cache mode as the original backing.
        assert((options & kIOMapCacheMask)==(mode & kIOMapCacheMask));
        if (failAt==4) return nullptr;
        return new IOMemoryMap(failAt==5 ? nullptr : bytes.data());
    }
};
struct ArcBo {
    unsigned refCount=0;
    IOBufferMemoryDescriptor *ownedBuf=nullptr, *memDesc=nullptr;
    bool prepared=false;
    uint64_t size=0,ggttOffset=0,ggttSize=0;
    uint32_t flags=0;
    uint16_t patIndex=0;
    IOMemoryMap *kernMap=nullptr;
};
static ArcBo *allocateBo() { if (failAt==3) return nullptr; ++bos; return new ArcBo; }
#define IONewZero(T,n) allocateBo()
static void arcClflushRange(const void *, size_t) { ++flushes; }
struct ArcXeService {
    ArcBo *createKernelBo(uint64_t,uint32_t,uint16_t,bool);
    void releaseBo(ArcBo *bo) {
        if (bo->kernMap) bo->kernMap->release();
        if (bo->prepared) bo->ownedBuf->complete();
        bo->ownedBuf->release(); --bos; delete bo;
    }
    bool allocGgttAndBind(IOBufferMemoryDescriptor *d, uint64_t size, uint16_t pat,
                          uint64_t *offset, uint64_t *length) {
        assert(pat<=3 && size==d->bytes.size());
        for (auto byte:d->bytes) assert(byte==0);
        ++ggtts; if (failAt==6) return false;
        *offset=0x100000; *length=size; return true;
    }
};
'''
CASES = r'''
int main() {
    ArcXeService s;
    assert(!s.createKernelBo(0,0,3,false));
    for (bool wc : {false,true}) for (bool kernel : {false,true})
    for (bool scanout : {false,true}) for (uint64_t size : {4096u,16384u,16385u}) {
        uint32_t flags=(wc ? ARCXE_GEM_FLAG_CPU_WC : ARCXE_GEM_FLAG_CPU_WB_COHERENT)
                      | (scanout ? ARCXE_GEM_FLAG_SCANOUT : 0);
        uint16_t pat=wc ? 1 : 3;
        for (failAt=0; failAt<=6; ++failAt) {
            flushes=ggtts=0;
            auto *bo=s.createKernelBo(size,flags,pat,kernel);
            bool expected=failAt==0 || (failAt==6 && !scanout);
            assert(bool(bo)==expected);
            assert((lastAlloc & kIOMapCacheMask)==(wc ? kIOMapWriteCombineCache : 0));
            assert(bool(lastAlloc & kIOMemoryKernelUserShared)==!kernel);
            if (bo) {
                assert(bo->size==((size+4095)&~4095ull) && bo->patIndex==pat);
                assert(bo->flags==flags && bool(bo->kernMap)==kernel);
                assert(descriptors==1 && bos==1 && maps==int(kernel));
                for (auto byte:bo->ownedBuf->bytes) assert(!byte);
                assert(flushes==int(wc) && ggtts==int(scanout));
                s.releaseBo(bo);
            }
            assert(!descriptors && !maps && !bos);
        }
    }
    std::puts("PASS: production BO backing cache mode, consistent zeroing maps and allocation failure cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='arcxe-memory-test-') as directory:
    path=Path(directory)
    cpp=path/'test.cpp'
    binary=path/('test.exe' if os.name=='nt' else 'test')
    cpp.write_text(HARNESS+create+CASES,encoding='utf-8')
    compiler=os.environ.get('CXX','c++')
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++17','/EHsc',f'/I{ROOT / "include"}',str(cpp),f'/Fe:{binary}']
    else:
        args=[compiler,'-std=c++17','-I',str(ROOT/'include'),str(cpp),'-o',str(binary)]
    subprocess.run(args,check=True,cwd=path)
    subprocess.run([str(binary)],check=True,timeout=10)
