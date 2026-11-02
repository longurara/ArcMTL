"""Test production sync wait output negotiation and shim with old/new kernel replies."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
kernel = (ROOT / 'kexts/ArcXe/ArcXeUserClient.cpp').read_text()
start = kernel.index('        auto *out      = static_cast<ArcXeSyncobjWaitOut *>')
negotiation = kernel[start:kernel.index('\n        const uint32_t count', start)]
start = kernel.index('                    if (detail)\n')
copy_detail = kernel[start:kernel.index('                    out->dbg2', start)]
shim = (ROOT / 'mesa-anv/anv_xnu_drm_shim.c').read_text()
start = shim.index('static int arc_sync_timeline_wait(', shim.index('static int arc_sync_timeline_wait(') + 1)
wait = shim[start:shim.index('\nstatic int arc_sync_wait(', start)]

HARNESS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <cstddef>
#include "arc_uapi.h"
#ifndef ETIME
#define ETIME 62
#endif
#define bzero(p,n) std::memset(p,0,n)
#define bcopy(s,d,n) std::memcpy(d,s,n)
struct Args { void *structureOutput; size_t structureOutputSize; };
static void negotiate(Args *args, bool capture) {
'''
STUBS = r'''
}
using io_connect_t = int;
enum { IO_OBJECT_NULL=0, KERN_SUCCESS=0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL=1 };
struct util_sync_provider {};
struct ArcSyncProvider { util_sync_provider base; int fd; };
struct ArcFdSession { io_connect_t connect=1; };
static int g_arc_lock=0;
static int replyError=0, dumps=0;
static size_t replySize=sizeof(ArcXeSyncobjWaitOutExtended);
static bool timeout=false, connected=true;
static void pthread_mutex_lock(int *) {}
static void pthread_mutex_unlock(int *) {}
static ArcFdSession *lookup_session_locked(int) { static ArcFdSession s; return connected ? &s : nullptr; }
static void dump_timeout_batch(int, uint64_t) { ++dumps; }
static void dump_timeout_history(int) {}
#define clock_gettime fake_clock_gettime
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
static void fake_clock_gettime(int, timespec *t) { t->tv_sec=1; t->tv_nsec=0; }
static int IOConnectCallStructMethod(io_connect_t, int method, const void *input, size_t insize,
                                     void *output, size_t *size) {
    assert(method==kArcXeMethodSyncobjWait && insize==sizeof(ArcXeSyncobjWaitIn));
    assert(*size==528);
    const auto *in=static_cast<const ArcXeSyncobjWaitIn *>(input);
    assert(in->count_handles==1 && in->handles[0]==7 && in->points[0]==9);
    assert(in->flags==ARCXE_SYNCOBJ_WAIT_ALL && in->timeout_nsec==5000000000LL);
    ArcXeSyncobjWaitOutExtended response={};
    response.base.first_signaled=3;
    response.base.timed_out=response.base.diag.timed_out=timeout;
    response.detail.valid=1;
    response.detail.fault_reg=1;
    response.detail.fault_data0=0x12345678;
    response.detail.fault_data1=0x1a;
    response.detail.idle_park_count=4;
    response.detail.ring_count=UINT32_MAX; // corrupted count must stay bounded
    response.detail.mcr.version=1;
    response.detail.mcr.enabled_dss=0xff;
    response.detail.mcr.count=UINT32_MAX; // bounded to eight even if corrupted
    response.detail.hw.version=1;
    response.detail.hw.l3_alloc=0x200;
    response.detail.rcu.version=1;
    response.detail.rcu.mode=1;
    std::memcpy(output,&response,replySize); *size=replySize;
    return replyError;
}
'''
CASES = r'''
int main() {
    static_assert(sizeof(ArcXeSyncobjWaitOut)==96);
    static_assert(offsetof(ArcXeSyncobjWaitOutExtended,detail)+offsetof(ArcXeTimeoutDetail,hw)==200);
    static_assert(sizeof(ArcXeSyncobjWaitOutExtended)==528);
    static_assert(offsetof(ArcXeSyncobjWaitOutExtended,detail)+offsetof(ArcXeTimeoutDetail,rcu)==520);
    static_assert(offsetof(ArcXeSyncobjWaitOutExtended,detail)+offsetof(ArcXeTimeoutDetail,mcr)==280);
    alignas(8) unsigned char buffer[536];
    for (size_t size : {96u, 104u, 199u, 200u, 208u, 279u, 280u, 288u, 519u, 520u, 524u, 527u, 528u, 536u}) {
        std::memset(buffer,0xa5,sizeof(buffer));
        Args args={buffer,size}; negotiate(&args, false);
        size_t expected=size>=528 ? 528 : size>=520 ? 520 : size>=280 ? 280 : size>=200 ? 200 : 96;
        assert(args.structureOutputSize==expected);
        for (size_t i=0; i<sizeof(buffer); ++i) assert(buffer[i]==(i<expected ? 0 : 0xa5));
        negotiate(&args, true);
        for (size_t i=0; i<sizeof(buffer); ++i)
            assert(buffer[i]==(i<96 ? 0 : i<expected ? 0x3c : 0xa5));
    }
    ArcSyncProvider provider={}; provider.fd=1;
    uint32_t handle=7, first=99; uint64_t point=9;
    for (size_t size : {96u, 200u, 280u, 520u, 528u}) {
        replySize=size; timeout=false;
        assert(arc_sync_timeline_wait(&provider.base,&handle,&point,1,INT64_MAX,1,&first)==0);
        assert(first==3);
        timeout=true;
        assert(arc_sync_timeline_wait(&provider.base,&handle,&point,1,INT64_MAX,1,&first)==-ETIME);
    }
    assert(dumps==5);
    replySize=95;
    assert(arc_sync_timeline_wait(&provider.base,&handle,&point,1,INT64_MAX,1,&first)==-EIO);
    replySize=200; replyError=1;
    assert(arc_sync_timeline_wait(&provider.base,&handle,&point,1,INT64_MAX,1,&first)==-EIO);
    assert(dumps==5);
    connected=false;
    assert(arc_sync_timeline_wait(&provider.base,&handle,&point,1,INT64_MAX,1,&first)==-EBADF);
    std::puts("PASS: production kernel/shim wait ABI, legacy replies, bounded diagnostics and errors");
}
'''
with tempfile.TemporaryDirectory(prefix='arcxe-wait-abi-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    binary = path / ('test.exe' if os.name == 'nt' else 'test')
    cpp.write_text('#include <initializer_list>\n' + HARNESS + negotiation +
                  '\nif (capture) { std::memset(&localDetail,0x3c,sizeof(localDetail));\n' +
                  copy_detail + '\n}\n' + STUBS + wait + CASES)
    compiler = os.environ.get('CXX', 'c++')
    if Path(compiler).stem.lower() == 'cl':
        args = [compiler, '/nologo', '/std:c++17', '/EHsc', f'/I{ROOT / "include"}', str(cpp), f'/Fe:{binary}']
    else:
        args = [compiler, '-std=c++17', '-I', str(ROOT / 'include'), str(cpp), '-o', str(binary)]
    subprocess.run(args, check=True, cwd=path)
    result = subprocess.run([str(binary)], check=True, timeout=10, capture_output=True, text=True)
    assert result.stderr.count('[ArcXe CONTEXT]') == 4
    assert result.stderr.count('[ArcXe HW STATE]') == 3
    assert result.stderr.count('[ArcXe MCR]') == 2
    assert result.stderr.count('[ArcXe DSS]') == 16
    assert result.stderr.count('[ArcXe RCU]') == 1
    assert 'MODE=00000001 CCS_ENABLE=1' in result.stderr
    assert 'L3ALLOC=00000200' in result.stderr
    assert 'address=0xa12345678000 space=GGTT' in result.stderr
    assert 'count=16' in result.stderr and '+30:' in result.stderr
    print(result.stdout.strip())
