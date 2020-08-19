#include "lua.h"
#include "lauxlib.h"

#if LUA_VERSION_NUM > 501 || defined(LUAI_BITSINT)
#define PUCRIOLUA
#endif

#ifdef PUCRIOLUA
#if LUA_VERSION_NUM == 501
#include "lua51/lobject.h"
#elif LUA_VERSION_NUM == 502
#include "lua52/lobject.h"
#elif LUA_VERSION_NUM == 503
#include "lua53/lobject.h"
#elif LUA_VERSION_NUM == 504
#include "lua54/lobject.h"
#else
#error unsupported Lua version
#endif
#else /* LuaJIT */
#include "lj2/lj_obj.h"
#endif

#ifdef PUCRIOLUA

static Proto *get_proto(lua_State *L) {
    return ((Closure *) lua_topointer(L, 1))->l.p;
}

#if LUA_VERSION_NUM == 504

#define ABSLINEINFO (-0x80)

/*
** Get a "base line" to find the line corresponding to an instruction.
** For that, search the array of absolute line info for the largest saved
** instruction smaller or equal to the wanted instruction. A special
** case is when there is no absolute info or the instruction is before
** the first absolute one.
*/
static int getbaseline (const Proto *f, int pc, int *basepc) {
  if (f->sizeabslineinfo == 0 || pc < f->abslineinfo[0].pc) {
    *basepc = -1;  /* start from the beginning */
    return f->linedefined;
  }
  else {
    unsigned int i;
    if (pc >= f->abslineinfo[f->sizeabslineinfo - 1].pc)
      i = f->sizeabslineinfo - 1;  /* instruction is after last saved one */
    else {  /* binary search */
      unsigned int j = f->sizeabslineinfo - 1;  /* pc < anchorlines[j] */
      i = 0;  /* abslineinfo[i] <= pc */
      while (i < j - 1) {
        unsigned int m = (j + i) / 2;
        if (pc >= f->abslineinfo[m].pc)
          i = m;
        else
          j = m;
      }
    }
    *basepc = f->abslineinfo[i].pc;
    return f->abslineinfo[i].line;
  }
}

/*
** Get the line corresponding to instruction 'pc' in function 'f';
** first gets a base line and from there does the increments until
** the desired instruction.
*/
int luaG_getfuncline (const Proto *f, int pc) {
  if (f->lineinfo == NULL)  /* no debug information? */
    return -1;
  else {
    int basepc;
    int baseline = getbaseline(f, pc, &basepc);
    while (basepc++ < pc) {  /* walk until given instruction */
      baseline += f->lineinfo[basepc];  /* correct line */
    }
    return baseline;
  }
}

static int nextline (const Proto *p, int currentline, int pc) {
  if (p->lineinfo[pc] != ABSLINEINFO)
    return currentline + p->lineinfo[pc];
  else
    return luaG_getfuncline(p, pc);
}

static void add_activelines(lua_State *L, Proto *proto) {
    /*
    ** For standard Lua active lines and nested prototypes
    ** are simply members of Proto, see lobject.h.
    */
    int i;
    int currentline = proto->linedefined;

    for (i = 0; i < proto->sizelineinfo; i++) {  /* for all lines with code */
        currentline = nextline(proto, currentline, i);
        lua_pushinteger(L, currentline);
        lua_pushboolean(L, 1);
        lua_settable(L, -3);
    }

    for (i = 0; i < proto->sizep; i++) {
        add_activelines(L, proto->p[i]);
    }
}

# else /* LUA_VERSION_NUM == 504 */

static void add_activelines(lua_State *L, Proto *proto) {
    /*
    ** For standard Lua active lines and nested prototypes
    ** are simply members of Proto, see lobject.h.
    */
    int i;

    for (i = 0; i < proto->sizelineinfo; i++) {
        lua_pushinteger(L, proto->lineinfo[i]);
        lua_pushboolean(L, 1);
        lua_settable(L, -3);
    }

    for (i = 0; i < proto->sizep; i++) {
        add_activelines(L, proto->p[i]);
    }
}

#endif /* LUA_VERSION_NUM == 504 */

#else /* LuaJIT */

static GCproto *get_proto(lua_State *L) {
    return funcproto(funcV(L->base));
}

static void add_activelines(lua_State *L, GCproto *proto) {
    /*
    ** LuaJIT packs active lines depending on function length.
    ** See implementation of lj_debug_getinfo in lj_debug.c.
    */
    ptrdiff_t idx;
    const void *lineinfo = proto_lineinfo(proto);

    if (lineinfo) {
        BCLine first = proto->firstline;
        int sz = proto->numline < 256 ? 1 : proto->numline < 65536 ? 2 : 4;
        MSize i, szl = proto->sizebc - 1;

        for (i = 0; i < szl; i++) {
            BCLine line = first +
                (sz == 1 ? (BCLine) ((const uint8_t *) lineinfo)[i] :
                 sz == 2 ? (BCLine) ((const uint16_t *) lineinfo)[i] :
                 (BCLine) ((const uint32_t *) lineinfo)[i]);
            lua_pushinteger(L, line);
            lua_pushboolean(L, 1);
            lua_settable(L, -3);
        }
    }

    /*
    ** LuaJIT stores nested prototypes as garbage-collectible constants,
    ** iterate over them. See implementation of jit_util_funck in lib_jit.c.
    */
    for (idx = -1; ~idx < (ptrdiff_t) proto->sizekgc; idx--) {
        GCobj *gc = proto_kgc(proto, idx);

        if (~gc->gch.gct == LJ_TPROTO) {
            add_activelines(L, (GCproto *) gc);
        }
    }
}

#endif

static int l_deepactivelines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_argcheck(L, !lua_iscfunction(L, 1), 1,
        "Lua function expected, got C function");
    lua_settop(L, 1);
    lua_newtable(L);
    add_activelines(L, get_proto(L));
    return 1;
}

int luaopen_cluacov_deepactivelines(lua_State *L) {
    lua_newtable(L);
    lua_pushliteral(L, "0.1.0");
    lua_setfield(L, -2, "version");
    lua_pushcfunction(L, l_deepactivelines);
    lua_setfield(L, -2, "get");
    return 1;
}
