"""ABI tests exercise the same copied-value boundary used by managed bindings."""
import ctypes as c
import struct
import sys

lib = c.CDLL(sys.argv[1])
callback_type = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_int, c.c_void_p, c.c_int, c.c_void_p, c.c_int)
lib.luau_host_create.argtypes = [c.c_uint64, callback_type, c.c_void_p]
lib.luau_host_create.restype = c.c_void_p
lib.luau_host_destroy.argtypes = [c.c_void_p]
lib.luau_host_bind.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
lib.luau_host_module.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p, c.c_int]
lib.luau_host_call.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p, c.c_int, c.c_char_p, c.c_int, c.c_int, c.POINTER(c.c_void_p), c.POINTER(c.c_int)]
lib.luau_host_error.argtypes = [c.c_void_p]
lib.luau_host_error.restype = c.c_char_p

@callback_type
def host(user, operation, args, size, output, capacity):
    if operation == 1:
        error = b"managed host rejected request"
        c.memmove(output, error, len(error))
        return -len(error)
    response = struct.pack("<IBd", 1, 3, 42)
    c.memmove(output, response, len(response))
    return len(response)

def add(vm, name, source):
    data = source.encode()
    assert lib.luau_host_module(vm, name.encode(), data, len(data)) == 0

def call(vm, name, arguments=struct.pack("<I", 0), fail=False):
    output, size = c.c_void_p(), c.c_int()
    status = lib.luau_host_call(vm, name.encode(), b"", 0, arguments, len(arguments), 100, c.byref(output), c.byref(size))
    error = lib.luau_host_error(vm)
    assert bool(status) == fail, (name, status, error)
    return c.string_at(output, size.value) if not status else error

assert lib.luau_host_abi() == 1
for iteration in range(3):
    vm = lib.luau_host_create(8 * 1024 * 1024, host, None)
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
