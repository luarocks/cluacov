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
#include "luajit.h"
#if LUAJIT_VERSION_NUM == 20199
#include "lj2/lua_assert.h"
#include "lj2/lj_obj.h"
#elif LUAJIT_VERSION_NUM == 20100
#include "luajit-2.1.0-beta3/lua_assert.h"
#include "luajit-2.1.0-beta3/lj_obj.h"
#else
#error unsupported LuaJIT version
#endif
#endif

#ifdef PUCRIOLUA

static Proto *get_proto(lua_State *L) {
    return ((Closure *) lua_topointer(L, 1))->l.p;
}

#if LUA_VERSION_NUM == 504

/*
** mark for entries in 'lineinfo' array that has absolute information in
** 'abslineinfo' array
*/
#define ABSLINEINFO	(-0x80)

/*
** MAXimum number of successive Instructions WiTHout ABSolute line
** information. (A power of two allows fast divisions.)
*/
#if !defined(MAXIWTHABS)
#define MAXIWTHABS	128
#endif

/*
** Get a "base line" to find the line corresponding to an instruction.
** Base lines are regularly placed at MAXIWTHABS intervals, so usually
** an integer division gets the right place. When the source file has
** large sequences of empty/comment lines, it may need extra entries,
** so the original estimate needs a correction.
** If the original estimate is -1, the initial 'if' ensures that the
** 'while' will run at least once.
** The assertion that the estimate is a lower bound for the correct base
** is valid as long as the debug info has been generated with the same
** value for MAXIWTHABS or smaller. (Previous releases use a little
** smaller value.)
*/
static int getbaseline (const Proto *f, int pc, int *basepc) {
  if (f->sizeabslineinfo == 0 || pc < f->abslineinfo[0].pc) {
    *basepc = -1;  /* start from the beginning */
    return f->linedefined;
  }
  else {
    int i = cast_uint(pc) / MAXIWTHABS - 1;  /* get an estimate */
    /* estimate must be a lower bound of the correct base */
    lua_assert(i < 0 ||
              (i < f->sizeabslineinfo && f->abslineinfo[i].pc <= pc));
    while (i + 1 < f->sizeabslineinfo && pc >= f->abslineinfo[i + 1].pc)
      i++;  /* low estimate; adjust it */
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
  if (p->lineinfo == NULL)  /* no debug information? */
    return -1;

  if (p->lineinfo[pc] != ABSLINEINFO) {
    return currentline + p->lineinfo[pc];
  } else {
    return luaG_getfuncline(p, pc);
  }
}

static void add_activelines(lua_State *L, Proto *p) {
    /*
    ** For standard Lua active lines and nested prototypes
    ** are simply members of Proto, see lobject.h.
    */
    int i;
    int currentline = p->linedefined;

#if LUA_VERSION_RELEASE_NUM >= 50404
    if (!p->is_vararg)  /* regular function? */
      i = 0;  /* consider all instructions */
    else {  /* vararg function */
      currentline = nextline(p, currentline, 0);
      if (currentline == -1) {
        return;
      }
      i = 1;  /* skip first instruction (OP_VARARGPREP) */
    }
#else
    i = 0;
#endif

    for (; i < p->sizelineinfo; i++) {  /* for all lines with code */
        currentline = nextline(p, currentline, i);
        lua_pushinteger(L, currentline);
        lua_pushboolean(L, 1);
        lua_settable(L, -3);
    }

    for (i = 0; i < p->sizep; i++) {
        add_activelines(L, p->p[i]);
    }
}

#else /* PUC-Rio Lua below 5.4 */

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

#endif

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
    lua_pushliteral(L, "0.1.4");
    lua_setfield(L, -2, "version");
    lua_pushcfunction(L, l_deepactivelines);
    lua_setfield(L, -2, "get");
    return 1;
}
