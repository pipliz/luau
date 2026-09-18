#include "bridge.h"
#include "lua.h"
#include "lualib.h"
#include "luacode.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <cmath>

namespace {
constexpr int MaxWire = 8 * 1024 * 1024;
constexpr int MaxSource = 512 * 1024;
constexpr int MaxNodes = 200000;
struct Module { std::string source; int ref = 0; bool loading = false; };
}
struct luau_host_vm {
    lua_State* state = nullptr;
    size_t used = 0, limit = 0;
    luau_host_callback callback = nullptr;
    void* user = nullptr;
    std::map<std::string, Module> modules;
    std::vector<uint8_t> output, callbackInput, callbackOutput;
    std::string error;
    std::chrono::steady_clock::time_point deadline;
    bool expired = false, busy = false, sealed = false;
    char* bytecode = nullptr;
    int calls = 0, refs = 0, nodes = 0;
    const uint8_t* input = nullptr;
    int inputSize = 0, cursor = 0;
    const char* name = nullptr;
    const char* member = nullptr;
    int operation = 0, reference = 0;
};
namespace {
luau_host_vm* vmof(lua_State* L) { return static_cast<luau_host_vm*>(lua_callbacks(L)->userdata); }
void* alloc(void* ud, void* ptr, size_t oldSize, size_t newSize) {
    auto* v = static_cast<luau_host_vm*>(ud);
    if (!ptr) oldSize = 0;
    if (!newSize) { v->used -= oldSize; free(ptr); return nullptr; }
    if (newSize > oldSize && newSize - oldSize > v->limit - v->used) return nullptr;
    void* result = realloc(ptr, newSize);
    if (result) v->used = v->used - oldSize + newSize;
    return result;
}
void interrupt(lua_State* L, int gc) {
    auto* v = vmof(L);
    if (gc >= 0) return;
    if (v->expired || std::chrono::steady_clock::now() >= v->deadline) {
        v->expired = true;
        if (lua_isyieldable(L)) lua_break(L);
        else luaL_error(L, "script execution budget exceeded");
    }
}
void append(std::vector<uint8_t>& b, const void* p, size_t n) {
    if (b.size() + n > MaxWire) throw std::runtime_error("script value byte limit exceeded");
    auto* s = static_cast<const uint8_t*>(p); b.insert(b.end(), s, s + n);
}
void u8(std::vector<uint8_t>& b, uint8_t x) { append(b, &x, 1); }
void u32(std::vector<uint8_t>& b, uint32_t x) {
    for (int i = 0; i < 4; ++i) u8(b, uint8_t(x >> (i * 8)));
}
void read(luau_host_vm* v, void* p, int n) {
    if (n < 0 || n > v->inputSize - v->cursor) throw std::runtime_error("invalid bridge value");
    memcpy(p, v->input + v->cursor, n); v->cursor += n;
}
uint32_t read32(luau_host_vm* v) {
    uint8_t p[4]; read(v,p,4); return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
void encode(lua_State* L, int index, std::vector<uint8_t>& out, int depth) {
    auto* v = vmof(L);
    if (depth > 32 || ++v->nodes > MaxNodes) throw std::runtime_error("script value depth/node limit exceeded (possible cycle)");
    index = lua_absindex(L,index);
    switch (lua_type(L,index)) {
    case LUA_TNIL: u8(out,0); break;
    case LUA_TBOOLEAN: u8(out,lua_toboolean(L,index) ? 2 : 1); break;
    case LUA_TNUMBER: { double x=lua_tonumber(L,index); if (!std::isfinite(x)) throw std::runtime_error("non-finite script number"); u8(out,3); append(out,&x,8); break; }
    case LUA_TSTRING: { size_t n; const char* s=lua_tolstring(L,index,&n); u8(out,4); u32(out,uint32_t(n)); append(out,s,n); break; }
    case LUA_TTABLE: {
        u8(out,5); size_t offset=out.size(); u32(out,0); uint32_t count=0;
        lua_pushnil(L);
        while (lua_next(L,index)) {
            if (lua_type(L,-2)!=LUA_TSTRING && lua_type(L,-2)!=LUA_TNUMBER) throw std::runtime_error("unsupported table key");
            encode(L,-2,out,depth+1); encode(L,-1,out,depth+1); lua_pop(L,1); ++count;
        }
        for (int i=0;i<4;++i) out[offset+i]=uint8_t(count>>(i*8));
        break;
    }
    case LUA_TFUNCTION:
        if (++v->refs > 4096) throw std::runtime_error("script function reference limit exceeded");
        u8(out,6); u32(out,lua_ref(L,index)); break;
    default: throw std::runtime_error("unsupported script value (expected data or registration function)");
    }
}
void decode(lua_State* L, int depth) {
    auto* v=vmof(L);
    if (depth>32 || ++v->nodes>MaxNodes) throw std::runtime_error("host value depth/node limit exceeded");
    uint8_t tag; read(v,&tag,1);
    switch(tag) {
    case 0: lua_pushnil(L); break;
    case 1: case 2: lua_pushboolean(L,tag==2); break;
    case 3: { double x; read(v,&x,8); lua_pushnumber(L,x); break; }
    case 4: { uint32_t n=read32(v); if(n>uint32_t(v->inputSize-v->cursor)) throw std::runtime_error("invalid string length"); lua_pushlstring(L,reinterpret_cast<const char*>(v->input+v->cursor),n); v->cursor+=n; break; }
    case 5: { uint32_t n=read32(v); if(n>MaxNodes) throw std::runtime_error("host table limit exceeded"); lua_createtable(L,0,0); for(uint32_t i=0;i<n;++i) { decode(L,depth+1); decode(L,depth+1); lua_rawset(L,-3); } break; }
    default: throw std::runtime_error("invalid host value tag");
    }
}
int hostImpl(lua_State* L) {
    auto* v=vmof(L);
    if (v->expired || ++v->calls>10000) { v->expired=true; luaL_error(L,"host call budget exceeded"); }
    v->callbackInput.clear(); v->nodes=0;
    int count=lua_gettop(L); u32(v->callbackInput,count);
    for(int i=1;i<=count;++i) encode(L,i,v->callbackInput,0);
    int n=v->callback(v->user,lua_tointeger(L,lua_upvalueindex(1)),v->callbackInput.data(),int(v->callbackInput.size()),v->callbackOutput.data(),MaxWire);
    // Managed code has returned. Raising errors or allocating VM values is safe now.
    if(n<0) { lua_pushlstring(L,reinterpret_cast<char*>(v->callbackOutput.data()),size_t(-n)); lua_error(L); }
    if(n>MaxWire || n<4) luaL_error(L,"invalid managed callback response");
    v->input=v->callbackOutput.data(); v->inputSize=n; v->cursor=0; v->nodes=0;
    uint32_t results=read32(v); if(results>64) throw std::runtime_error("host result count limit exceeded");
    for(uint32_t i=0;i<results;++i) decode(L,0);
    return int(results);
}
int host(lua_State* L) {
    try { return hostImpl(L); }
    catch(const std::exception& e) { vmof(L)->error=e.what(); }
    catch(...) { vmof(L)->error="native host bridge failure"; }
    luaL_error(L,"%s",vmof(L)->error.c_str());
}
void loadModule(lua_State* L, Module* module, const char* name);
int requireModule(lua_State* L) {
    auto* v=vmof(L); Module* target=nullptr;
    try {
        std::string name=luaL_checkstring(L,1);
        std::string base=lua_tostring(L,lua_upvalueindex(1));
        if(name.compare(0,2,"./")==0) name=base.substr(0,base.find_last_of('/')+1)+name.substr(2);
        if(name.find("..")!=std::string::npos || name.find('\\')!=std::string::npos) throw std::runtime_error("invalid module path");
        auto found=v->modules.find(name);
        if(found==v->modules.end()) throw std::runtime_error("module not in this mod's manifest: "+name);
        target=&found->second;
        // Pointer to map key remains stable; don't keep an owning string over Lua execution.
        v->name=found->first.c_str();
    } catch(const std::exception& e) { v->error=e.what(); }
    if(!target) luaL_error(L,"%s",v->error.c_str());
    loadModule(L,target,v->name); return 1;
}
void loadModule(lua_State* L, Module* module, const char* name) {
    auto* v=vmof(L);
    if(module->ref) { lua_getref(L,module->ref); return; }
    if(module->loading) luaL_error(L,"cyclic module dependency: %s",name);
    module->loading=true;
    lua_State* co=lua_newthread(L); int threadIndex=lua_gettop(L);
    luaL_sandboxthread(co);
    lua_pushstring(co,name); lua_pushcclosure(co,requireModule,"require",1); lua_setglobal(co,"require");
    size_t size=0; char* bytecode=nullptr;
    try { bytecode=luau_compile(module->source.data(),module->source.size(),nullptr,&size); }
    catch(...) { v->error="source compilation failed"; }
    if(!bytecode) luaL_error(L,"source compilation failed");
    v->bytecode=bytecode;
    int status=luau_load(co,name,bytecode,size,0); free(bytecode); v->bytecode=nullptr;
    if(!status) status=lua_resume(co,L,0);
    if(status) {
        if(status==LUA_BREAK || status==LUA_YIELD) luaL_error(L,"module execution budget exceeded or yielded");
        lua_xmove(co,L,1); lua_error(L);
    }
    if(lua_gettop(co)!=1 || (!lua_istable(co,-1) && !lua_isfunction(co,-1))) luaL_error(L,"module must return one table or function: %s",name);
    lua_xmove(co,L,1); module->ref=lua_ref(L,-1); module->loading=false;
    lua_remove(L,threadIndex);
}
int initialize(lua_State* L) {
    luaL_openlibs(L);
    // No independent coroutine scheduler; script execution is synchronous.
    const char* removed[]={"coroutine","debug","getfenv","setfenv","collectgarbage","newproxy",nullptr};
    for(int i=0;removed[i];++i) { lua_pushnil(L); lua_setglobal(L,removed[i]); }
    return 0;
}
int bind(lua_State* L) {
    auto* v=vmof(L); lua_pushinteger(L,v->operation); lua_pushcclosure(L,host,v->name,1); lua_setglobal(L,v->name); return 0;
}
int invokeImpl(lua_State* L) {
    auto* v=vmof(L);
    if(!v->sealed) { luaL_sandbox(L); v->sealed=true; }
    const uint8_t* arguments=v->input; int argumentSize=v->inputSize;
    if(v->reference) lua_getref(L,v->reference);
    else {
        auto found=v->modules.find(v->name);
        if(found==v->modules.end()) luaL_error(L,"unknown module: %s",v->name);
        loadModule(L,&found->second,found->first.c_str());
        if(v->member && *v->member) { lua_getfield(L,-1,v->member); lua_remove(L,-2); }
    }
    if(!lua_isfunction(L,-1)) luaL_error(L,"module export is not callable");
    lua_State* co=lua_newthread(L); lua_pushvalue(L,-2); lua_xmove(L,co,1);
    v->input=arguments; v->inputSize=argumentSize; v->cursor=0; v->nodes=0;
    uint32_t count=read32(v); if(count>64) throw std::runtime_error("argument count limit exceeded");
    for(uint32_t i=0;i<count;++i) { decode(L,0); lua_xmove(L,co,1); }
    int status=lua_resume(co,L,int(count));
    if(status) {
        if(status==LUA_BREAK || status==LUA_YIELD) luaL_error(L,"script execution budget exceeded or yielded");
        lua_xmove(co,L,1); lua_error(L);
    }
    if(v->expired) luaL_error(L,"script execution budget exceeded");
    v->output.clear(); v->nodes=0;
    int n=lua_gettop(co); u32(v->output,n);
    for(int i=1;i<=n;++i) encode(co,i,v->output,0);
    return 0;
}
int invoke(lua_State* L) {
    try { return invokeImpl(L); }
    catch(const std::exception& e) { vmof(L)->error=e.what(); }
    catch(...) { vmof(L)->error="native bridge failure"; }
    luaL_error(L,"%s",vmof(L)->error.c_str());
}
int protect(luau_host_vm* v,lua_CFunction f) {
    int status=lua_cpcall(v->state,f,nullptr);
    free(v->bytecode); v->bytecode=nullptr;
    if(status) { const char* s=lua_tostring(v->state,-1); v->error=s?s:"native VM allocation failure"; }
    lua_settop(v->state,0);
    for(auto& entry:v->modules) entry.second.loading=false;
    return status;
}
}
int luau_host_abi() { return 1; }
luau_host_vm* luau_host_create(uint64_t limit,luau_host_callback cb,void* user) {
    auto* v=new(std::nothrow) luau_host_vm;
    if(!v) return nullptr;
    try {
        v->limit=size_t(limit); v->callback=cb; v->user=user; v->callbackOutput.resize(MaxWire);
        v->state=lua_newstate(alloc,v); if(!v->state) { delete v; return nullptr; }
        lua_callbacks(v->state)->userdata=v;
        if(protect(v,initialize)) { luau_host_destroy(v); return nullptr; }
        return v;
    } catch(...) { luau_host_destroy(v); return nullptr; }
}
void luau_host_destroy(luau_host_vm* v) { if(v) { if(v->state) lua_close(v->state); delete v; } }
int luau_host_bind(luau_host_vm* v,const char* name,int op) {
    if(!v || v->busy || v->sealed) return -1;
    v->name=name; v->operation=op; return protect(v,bind);
}
int luau_host_module(luau_host_vm* v,const char* name,const char* source,int length) {
    if(!v || v->busy || length<0 || length>MaxSource) return -1;
    try { if(v->modules.size()>=1024 || v->modules.count(name)) throw std::runtime_error("duplicate module or module limit exceeded"); v->modules[name].source.assign(source,length); return 0; }
    catch(const std::exception& e) { v->error=e.what(); return -1; }
}
int luau_host_call(luau_host_vm* v,const char* name,const char* member,int ref,const uint8_t* args,int length,int ms,const uint8_t** result,int* size) {
    if(!v || v->busy || length<4 || length>MaxWire || ms<1 || ms>5000) return -1;
    v->busy=true; v->name=name; v->member=member; v->reference=ref;
    v->input=args; v->inputSize=length; v->cursor=0; v->nodes=0; v->calls=0; v->expired=false;
    v->deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    lua_callbacks(v->state)->interrupt=interrupt;
    int status=protect(v,invoke);
    lua_callbacks(v->state)->interrupt=nullptr;
    v->busy=false; *result=v->output.data(); *size=status?0:int(v->output.size());
    return status;
}
const char* luau_host_error(luau_host_vm* v) { return v?v->error.c_str():"VM unavailable"; }
