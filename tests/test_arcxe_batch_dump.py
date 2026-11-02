"""Exercise actual timeout snapshot resolver, including stale and ambiguous GPU VAs."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'mesa-anv/anv_xnu_drm_shim.c').read_text(encoding='utf-8')
a = source.index('struct ArcMmapRecord {')
structs = source[a:source.index('static pthread_mutex_t', a)]
a = source.index('static void invalidate_bind_dump_locked(')
body = source[a:source.index('static void dump_timeout_batch(', a)]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using io_connect_t = unsigned;
using mach_vm_address_t = uintptr_t;
#define ARC_MAX_MMAPS 4
#define ARC_MAX_BIND_RECORDS 8
''' + structs + r'''
static ArcMmapRecord g_mmaps[ARC_MAX_MMAPS];
static ArcVmBindRecord g_binds[ARC_MAX_BIND_RECORDS];
'''
CASES = r'''
int main() {
    uint32_t data[256], words[48]; uint64_t start;
    for (unsigned i=0; i<256; ++i) data[i]=i;
    g_mmaps[0] = {true, 7, 1, 3, (uintptr_t)data, data+16, 512};
    record_vm_bind_locked(7,1,3,0x10000,512,64);
    assert(snapshot_batch_locked(7,0x10080,words,&start) == 48);
    assert(start == 0x10040 && words[0] == 32 && words[47] == 79);
    assert(snapshot_batch_locked(7,0x10000,words,&start) == 32 && words[0] == 16);
    assert(snapshot_batch_locked(7,0x101fc,words,&start) == 17 && words[16] == 143);
    assert(snapshot_batch_locked(7,0x10200,words,&start) == 0);
    assert(snapshot_batch_locked(8,0x10080,words,&start) == 0);
    assert(snapshot_batch_locked(7,0x98c,words,&start) == 0);
    record_vm_bind_locked(7,2,3,0x10000,512,64);
    assert(snapshot_batch_locked(7,0x10080,words,&start) == 0); // ambiguous VM
    g_binds[1].in_use = false;
    invalidate_bind_dump_locked(7,1,0x10040,4);
    assert(snapshot_batch_locked(7,0x10080,words,&start) == 0); // partial unmap
    record_vm_bind_locked(7,1,3,0x10000,512,64);
    g_mmaps[0].in_use = false;
    assert(snapshot_batch_locked(7,0x10080,words,&start) == 0);
    g_mmaps[0].in_use = true;
    g_binds[0].bo_offset = UINT64_MAX - 8;
    assert(snapshot_batch_locked(7,0x10080,words,&start) == 0);
    g_binds[0].bo_offset = 0;
    assert(snapshot_batch_locked(7,0x10000,words,&start) == 0); // before CPU submap
    assert(snapshot_batch_locked(7,0x10040,words,&start) == 32 && words[0] == 16);
    std::puts("PASS: production batch snapshot bounds, offsets, stale mappings and ambiguous VM rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='arcxe-dump-test-') as directory:
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
