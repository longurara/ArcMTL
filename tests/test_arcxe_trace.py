"""Run the real userspace flight recorder: wrap, fd isolation, errors and bounded copies."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'mesa-anv/anv_xnu_drm_shim.c').read_text()
start = source.index('#define ARC_TRACE_EVENTS')
end = source.index('#define IOConnectCallStructMethod traced_struct_call', start)
recorder = source[start:end]
start = source.index('static void dump_trace_syncs(')
dump = source[start:source.index('static void dump_timeout_batch(', start)]

HARNESS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "arc_uapi.h"
using io_connect_t = unsigned;
using io_service_t = unsigned;
using mach_vm_address_t = uintptr_t;
using kern_return_t = int;
using pthread_mutex_t = int;
#define PTHREAD_MUTEX_INITIALIZER 0
#define ARC_MAX_FDS 4
#define ARC_MAX_SYNCOBJ_TRACK 8
#define ARC_MAX_MMAPS 4
#define ARC_MAX_BIND_RECORDS 8
#define KERN_SUCCESS 0
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#define clock_gettime fake_clock
static void fake_clock(int, timespec *t) { static int n=0; t->tv_sec=1; t->tv_nsec=++n; }
static int locks=0, error=0;
static void pthread_mutex_lock(int *) { assert(!locks); ++locks; }
static void pthread_mutex_unlock(int *) { assert(locks==1); --locks; }
static int IOConnectCallStructMethod(io_connect_t, uint32_t, const void *, size_t,
                                      void *output, size_t *size) {
    assert(!locks); // never hold the recorder lock across a blocking kernel call
    if (output && size) { std::memset(output,0x12,*size); *size=99999; }
    return error;
}
'''

CASES = r'''
int main() {
    auto &s=g_sessions[0]; s.in_use=true; s.fd=7; s.connect=70;
    auto &other=g_sessions[1]; other.in_use=true; other.fd=8; other.connect=80;
    ArcXeExecIn exec={}; exec.exec_queue_id=42; exec.batch_gpu_va=0x12340000;
    exec.num_batch_buffer=1; exec.num_syncs=1; exec.syncs[0]={9,1,19};
    uint64_t output=0; size_t size=sizeof(output);
    assert(traced_struct_call(70,kArcXeMethodExec,&exec,sizeof(exec),&output,&size)==0);
    assert(s.trace_next==2 && other.trace_next==0 && !locks);
    assert(s.trace[0].bytes==sizeof(exec) && !s.trace[0].finished && s.trace[0].call==1);
    assert(s.trace[1].bytes==8 && s.trace[1].finished && s.trace[1].call==1);
    assert(output==0x1212121212121212ull); // copied only caller capacity despite invalid returned size
    error=123; size=sizeof(output);
    traced_struct_call(70,kArcXeMethodExec,&exec,sizeof(exec),&output,&size);
    assert(s.trace[3].status==123 && !s.trace[3].bytes && s.trace[3].call==3);
    assert(record_call(70,kArcXeMethodSyncobjWait,0,false,0,nullptr,0)==0 && s.trace_next==4);
    assert(record_call(99,kArcXeMethodExec,0,false,0,nullptr,0)==0);
    uint8_t big[512]; std::memset(big,0xab,sizeof(big));
    record_call(80,kArcXeMethodGemCreate,0,false,0,big,sizeof(big));
    assert(other.trace_next==1 && other.trace[0].bytes==sizeof(ArcXeVmBindIn));
    for (unsigned i=0; i<130; ++i) record_call(70,kArcXeMethodExec,0,false,0,&exec,sizeof(exec));
    assert(s.trace_next==134);
    for (uint64_t i=7; i<=134; ++i) assert(s.trace[(i-1)%ARC_TRACE_EVENTS].index==i);
    ArcXeVmBindIn bind={}; bind.vm_id=3; bind.gem_handle=4; bind.gpu_va=0x98760000;
    bind.range_size=0x1000; bind.pat_index=3; bind.num_syncs=UINT32_MAX;
    record_call(70,kArcXeMethodVmBind,0,false,0,&bind,sizeof(bind));
    g_binds[0]={true,true,7,3,4,0x98760000,0x1000,0};
    g_binds[1]={true,false,7,3,5,0x88880000,0x2000,0};
    g_binds[2]={true,true,8,99,99,0xbadbad,0x1000,0};
    g_mmaps[0]={true,7,70,4,0x10000,(void *)0x10000,0x1000};
    dump_timeout_history(7);
    dump_timeout_history(7); // first failure only, avoid flooding
    dump_timeout_history(99);
    assert(s.diagnostics_dumped && !other.diagnostics_dumped && !locks);
    std::memset(&s,0,sizeof(s)); // close/reopen does not inherit the old fd's history
    assert(!s.trace_next && !s.diagnostics_dumped);
    std::puts("PASS: production flight recorder wrap, isolation, bounds, failure records and first-error snapshot");
}
'''
with tempfile.TemporaryDirectory(prefix='arcxe-trace-test-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    binary = path / ('test.exe' if os.name == 'nt' else 'test')
    cpp.write_text(HARNESS + recorder + dump + CASES)
    compiler = os.environ.get('CXX', 'c++')
    if Path(compiler).stem.lower() == 'cl':
        args = [compiler, '/nologo', '/std:c++17', '/EHsc', f'/I{ROOT / "include"}', str(cpp), f'/Fe:{binary}']
    else:
        args = [compiler, '-std=c++17', '-I', str(ROOT / 'include'), str(cpp), '-o', str(binary)]
    subprocess.run(args, check=True, cwd=path)
    result = subprocess.run([str(binary)], check=True, timeout=10, capture_output=True, text=True)
    assert result.stderr.count('[ArcXe HISTORY]') == 1
    assert '#8 call=8' in result.stderr and '#7 call=7' not in result.stderr
    assert '#135 call=135' in result.stderr and 'queue=42 batch=0x000012340000' in result.stderr
    assert 'vm=3 op=0 gem=4' in result.stderr and 'pat=3' in result.stderr
    assert 'tracked BO ranges=2' in result.stderr and 'gem=5' in result.stderr and 'valid=0' in result.stderr
    assert 'gem=99' not in result.stderr and '[ArcXe CPU MAPS] count=1' in result.stderr
    print(result.stdout.strip())
