"""Exercise the exported ABI through a dynamic loader, like a future binding."""
import ctypes as c
import pathlib
import sys


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    lib = c.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))

    def bind(name, result, *args):
        function = getattr(lib, name)
        function.restype = result
        function.argtypes = args
        return function

    newstate = bind("luaL_newstate", c.c_void_p)
    close = bind("lua_close", None, c.c_void_p)
    openlibs = bind("luaL_openlibs", None, c.c_void_p)
    compile_source = bind("luau_compile", c.c_void_p, c.c_char_p, c.c_size_t, c.c_void_p, c.POINTER(c.c_size_t))
    free = bind("luau_native_free", None, c.c_void_p)
    load = bind("luau_load", c.c_int, c.c_void_p, c.c_char_p, c.c_void_p, c.c_size_t, c.c_int)
    pcall = bind("lua_pcall", c.c_int, c.c_void_p, c.c_int, c.c_int, c.c_int)
    number = bind("lua_tonumberx", c.c_double, c.c_void_p, c.c_int, c.POINTER(c.c_int))
    string = bind("lua_tolstring", c.c_char_p, c.c_void_p, c.c_int, c.POINTER(c.c_size_t))
    settop = bind("lua_settop", None, c.c_void_p, c.c_int)
    pushnumber = bind("lua_pushnumber", None, c.c_void_p, c.c_double)
    callback_type = c.CFUNCTYPE(c.c_int, c.c_void_p)
    pushclosure = bind("lua_pushcclosurek", None, c.c_void_p, callback_type, c.c_char_p, c.c_int, c.c_void_p)
    setfield = bind("lua_setfield", None, c.c_void_p, c.c_int, c.c_char_p)

    @callback_type
    def host_add(state):
        pushnumber(state, number(state, 1, None) + number(state, 2, None))
        return 1

    # Multiple independent states also exercise ownership and teardown.
    for _ in range(10):
        state = newstate()
        check(state, "luaL_newstate failed")
        try:
            openlibs(state)
            pushclosure(state, host_add, b"host_add", 0, None)
            setfield(state, -10002, b"host_add")  # LUA_GLOBALSINDEX from lua.h

            def load_source(source):
                size = c.c_size_t()
                bytecode = compile_source(source, len(source), None, c.byref(size))
                check(bytecode, "luau_compile failed to allocate bytecode")
                try:
                    return load(state, b"=native-smoke", bytecode, size.value, 0)
                finally:
                    free(bytecode)

            check(load_source(b"local x: number = host_add(12, 30); return x") == 0, "load failed")
            check(pcall(state, 0, 1, 0) == 0, "execution failed")
            check(number(state, -1, None) == 42, "callback returned the wrong result")
            settop(state, 0)
            check(load_source(b"return (") != 0, "syntax error was accepted")
            check(string(state, -1, None), "missing syntax error message")
            settop(state, 0)
            check(load_source(b"error('expected failure')") == 0, "error script did not load")
            check(pcall(state, 0, 1, 0) != 0, "runtime error escaped protected execution")
            check(b"expected failure" in string(state, -1, None), "missing runtime error message")
        finally:
            close(state)
    check(not hasattr(lib, "luau_codegen_create"), "unexpected native code generator")
    print("PASS: dynamic load, compile, interpreter, host callback, errors, allocation and teardown")


if __name__ == "__main__":
    main()
