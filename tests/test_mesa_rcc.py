"""Apply the real ANV RCC patch twice and execute Darwin/Linux option paths."""
import ast
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
patcher = ROOT / 'mesa-anv/apply_mesa_anv_macos.py'
spec = importlib.util.spec_from_file_location('arc_mesa_patch', patcher)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
marker = 'ARCXE_FIXED_BTP_BTI_RCC'
for call in ast.walk(ast.parse(patcher.read_text(encoding='utf-8'))):
    if (isinstance(call, ast.Call) and len(call.args) == 4 and
        isinstance(call.args[3], ast.Constant) and call.args[3].value == marker):
        old, new = (ast.literal_eval(arg) for arg in call.args[1:3])
        break
else:
    raise AssertionError('Missing RCC backend policy patch')
with tempfile.TemporaryDirectory(prefix='arc-rcc-patch-') as directory:
    fixture = Path(directory) / 'anv_physical_device.c'
    fixture.write_text(old, encoding='utf-8')
    module.replace_once(fixture, old, new, marker)
    once = fixture.read_bytes()
    module.replace_once(fixture, old, new, marker)
    assert fixture.read_bytes() == once
    body = fixture.read_text(encoding='utf-8')
HARNESS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
struct Device { bool rt_change_needs_flush; };
struct Instance { struct { struct { bool state_cache_perf_fix; } perf; } drirc; };
static std::map<std::string,std::string> env;
static const char *fake_getenv(const char *key) {
    auto it=env.find(key); return it == env.end() ? nullptr : it->second.c_str();
}
static int fake_setenv(const char *key, const char *value, int overwrite) {
    if (overwrite || !env.count(key)) env[key]=value;
    return 0;
}
'''
shim = (ROOT / 'mesa-anv/anv_xnu_drm_shim.c').read_text(encoding='utf-8')
begin = shim.index('static void arc_xnu_shim_init_env(void)')
constructor = shim[begin:shim.index('\n/* ===', begin)]
HARNESS += ('\n#define getenv fake_getenv\n#define setenv fake_setenv\n' + constructor +
            '\n#undef getenv\n#undef setenv\n')
prefix = r'''(bool enabled, bool platform_supports_btp_bit_rcc) {
    Device dev{}; Device *device=&dev;
    Instance inst{}; Instance *instance=&inst;
    instance->drirc.perf.state_cache_perf_fix = enabled;
'''
suffix = '\nreturn device->rt_change_needs_flush;\n}\n'
body = ('#undef __APPLE__\nbool linux_policy' + prefix + body + suffix +
        '#define __APPLE__ 1\nbool darwin_policy' + prefix + body + suffix)
CASES = r'''
int main() {
    arc_xnu_shim_init_env();
    assert(env["anv_state_cache_perf_fix"] == "true");
    assert(!darwin_policy(env["anv_state_cache_perf_fix"] == "true", false));
    assert(env["intel_tbimr"] == "false");
    for (const char *value : {"false", "true"}) {
        env["anv_state_cache_perf_fix"] = value;
        arc_xnu_shim_init_env();
        assert(env["anv_state_cache_perf_fix"] == value); // preserve user override
    }
    for (int opt=0; opt<2; ++opt) for (int capability=0; capability<2; ++capability) {
        assert(linux_policy(opt,capability) == (!opt || !capability));
        assert(darwin_policy(opt,capability) == !opt);
    }
    std::puts("PASS: real ANV RCC patch idempotence, Darwin fixed BTP+BTI policy, opt-out and Linux policy");
}
'''
with tempfile.TemporaryDirectory(prefix='arc-rcc-test-') as directory:
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
