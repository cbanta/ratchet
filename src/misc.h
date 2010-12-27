#ifndef __RATCHET_MISC_H
#define __RATCHET_MISC_H

#include <sys/time.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

struct luafunc {
	const char *fname;
	char *fstr;
};

void build_lua_function (lua_State *L, const char *fstr);
void register_luafuncs (lua_State *L, int index, const struct luafunc *fs);
int strmatch (lua_State *L, int index, const char *match);
int strequal (lua_State *L, int index, const char *s2);
void gettimeval (lua_State *L, int index, struct timeval *tv);

#endif
// vim:foldmethod=marker:ai:ts=4:sw=4:
