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
#include <tuple>
#include <algorithm>
#include <limits>

namespace {
constexpr int MaxWire = 8 * 1024 * 1024;
constexpr int MaxSource = 512 * 1024;
constexpr int MaxNodes = 200000;
struct Handle { uint32_t kind, scope, index; };
constexpr int HandleTag = 1;
struct Function { int ref; const void* identity; };
struct Module { std::string source; int ref = 0; bool loading = false; };
}
struct luau_host_vm {
    lua_State* state = nullptr;
    size_t used = 0, limit = 0, peak = 0;
    luau_host_callback callback = nullptr;
    void* user = nullptr;
    std::map<std::string, Module> modules;
    std::vector<uint8_t> output, callbackInput;
    std::string error, traceback;
    int errorKind = LUAU_OK;
    uint32_t payloadLimit=MaxWire, referenceLimit=4096, callLimit=10000, handleLimit=65536;
    uint64_t totalCalls=0;
    int nextReference=1;
    std::map<int, Function> functions;
    std::map<const void*, int> functionIds;
    std::vector<int> pendingRefs;
    std::map<std::tuple<uint32_t,uint32_t,uint32_t>, int> handles;
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
    if (result) { v->used = v->used - oldSize + newSize; v->peak=std::max(v->peak,v->used); }
    return result;
}
void interrupt(lua_State* L, int gc) {
    auto* v = vmof(L);
    if (gc >= 0) return;
    if (v->expired || std::chrono::steady_clock::now() >= v->deadline) {
        v->expired = true; v->errorKind=LUAU_TIMEOUT;
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
    case LUA_TFUNCTION: {
        const void* identity=lua_topointer(L,index);
        auto found=v->functionIds.find(identity);
        int id;
        if(found!=v->functionIds.end()) id=found->second;
        else {
            if(v->functions.size()>=v->referenceLimit || v->nextReference==std::numeric_limits<int>::max()) { v->errorKind=LUAU_RESOURCE_LIMIT; throw std::runtime_error("script reference limit exceeded"); }
            id=v->nextReference++;
            v->pendingRefs.push_back(id);
            v->functions.emplace(id,Function{0,identity});
            v->functions.at(id).ref=lua_ref(L,index);
            v->functionIds.emplace(identity,id);
        }
        u8(out,6); u32(out,id); break;
    }
    case LUA_TUSERDATA: {
        auto* h=static_cast<Handle*>(lua_touserdatatagged(L,index,HandleTag));
        if(!h) { v->errorKind=LUAU_INVALID_HANDLE; throw std::runtime_error("unsupported userdata"); }
        u8(out,7); u32(out,h->kind); u32(out,h->scope); u32(out,h->index); break;
    }
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
    case 3: { double x; read(v,&x,8); if(!std::isfinite(x)) throw std::runtime_error("non-finite host number"); lua_pushnumber(L,x); break; }
    case 4: { uint32_t n=read32(v); if(n>uint32_t(v->inputSize-v->cursor)) throw std::runtime_error("invalid string length"); lua_pushlstring(L,reinterpret_cast<const char*>(v->input+v->cursor),n); v->cursor+=n; break; }
    case 5: { uint32_t n=read32(v); if(n>MaxNodes) throw std::runtime_error("host table limit exceeded"); lua_createtable(L,0,0); for(uint32_t i=0;i<n;++i) { decode(L,depth+1); decode(L,depth+1); lua_rawset(L,-3); } break; }
    case 7: {
        Handle h{read32(v),read32(v),read32(v)};
        if((h.kind!=1 && h.kind!=2) || !h.scope) { v->errorKind=LUAU_INVALID_HANDLE; throw std::runtime_error("invalid handle kind or scope"); }
        auto key=std::make_tuple(h.kind,h.scope,h.index);
        auto found=v->handles.find(key);
        if(found!=v->handles.end() && found->second) { lua_getref(L,found->second); break; }
        if(v->handles.size()>=v->handleLimit) { v->errorKind=LUAU_RESOURCE_LIMIT; throw std::runtime_error("handle limit exceeded"); }
        auto* data=static_cast<Handle*>(lua_newuserdatatagged(L,sizeof(Handle),HandleTag));
        *data=h;
        // Insert before taking a Lua reference so allocation failures cannot leak it.
        auto entry=v->handles.emplace(key,0).first;
        entry->second=lua_ref(L,-1);
        break;
    }
    default: throw std::runtime_error("invalid host value tag");
    }
}
int hostImpl(lua_State* L) {
    auto* v=vmof(L);
    ++v->totalCalls;
    if (v->expired || ++v->calls>int(v->callLimit)) { v->expired=true; v->errorKind=LUAU_RESOURCE_LIMIT; luaL_error(L,"host call budget exceeded"); }
    v->callbackInput.clear(); v->nodes=0;
    int count=lua_gettop(L); if(count>64) { v->errorKind=LUAU_BAD_ARGUMENT; luaL_error(L,"argument count limit exceeded"); }
    u32(v->callbackInput,count);
    for(int i=1;i<=count;++i) encode(L,i,v->callbackInput,0);
    if(v->callbackInput.size()>v->payloadLimit) { v->errorKind=LUAU_RESOURCE_LIMIT; throw std::runtime_error("callback payload limit exceeded"); }
    v->pendingRefs.clear(); // These references now belong to the host.
    const uint8_t* output=nullptr; int n=0;
    int error=v->callback(v->user,lua_tointeger(L,lua_upvalueindex(1)),v->callbackInput.data(),int(v->callbackInput.size()),&output,&n);
    // Managed code has returned; decoding and raising Lua errors are safe now.
    if(n<0 || n>int(v->payloadLimit) || (!output && n)) { v->errorKind=LUAU_BAD_ARGUMENT; luaL_error(L,"invalid managed callback response"); }
    if(error) { v->errorKind=(error>=LUAU_SCRIPT_ERROR && error<=LUAU_RESOURCE_LIMIT)?error:LUAU_HOST_ERROR; lua_pushlstring(L,reinterpret_cast<const char*>(output),size_t(std::min(n,4096))); lua_error(L); }
    v->input=output; v->inputSize=n; v->cursor=0; v->nodes=0;
    uint32_t results=read32(v); if(results>64) throw std::runtime_error("host result count limit exceeded");
    for(uint32_t i=0;i<results;++i) decode(L,0);
    if(v->cursor!=v->inputSize) throw std::runtime_error("trailing host data");
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
        v->traceback=lua_debugtrace(co);
        if(status==LUA_ERRMEM) v->errorKind=LUAU_MEMORY_LIMIT;
        if(status==LUA_BREAK || status==LUA_YIELD) v->errorKind=LUAU_TIMEOUT;
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
    auto* v=vmof(L);
    if(strncmp(v->name,"game.",5)==0) {
        lua_getglobal(L,"game");
        if(lua_isnil(L,-1)) { lua_pop(L,1); lua_newtable(L); lua_pushvalue(L,-1); lua_setglobal(L,"game"); }
        lua_pushinteger(L,v->operation); lua_pushcclosure(L,host,v->name,1); lua_setfield(L,-2,v->name+5);
    } else { lua_pushinteger(L,v->operation); lua_pushcclosure(L,host,v->name,1); lua_setglobal(L,v->name); }
    return 0;
}
void freeze(lua_State* L, int index) {
    index=lua_absindex(L,index);
    if(!lua_istable(L,index)) return;
    lua_pushnil(L);
    while(lua_next(L,index)) { freeze(L,-1); lua_pop(L,1); }
    lua_setreadonly(L,index,true);
}
int constant(lua_State* L) {
    auto* v=vmof(L);
    decode(L,0); freeze(L,-1);
    if(strncmp(v->name,"game.",5)==0) { lua_getglobal(L,"game"); lua_pushvalue(L,-2); lua_setfield(L,-2,v->name+5); }
    else lua_setglobal(L,v->name);
    return 0;
}
int constantSafe(lua_State* L) {
    try { return constant(L); } catch(const std::exception& e) { vmof(L)->error=e.what(); }
    luaL_error(L,"%s",vmof(L)->error.c_str());
}
int invokeImpl(lua_State* L) {
    auto* v=vmof(L);
    if(!v->sealed) { luaL_sandbox(L); v->sealed=true; }
    const uint8_t* arguments=v->input; int argumentSize=v->inputSize;
    if(v->reference) {
        auto found=v->functions.find(v->reference);
        if(found==v->functions.end()) { v->errorKind=LUAU_INVALID_HANDLE; luaL_error(L,"released or invalid function handle"); }
        lua_getref(L,found->second.ref);
    }
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
    if(v->cursor!=v->inputSize) { v->errorKind=LUAU_BAD_ARGUMENT; throw std::runtime_error("trailing arguments"); }
    int status=lua_resume(co,L,int(count));
    if(status) {
        v->traceback=lua_debugtrace(co);
        if(status==LUA_ERRMEM) v->errorKind=LUAU_MEMORY_LIMIT;
        if(status==LUA_BREAK || status==LUA_YIELD) v->errorKind=LUAU_TIMEOUT;
        if(status==LUA_BREAK || status==LUA_YIELD) luaL_error(L,"script execution budget exceeded or yielded");
        lua_xmove(co,L,1); lua_error(L);
    }
    if(v->expired) luaL_error(L,"script execution budget exceeded");
    v->output.clear(); v->nodes=0;
    int n=lua_gettop(co); u32(v->output,n);
    for(int i=1;i<=n;++i) encode(co,i,v->output,0);
    if(v->output.size()>v->payloadLimit) { v->errorKind=LUAU_RESOURCE_LIMIT; throw std::runtime_error("result payload limit exceeded"); }
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
    if(status) { if(status==LUA_ERRMEM) v->errorKind=LUAU_MEMORY_LIMIT; else if(!v->errorKind) v->errorKind=LUAU_SCRIPT_ERROR; const char* s=lua_tostring(v->state,-1); v->error=s?s:"native VM allocation failure"; }
    if(status) {
        if(v->error.size()>4096) v->error.resize(4096);
        if(v->traceback.size()>8192) v->traceback.resize(8192);
        for(int id:v->pendingRefs) {
            auto found=v->functions.find(id);
            if(found!=v->functions.end()) {
                if(found->second.ref) lua_unref(v->state,found->second.ref);
                v->functionIds.erase(found->second.identity); v->functions.erase(found);
            }
        }
    }
    v->pendingRefs.clear();
    lua_settop(v->state,0);
    for(auto& entry:v->modules) entry.second.loading=false;
    return status;
}
}
int luau_host_abi() { return 2; }
luau_host_vm* luau_host_create(const luau_host_options* options,luau_host_callback cb,void* user) {
    if(!options || options->size<sizeof(luau_host_options) || options->abi!=2 || !cb ||
       options->memory_limit<1024*1024 || options->memory_limit>SIZE_MAX ||
       options->payload_limit<4 || options->payload_limit>MaxWire ||
       !options->reference_limit || options->reference_limit>1000000 ||
       !options->host_call_limit || options->host_call_limit>1000000 ||
       !options->handle_limit || options->handle_limit>1000000) return nullptr;
    auto* v=new(std::nothrow) luau_host_vm;
    if(!v) return nullptr;
    try {
        v->limit=size_t(options->memory_limit); v->callback=cb; v->user=user;
        v->payloadLimit=options->payload_limit; v->referenceLimit=options->reference_limit;
        v->callLimit=options->host_call_limit; v->handleLimit=options->handle_limit;
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
    if(!v || v->busy || length<4 || length>int(v->payloadLimit) || ms<1 || ms>5000) return -1;
    v->error.clear(); v->traceback.clear(); v->errorKind=LUAU_OK;
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

int luau_host_error_kind(luau_host_vm* v) { return v?v->errorKind:LUAU_BAD_ARGUMENT; }
const char* luau_host_traceback(luau_host_vm* v) { return v?v->traceback.c_str():""; }
int luau_host_release(luau_host_vm* v,int id) {
    if(!v || v->busy) return LUAU_BAD_ARGUMENT;
    auto found=v->functions.find(id);
    if(found==v->functions.end()) return LUAU_INVALID_HANDLE;
    lua_unref(v->state,found->second.ref); v->functionIds.erase(found->second.identity); v->functions.erase(found);
    return 0;
}
int luau_host_statistics(luau_host_vm* v,luau_host_stats* s) {
    if(!v || !s || s->size<sizeof(luau_host_stats)) return LUAU_BAD_ARGUMENT;
    s->live_references=uint32_t(v->functions.size()); s->memory_used=v->used; s->memory_peak=v->peak;
    s->host_calls=v->totalCalls; s->last_host_calls=v->calls; s->live_handles=uint32_t(v->handles.size()); return 0;
}
int luau_host_constant(luau_host_vm* v,const char* name,const uint8_t* value,int length) {
    if(!v || v->busy || v->sealed || !name || !value || length<1 || length>int(v->payloadLimit)) return LUAU_BAD_ARGUMENT;
    v->name=name; v->input=value; v->inputSize=length; v->cursor=0; v->nodes=0;
    return protect(v,constantSafe);
}
