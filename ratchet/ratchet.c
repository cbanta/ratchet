#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "misc.h"
#include "makeclass.h"
#include "ratchet.h"
#include "dns.h"
#include "context.h"
#include "epoll.h"
#include "parseuri.h"

/* {{{ luaH_ratchet_add_types() */
static void luaH_ratchet_add_types (lua_State *L)
{
	lua_getfield (L, -2, "socket");
	lua_setfield (L, -2, "tcp");

	lua_getfield (L, -2, "socket");
	lua_setfield (L, -2, "unix");
}
/* }}} */

/* {{{ ratchet_init() */
static int ratchet_init (lua_State *L)
{
	return 0;
}
/* }}} */

/* {{{ ratchet_urifactory() */
static int ratchet_urifactory (lua_State *L)
{
	int rets;

	/* Process the URI based on known schemas. */
	lua_getfield (L, 1, "parseuri");
	lua_pushvalue (L, 2);
	lua_getfield (L, 1, "uri_schemas");
	rets = luaH_callfunction (L, -3, 2);
	if (rets != 2 || lua_isnil (L, -1))
	{
		lua_pop (L, rets+1);
		lua_pushnil (L);
		return 1;
	}
	lua_remove (L, -3);

	/* Get the ratchet type handler for the URI schema. */
	lua_getfield (L, 1, "ratchet_types");
	lua_pushvalue (L, -3);
	lua_gettable (L, -2);
	if (lua_isnil (L, -1))
	{
		lua_pop (L, 4);
		lua_pushnil (L);
		return 1;
	}
	lua_remove (L, -2);

	/* Get the ratchet object from the handler, given the data. */
	lua_pushvalue (L, -2);
	lua_call (L, 1, 1);
	lua_remove (L, -2);

	return 2;
}
/* }}} */

/* {{{ ratchet_instantiate_context() */
static int ratchet_instantiate_context (lua_State *L)
{
	int args = lua_gettop (L);
	int extra_args = (args > 3 ? args-3 : 0);
	luaL_checkstring (L, 2);

	if (args < 3)
	{
		luaH_callmethod (L, 1, "new_context", 0);
		lua_insert (L, 3);
	}

	lua_pushvalue (L, 2);
	int rets = luaH_callmethod (L, 1, "urifactory", 1);
	if (rets != 2 || lua_isnil (L, -1))
		return 0;
	lua_insert (L, 4);
	lua_insert (L, 4);
	lua_pushvalue (L, 1);
	lua_insert (L, 4);

	return luaH_callfunction (L, 3, 3+extra_args);
}
/* }}} */

/* {{{ ratchet_connect() */
static int ratchet_connect (lua_State *L)
{
	int args = lua_gettop (L) - 1;
	int rets = luaH_callmethod (L, 1, "instantiate_context", args);
	if (rets != 1)
		return 0;
	lua_getfield (L, -1, "engine");
	luaH_callmethod (L, -1, "connect", 0);
	lua_pop (L, 1);
	return 1;
}
/* }}} */

/* {{{ ratchet_listen() */
static int ratchet_listen (lua_State *L)
{
	int args = lua_gettop (L) - 1;
	int rets = luaH_callmethod (L, 1, "instantiate_context", args);
	if (rets != 1)
		return 0;
	lua_getfield (L, -1, "engine");
	luaH_callmethod (L, -1, "listen", 0);
	lua_pop (L, 1);
	return 1;
}
/* }}} */

/* {{{ ratchet_close() */
static int ratchet_close (lua_State *L)
{
}
/* }}} */

/* {{{ ratchet_new_context() */
static int ratchet_new_context (lua_State *L)
{
	lua_pushcfunction (L, luaH_ratchet_new_context);
	lua_call (L, 0, 1);
	return 1;
}
/* }}} */

/* {{{ ratchet_run_once() */
static int ratchet_run_once (lua_State *L)
{

	return 0;
}
/* }}} */

/* {{{ ratchet_run() */
static int ratchet_run (lua_State *L)
{
	int iterations = -1;
	int i;
	int args = lua_gettop (L) - 1;

	if (lua_istable (L, 2))
	{
		lua_getfield (L, 2, "iterations");
		if (lua_isnumber (L, -1))
			iterations = lua_tointeger (L, -1);
		lua_pop (L, 1);
	}
	else if (!lua_isnoneornil (L, 2))
		luaL_checktype (L, 2, LUA_TTABLE);

	for (i=0; i<iterations || iterations<0; i++)
	{
		luaH_callmethod (L, 1, "run_once", args);
		lua_settop (L, 2);
	}

	return 0;
}
/* }}} */

/* {{{ luaopen_luah_ratchet() */
int luaopen_luah_ratchet (lua_State *L)
{
	const luaL_Reg meths[] = {
		{"init", ratchet_init},
		{"parseuri", luaH_parseuri},
		{"urifactory", ratchet_urifactory},
		{"instantiate_context", ratchet_instantiate_context},
		{"connect", ratchet_connect},
		{"listen", ratchet_listen},
		{"close", ratchet_close},
		{"new_context", ratchet_new_context},
		{"run_once", ratchet_run_once},
		{"run", ratchet_run},
		{NULL}
	};

	luaH_newclass (L, "luah.ratchet", meths);

	/* Set up submodules. */
	luaopen_luah_ratchet_epoll (L);
	luaH_setclassfield (L, -2, "epoll");
	luaopen_luah_ratchet_socket (L);
	luaH_setclassfield (L, -2, "socket");
	luaopen_luah_ratchet_dns (L);
	luaH_setclassfield (L, -2, "dns");

	/* Set up URI schema table. */
	lua_newtable (L);
	luaH_parseuri_add_builtin (L);
	lua_setfield (L, -2, "uri_schemas");

	/* Set up the ratchet type table. */
	lua_newtable (L);
	luaH_ratchet_add_types (L);
	lua_setfield (L, -2, "ratchet_types");

	return 1;
}
/* }}} */

// vim:foldmethod=marker:ai:ts=4:sw=4:
