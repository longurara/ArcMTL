"""Exercise production Mesa -> GEM cache policy -> CPU mapping -> bind PAT selection.

These checks catch cache-mode rewriting without requiring macOS. The live
arc_probe tests check the resulting CPU/GPU visibility on the real machine.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
kernel = (ROOT / 'kexts/ArcXe/ArcXeUserClient.cpp').read_text()
shim = (ROOT / 'mesa-anv/anv_xnu_drm_shim.c').read_text()
start = shim.index('        in.size = gc->size;')
shim_policy = shim[start:shim.index('        size_t out_sz', start)]
start = kernel.index('        uint32_t flags = in->flags;')
create_policy = kernel[start:kernel.index('        ArcBo *bo =', start)]
start = kernel.index('        /* Only a real CPU WB mapping')
bind_policy = kernel[start:kernel.index('        const bool isUnmap', start)]
start = kernel.index('    *options = (bo->flags & ARCXE_GEM_FLAG_CPU_WC)')
map_policy = kernel[start:kernel.index('\n', start)]

harness = r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include "arc_uapi.h"
#include "arc_regs_mtl.h"
enum { DRM_XE_GEM_CPU_CACHING_WB=1, DRM_XE_GEM_CPU_CACHING_WC=2,
       DRM_XE_GEM_CREATE_FLAG_SCANOUT=1, kIOReturnBadArgument=-2,
       kIOMapDefaultCache=0, kIOMapWriteCombineCache=0x400 };
struct drm_xe_gem_create { uint64_t size=16384; uint32_t flags=0; uint16_t cpu_caching=0; };
struct ArcBo { uint32_t flags; uint16_t patIndex; };
static int shim_create(drm_xe_gem_create *gc, ArcXeGemCreateIn &in) {
SHIM
    return 0;
}
static int kernel_create(const ArcXeGemCreateIn *in, ArcBo *bo) {
CREATE
    bo->flags=flags; bo->patIndex=pat; return 0;
}
static uint16_t bind(ArcBo *bo, uint16_t pat) {
BIND
    return pat;
}
static uint32_t mapping(ArcBo *bo) {
    uint32_t result=0, *options=&result;
MAP
    return result;
}
int main() {
    for (unsigned scanout : {0u, 1u}) {
        for (uint16_t mode : {1, 2}) {
            drm_xe_gem_create gc; gc.flags=scanout; gc.cpu_caching=mode;
            ArcXeGemCreateIn in={}; ArcBo bo={};
            assert(shim_create(&gc,in)==0 && in.size==16384);
            assert(kernel_create(&in,&bo)==0);
            assert(bool(bo.flags & ARCXE_GEM_FLAG_SCANOUT)==bool(scanout));
            assert(mapping(&bo)==(mode==2 ? kIOMapWriteCombineCache : kIOMapDefaultCache));
            assert(bo.patIndex==(mode==2 ? 1 : 3));
            for (uint16_t pat=0; pat<5; ++pat)
                assert(bind(&bo,pat)==(mode==1 && pat<3 ? 4 : pat));
        }
    }
    // NULL has no CPU backing: retain PAT0 (shared 2 MiB NULL PT) and alternate PATs.
    for (uint16_t pat=0; pat<5; ++pat) assert(bind(nullptr,pat)==pat);
    // Preserve legacy WB defaults and explicit two-way coherent allocations.
    ArcXeGemCreateIn in={}; ArcBo bo={}; in.size=16384;
    assert(kernel_create(&in,&bo)==0 && bo.flags==ARCXE_GEM_FLAG_CPU_WB_COHERENT && bo.patIndex==4);
    in.pat_index=4; assert(kernel_create(&in,&bo)==0 && bo.patIndex==4);
    in.flags=ARCXE_GEM_FLAG_CPU_WB_COHERENT|ARCXE_GEM_FLAG_CPU_WC;
    assert(kernel_create(&in,&bo)==kIOReturnBadArgument);
    in.flags=ARCXE_GEM_FLAG_CPU_WC; in.pat_index=5;
    assert(kernel_create(&in,&bo)==kIOReturnBadArgument);
    for (uint16_t mode : {0, 3, 65535}) {
        drm_xe_gem_create gc; gc.cpu_caching=mode; errno=0;
        assert(shim_create(&gc,in)==-1 && errno==EINVAL);
    }
    std::puts("PASS: production shim/GEM/CPU-map/bind cache policy, WB3/4, WC1 and NULL PAT0");
}
'''.replace('SHIM', shim_policy).replace('CREATE\n', create_policy+'\n').replace('BIND', bind_policy).replace('MAP\n', map_policy+'\n')
# initializer_list is required by MSVC for braced range-for lists.
harness = '#include <initializer_list>\n' + harness
with tempfile.TemporaryDirectory(prefix='arcxe-cache-test-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    binary = path / ('test.exe' if os.name == 'nt' else 'test')
    cpp.write_text(harness, encoding='utf-8')
    compiler = os.environ.get('CXX', 'c++')
    if Path(compiler).stem.lower() == 'cl':
        args = [compiler, '/nologo', '/std:c++17', '/EHsc', f'/I{ROOT / "include"}', str(cpp), f'/Fe:{binary}']
    else:
        args = [compiler, '-std=c++17', '-I', str(ROOT / 'include'), str(cpp), '-o', str(binary)]
    subprocess.run(args, check=True, cwd=path)
    subprocess.run([str(binary)], check=True, timeout=10)
