"""ABI tests exercise the same copied-value boundary used by managed bindings."""
import ctypes as c
import struct
import sys

lib = c.CDLL(sys.argv[1])
callback_type = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_int, c.c_void_p, c.c_int, c.POINTER(c.c_void_p), c.POINTER(c.c_int))
class Options(c.Structure):
    _fields_=[("size",c.c_uint32),("abi",c.c_uint32),("memory",c.c_uint64),("payload",c.c_uint32),("refs",c.c_uint32),("calls",c.c_uint32),("handles",c.c_uint32)]
options=Options(c.sizeof(Options),2,8*1024*1024,8*1024*1024,4096,10000,65536)
lib.luau_host_create.argtypes = [c.POINTER(Options), callback_type, c.c_void_p]
lib.luau_host_create.restype = c.c_void_p
lib.luau_host_destroy.argtypes = [c.c_void_p]
lib.luau_host_bind.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
lib.luau_host_module.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p, c.c_int]
lib.luau_host_call.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p, c.c_int, c.c_char_p, c.c_int, c.c_int, c.POINTER(c.c_void_p), c.POINTER(c.c_int)]
lib.luau_host_error.argtypes = [c.c_void_p]
lib.luau_host_error.restype = c.c_char_p

callback_buffer=None
@callback_type
def host(user, operation, args, size, output, output_size):
    global callback_buffer
    response=b"managed host rejected request" if operation==1 else struct.pack("<IBd",1,3,42)
    callback_buffer=c.create_string_buffer(response)
    output[0]=c.cast(callback_buffer,c.c_void_p)
    output_size[0]=len(response)
    return 6 if operation==1 else 0

def add(vm, name, source):
    data = source.encode()
    assert lib.luau_host_module(vm, name.encode(), data, len(data)) == 0

def call(vm, name, arguments=struct.pack("<I", 0), fail=False):
    output, size = c.c_void_p(), c.c_int()
    status = lib.luau_host_call(vm, name.encode(), b"", 0, arguments, len(arguments), 100, c.byref(output), c.byref(size))
    error = lib.luau_host_error(vm)
    assert bool(status) == fail, (name, status, error)
    return c.string_at(output, size.value) if not status else error

assert lib.luau_host_abi() == 2
for iteration in range(3):
    vm = lib.luau_host_create(c.byref(options), host, None)
    assert vm
    try:
        assert lib.luau_host_bind(vm, b"answer", 0) == 0
        assert lib.luau_host_bind(vm, b"reject", 1) == 0
        add(vm, "good", "return function() return answer() end")
        add(vm, "argument", "return function(x) return x end")
        add(vm, "hosterror", "return function() reject() end")
        add(vm, "syntax", "return function broken")
        add(vm, "loop", "return function() while true do end end")
        add(vm, "catchloop", "return function() while true do pcall(function() while true do end end) end end")
        add(vm, "allocation", "return function() local t={} while true do table.insert(t,string.rep('x',10000)) end end")
        add(vm, "cycle", "return function() local t={} t.self=t return t end")
        add(vm, "dependency", "return { value=7 }")
        add(vm, "import", "local d=require('dependency'); return function() return d.value end")
        add(vm, "sandbox", "return function() math.abs=nil end")
        add(vm, "a", "return require('b')")
        add(vm, "b", "return require('a')")
        assert call(vm, "good") == struct.pack("<IBd", 1, 3, 42)
        assert call(vm, "argument", struct.pack("<IBd", 1, 3, 17)) == struct.pack("<IBd", 1, 3, 17)
        assert call(vm, "import") == struct.pack("<IBd", 1, 3, 7)
        for name in ("syntax", "hosterror", "loop", "catchloop", "allocation", "cycle", "sandbox", "a"):
            call(vm, name, fail=True)
            assert call(vm, "good") == struct.pack("<IBd", 1, 3, 42), name
    finally:
        lib.luau_host_destroy(vm)
print("Protected host ABI: values, modules, sandbox, errors, OOM, uncatchable budgets, teardown passed")

class Stats(c.Structure):
    _fields_=[("size",c.c_uint32),("refs",c.c_uint32),("used",c.c_uint64),("peak",c.c_uint64),("calls",c.c_uint64),("last_calls",c.c_uint32),("handles",c.c_uint32)]
lib.luau_host_statistics.argtypes=[c.c_void_p,c.POINTER(Stats)]
lib.luau_host_release.argtypes=[c.c_void_p,c.c_int]
lib.luau_host_error_kind.argtypes=[c.c_void_p]
def stats(vm):
    value=Stats(size=c.sizeof(Stats))
    assert lib.luau_host_statistics(vm,c.byref(value))==0
    return value
vm=lib.luau_host_create(c.byref(options),host,None)
try:
    lib.luau_host_bind(vm,b"answer",0)
    lib.luau_host_bind(vm,b"reject",1)
    add(vm,"handles","return function(a,b) assert(type(a)=='userdata' and a==b); local t={[a]=42}; assert(t[b]==42); return a end")
    handle=struct.pack("<BIII",7,1,123,456)
    assert call(vm,"handles",struct.pack("<I",2)+handle+handle)==struct.pack("<I",1)+handle
    assert stats(vm).handles==1
    add(vm,"refs","local f=function() return 42 end; return function() return f,f end")
    first=call(vm,"refs")
    assert first[4]==6 and first[9]==6
    ref=struct.unpack_from("<I",first,5)[0]
    assert ref==struct.unpack_from("<I",first,10)[0]
    for _ in range(4200):
        assert call(vm,"refs")==first
    assert stats(vm).refs==1
    assert lib.luau_host_release(vm,ref)==0
    assert stats(vm).refs==0
    assert lib.luau_host_release(vm,ref)==4
    assert call(vm,"refs")!=first
    add(vm,"failedrefs","return function() local t={}; t.self=t; return function() end,t end")
    before=stats(vm).refs
    for _ in range(20): call(vm,"failedrefs",fail=True)
    assert stats(vm).refs==before
    add(vm,"caught","return function() pcall(reject); error('ordinary script error') end")
    call(vm,"caught",fail=True)
    assert lib.luau_host_error_kind(vm)==1
    add(vm,"rethrow","return function() local ok,e=pcall(reject); error(e) end")
    call(vm,"rethrow",fail=True)
    assert lib.luau_host_error_kind(vm)==6
    add(vm,"good2","return function() return answer() end")
    call(vm,"good2")
    value=stats(vm)
    assert value.peak>=value.used>0 and value.calls>=3 and value.last_calls==1
finally:
    lib.luau_host_destroy(vm)
print("ABI 2: userdata identity, bounded references, release, rollback, structured errors and statistics passed")
