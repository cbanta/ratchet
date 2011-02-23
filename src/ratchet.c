/* Copyright (c) 2010 Ian C. Good
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include "config.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <event.h>
#include <netdb.h>
#include <string.h>

#include "ratchet.h"
#include "misc.h"

#define get_event_base(L, index) (*(struct event_base **) luaL_checkudata (L, index, "ratchet_meta"))
#define get_thread(L, index, s) luaL_checktype (L, index, LUA_TTHREAD); lua_State *s = lua_tothread (L, index)

/* {{{ return_first_upvalue() */
static int return_first_upvalue (lua_State *L)
{
	lua_pushvalue (L, lua_upvalueindex (1));
	return 1;
}
/* }}} */

/* {{{ setup_persistance_tables() */
static int setup_persistance_tables (lua_State *L)
{
	lua_newtable (L);

	/* Set up tables to store references to threads for persistence.
	 * There are two types of threads:
	 *
	 *     Foreground:  Calls to ratchet:loop() will not stop until all these
	 *                  threads are completed.
	 *
	 *     Background:  The existence of these threads will not force
	 *                  ratchet:loop() to keep going, however when the loop
	 *                  does break these threads will be destroyed.
	 */
	lua_newtable (L);
	lua_setfield (L, -2, "foreground");
	lua_newtable (L);
	lua_setfield (L, -2, "background");

	/* Set up a weak-ref table to track what threads are not yet started. */
	lua_newtable (L);
	lua_newtable (L);
	lua_pushliteral (L, "kv");
	lua_setfield (L, -2, "__mode");
	lua_setmetatable (L, -2);
	lua_setfield (L, -2, "ready");

	/* Set up a weak-key table to track what threads a thread is waiting on. */
	lua_newtable (L);
	lua_newtable (L);
	lua_pushliteral (L, "kv");
	lua_setfield (L, -2, "__mode");
	lua_setmetatable (L, -2);
	lua_setfield (L, -2, "waiting_on");

	/* Set up a weak-key table to track thread error handlers. */
	lua_newtable (L);
	lua_newtable (L);
	lua_pushliteral (L, "k");
	lua_setfield (L, -2, "__mode");
	lua_pushnil (L);
	lua_pushcclosure (L, return_first_upvalue, 1);
	lua_setfield (L, -2, "__index");
	lua_setmetatable (L, -2);
	lua_setfield (L, -2, "error_handlers");

	return 1;
}
/* }}} */

/* {{{ set_thread_persist() */
static void set_thread_persist (lua_State *L, int index, int background)
{
	if (background)
	{
		lua_getfenv (L, 1);
		lua_getfield (L, -1, "background");

		lua_pushvalue (L, index);
		lua_pushboolean (L, 1);
		lua_settable (L, -3);

		lua_pop (L, 2);
	}
	else
	{
		lua_getfenv (L, 1);
		lua_getfield (L, -1, "foreground");

		lua_pushvalue (L, index);
		lua_pushboolean (L, 1);
		lua_settable (L, -3);

		lua_pop (L, 2);
	}
}
/* }}} */

/* {{{ set_thread_ready() */
static void set_thread_ready (lua_State *L, int index)
{
	lua_getfenv (L, 1);
	lua_getfield (L, -1, "ready");

	lua_pushvalue (L, index);
	lua_pushboolean (L, 1);
	lua_settable (L, -3);

	lua_pop (L, 2);
}
/* }}} */

/* {{{ end_thread_persist() */
static void end_thread_persist (lua_State *L, int index)
{
	lua_getfenv (L, 1);

	lua_getfield (L, -1, "background");
	lua_pushvalue (L, index);
	lua_pushnil (L);
	lua_settable (L, -3);
	lua_pop (L, 1);

	lua_getfield (L, -1, "foreground");
	lua_pushvalue (L, index);
	lua_pushnil (L);
	lua_settable (L, -3);
	lua_pop (L, 1);

	lua_getfield (L, -1, "waiting_on");
	for (lua_pushnil (L); lua_next (L, -2) != 0; lua_pop (L, 1))
	{
		lua_pushvalue (L, index);
		lua_pushnil (L);
		lua_rawset (L, -3);
	}
	lua_pop (L, 1);

	lua_pop (L, 1);
}
/* }}} */

/* {{{ get_fd_from_object() */
static int get_fd_from_object (lua_State *L, int index)
{
	lua_pushvalue (L, index);
	lua_getfield (L, -1, "get_fd");
	if (!lua_isfunction (L, -1))
		luaL_argerror (L, index, "object has no get_fd() method");
	lua_insert (L, -2);
	lua_call (L, 1, 1);

	int fd = lua_tointeger (L, -1);
	lua_pop (L, 1);
	return fd;
}
/* }}} */

/* {{{ get_timeout_from_object() */
static double get_timeout_from_object (lua_State *L, int index)
{
	lua_pushvalue (L, index);
	lua_getfield (L, -1, "get_timeout");
	if (!lua_isfunction (L, -1))
	{
		lua_pop (L, 2);
		return -1.0;
	}
	lua_insert (L, -2);
	lua_call (L, 1, 1);

	double timeout = (double) lua_tonumber (L, -1);
	lua_pop (L, 1);
	return timeout;
}
/* }}} */

/* {{{ event_triggered() */
static void event_triggered (int fd, short event, void *arg)
{
	lua_State *L1 = (lua_State *) arg;
	lua_State *L = lua_tothread (L1, 1);

	/* Call the run_thread() helper method. */
	lua_getfield (L, 1, "run_thread");
	lua_pushvalue (L, 1);
	lua_settop (L1, 0);
	lua_pushthread (L1);
	lua_xmove (L1, L, 1);
	lua_call (L, 2, 0);
}
/* }}} */

/* ---- Namespace Functions ------------------------------------------------- */

/* {{{ ratchet_new() */
static int ratchet_new (lua_State *L)
{
	struct event_base **new = (struct event_base **) lua_newuserdata (L, sizeof (struct event_base *));
	*new = event_base_new ();
	if (!*new)
		return luaL_error (L, "failed to create event_base structure");

	luaL_getmetatable (L, "ratchet_meta");
	lua_setmetatable (L, -2);

	/* Set up persistance table. */
	setup_persistance_tables (L);
	lua_setfenv (L, -2);

	return 1;
}
/* }}} */

/* {{{ ratchet_stackdump() */
static int ratchet_stackdump (lua_State *L)
{
	stackdump (L);
	return 0;
}
/* }}} */

/* ---- Member Functions ---------------------------------------------------- */

/* {{{ ratchet_gc() */
static int ratchet_gc (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	event_base_free (e_b);

	return 0;
}
/* }}} */

/* {{{ ratchet_event_gc() */
static int ratchet_event_gc (lua_State *L)
{
	struct event *ev = (struct event *) luaL_checkudata (L, 1, "ratchet_event_internal_meta");
	event_del (ev);

	return 0;
}
/* }}} */

/* {{{ ratchet_get_method() */
static int ratchet_get_method (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	lua_pushstring (L, event_base_get_method (e_b));
	return 1;
}
/* }}} */

/* {{{ ratchet_set_error_handler() */
static int ratchet_set_error_handler (lua_State *L)
{
	get_event_base (L, 1);
	luaL_checkany (L, 2);
	int nargs = lua_gettop (L) - 1;
	int i;

	/* Set up table with error handler call stack. */
	lua_createtable (L, nargs, 0);
	for (i=1; i<=nargs; i++)
	{
		lua_pushvalue (L, i+1);
		lua_rawseti (L, -2, i);
	}
	lua_replace (L, 2);
	lua_settop (L, 2);

	/* Set the error handler, global or thread-specific. */
	lua_getfenv (L, 1);
	lua_getfield (L, -1, "error_handlers");
	if (lua_pushthread (L))
	{
		/* The main thread isn't needed. */
		lua_pop (L, 1);

		/* Set new default error handler as fallback. */
		lua_getmetatable (L, -1);
		lua_pushvalue (L, 2);
		lua_pushcclosure (L, return_first_upvalue, 1);
		lua_setfield (L, -2, "__index");
		lua_pop (L, 1);
	}
	else
	{
		lua_pushvalue (L, 2);
		lua_rawset (L, -3);	/* Key: thread, Value: call stack table. */
	}
	lua_pop (L, 2);

	return 0;
}
/* }}} */

/* {{{ ratchet_attach() */
static int ratchet_attach (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	luaL_checkany (L, 2);	/* Function or callable object. */
	int nargs = lua_gettop (L) - 2;

	/* Set up new coroutine. */
	lua_State *L1 = lua_newthread (L);
	lua_insert (L, 2);
	lua_xmove (L, L1, nargs+1);

	set_thread_persist (L, 2 /* index of thread */, 0 /* foreground thread */);
	set_thread_ready (L, 2 /* index of thread */);
	event_base_loopbreak (e_b);	/* So that new threads get started. */

	lua_pushvalue (L, 2);
	return 1;
}
/* }}} */

/* {{{ ratchet_attach_background() */
static int ratchet_attach_background (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	luaL_checkany (L, 2);	/* Function or callable object. */
	int nargs = lua_gettop (L) - 2;

	/* Set up new coroutine. */
	lua_State *L1 = lua_newthread (L);
	lua_insert (L, 2);
	lua_xmove (L, L1, nargs+1);

	set_thread_persist (L, 2 /* index of thread */, 1 /* background thread */);
	set_thread_ready (L, 2 /* index of thread */);
	event_base_loopbreak (e_b);	/* So that new threads get started. */

	lua_pushvalue (L, 2);
	return 1;
}
/* }}} */

/* {{{ ratchet_wait_all() */
static int ratchet_wait_all (lua_State *L)
{
	int i;
	struct event_base *e_b = get_event_base (L, 1);
	luaL_checktype (L, 2, LUA_TTABLE);
	lua_settop (L, 2);
	if (lua_pushthread (L))
		return luaL_error (L, "wait_all cannot be called from main thread");
	lua_pop (L, 1);

	lua_getfenv (L, 1);
	lua_getfield (L, -1, "waiting_on");

	/* Iterate through table of children, building duplicate with
	 * the child threads as weak keys. */
	lua_newtable (L);
	lua_getmetatable (L, 4);
	lua_setmetatable (L, -2);
	for (i=1; ; i++)
	{
		lua_rawgeti (L, 2, i);
		if (lua_isnil (L, -1))
		{
			lua_pop (L, 1);
			break;
		}
		lua_pushboolean (L, 1);
		lua_rawset (L, 5);
	}

	lua_pushthread (L);
	lua_pushvalue (L, 5);
	lua_settable (L, 4);
	lua_settop (L, 2);

	event_base_loopbreak (e_b);	/* So that new threads get started. */

	return lua_yield (L, 0);
}
/* }}} */

/* {{{ ratchet_running_thread() */
static int ratchet_running_thread (lua_State *L)
{
	if (lua_pushthread (L))
		lua_pushnil (L);
	return 1;
}
/* }}} */

/* {{{ ratchet_timer() */
static int ratchet_timer (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	if (lua_pushthread (L))
		return luaL_error (L, "timer cannot be called from main thread");
	lua_pop (L, 1);

	int nargs = lua_gettop (L) - 1;
	lua_pushliteral (L, "timeout");
	lua_insert (L, 2);

	return lua_yield (L, nargs+1);
}
/* }}} */

/* {{{ ratchet_pause() */
static int ratchet_pause (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	if (lua_pushthread (L))
		return luaL_error (L, "pause cannot be called from main thread");
	lua_pop (L, 1);

	event_base_loopbreak (e_b);	/* So that new threads get started. */

	return lua_yield (L, 0);
}
/* }}} */

/* {{{ ratchet_unpause() */
static int ratchet_unpause (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	get_thread (L, 2, L1);

	/* Make sure it's unpause-able. */
	if (lua_status (L1) != LUA_YIELD)
		return luaL_error (L, "thread is not yielding, cannot unpause");

	/* Set up the extra arguments as return values from pause(). */
	int nargs = lua_gettop (L) - 2;
	lua_xmove (L, L1, nargs);

	/* The following adds the thread to the ready table so it gets resumed. */
	set_thread_ready (L, 2 /* index of thread */);
	event_base_loopbreak (e_b);	/* So that new threads get started. */

	return 0;
}
/* }}} */

/* {{{ ratchet_loop() */
static int ratchet_loop (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);

	while (1)
	{
		/* Execute self:start_threads_ready(). */
		lua_getfield (L, 1, "start_threads_ready");
		lua_pushvalue (L, 1);
		lua_call (L, 1, 0);

		/* Execute self:start_threads_waiting(). */
		lua_getfield (L, 1, "start_threads_done_waiting");
		lua_pushvalue (L, 1);
		lua_call (L, 1, 0);

		/* Break loop if we're out of foreground threads. */
		lua_getfenv (L, 1);
		lua_getfield (L, -1, "foreground");
		lua_pushnil (L);
		if (lua_next (L, -2) == 0)
		{
			lua_pop (L, 2);
			break;
		}
		else lua_pop (L, 3);

		/* Call event loop, break if we're out of events. */
		int ret = event_base_loop (e_b, 0);
		if (ret < 0)
			return luaL_error (L, "libevent internal error");
		else if (ret > 0)
			break;
	}

	/* Clear all background threads. */
	lua_getfenv (L, 1);
	lua_newtable (L);
	lua_setfield (L, -2, "background");
	lua_pop (L, 1);

	return 0;
}
/* }}} */

/* {{{ ratchet_start_threads_ready() */
static int ratchet_start_threads_ready (lua_State *L)
{
	get_event_base (L, 1);
	lua_settop (L, 1);
	
	lua_getfenv (L, 1);
	lua_getfield (L, 2, "ready");
	lua_pushnil (L);
	while (1)
	{
		if (0 == lua_next (L, 3))
			break;

		/* Remove entry from ready. */
		lua_pushvalue (L, 4);
		lua_pushnil (L);
		lua_settable (L, 3);

		/* Call self:run_thread(t). */
		lua_getfield (L, 1, "run_thread");
		lua_pushvalue (L, 1);
		lua_pushvalue (L, 4);	/* The "key" is the thread to start. */
		lua_call (L, 2, 0);

		lua_pop (L, 2);
		lua_pushnil (L);
	}

	return 0;
}
/* }}} */

/* {{{ ratchet_start_threads_done_waiting() */
static int ratchet_start_threads_done_waiting (lua_State *L)
{
	get_event_base (L, 1);
	lua_settop (L, 1);

	lua_getfenv (L, 1);
	lua_getfield (L, 2, "waiting_on");
	for (lua_pushnil (L); lua_next (L, 3); lua_pop (L, 1))
	{
		/* Check for empty value table. */
		lua_pushnil (L);
		if (lua_next (L, 5) == 0)
		{
			/* Remove entry from waiting_on. */
			lua_pushvalue (L, 3);
			lua_pushvalue (L, 4);
			lua_pushnil (L);
			lua_settable (L, -3);
			lua_pop (L, 1);

			/* Call self:run_thread(t). */
			lua_getfield (L, 1, "run_thread");
			lua_pushvalue (L, 1);
			lua_pushvalue (L, 4);	/* The "key" is the thread to start. */
			lua_call (L, 2, 0);
		}
		else
			lua_pop (L, 2);

	}

	return 0;
}
/* }}} */

/* {{{ ratchet_run_thread() */
static int ratchet_run_thread (lua_State *L)
{
	struct event_base *e_b = get_event_base (L, 1);
	get_thread (L, 2, L1);

	int nargs = lua_gettop (L1);
	if (lua_status (L1) != LUA_YIELD)
		nargs--;
	int ret = lua_resume (L1, nargs);

	if (ret == 0)
	{
		/* Remove the entry from the persistance tables. */
		end_thread_persist (L, 2);

		/* So that waiting threads get started. */
		event_base_loopbreak (e_b);
	}

	else if (ret == LUA_YIELD)
	{
		/* Call self:yield_thread(). */
		lua_getfield (L, 1, "yield_thread");
		lua_pushvalue (L, 1);
		lua_pushvalue (L, 2);
		lua_call (L, 2, 0);
	}

	/* Handle the error from the thread. */
	else
	{
		/* Call self:handle_thread_error(). */
		lua_getfield (L, 1, "handle_thread_error");
		lua_pushvalue (L, 1);
		lua_pushvalue (L, 2);
		lua_call (L, 2, 0);

		/* Remove the entry from the persistance table. */
		end_thread_persist (L, 2);

		/* So that waiting threads get started. */
		event_base_loopbreak (e_b);
	}

	return 0;
}
/* }}} */

/* {{{ ratchet_yield_thread() */
static int ratchet_yield_thread (lua_State *L)
{
	get_event_base (L, 1);
	get_thread (L, 2, L1);

	int nrets = lua_gettop (L1);

	if (lua_isstring (L1, 1))
	{
		/* Get a wait_for_xxxxx method corresponding to first yield arg. */
		const char *yieldtype = lua_tostring (L1, 1);
		if (0 == strcmp (yieldtype, "write"))
			lua_getfield (L, 1, "wait_for_write");
		else if (0 == strcmp (yieldtype, "read"))
			lua_getfield (L, 1, "wait_for_read");
		else if (0 == strcmp (yieldtype, "timeout"))
			lua_getfield (L, 1, "wait_for_timeout");
		else
			lua_pushnil (L);

		if (lua_isfunction (L, -1))
		{
			/* Call wait_for_xxxxx method with self, thread, arg1, arg2... */
			lua_pushvalue (L, 1);
			lua_pushvalue (L, 2);
			lua_xmove (L1, L, nrets-1);
			lua_settop (L1, 0);
			lua_call (L, nrets+1, 0);
		}
	}

	return 0;
}
/* }}} */

/* {{{ ratchet_handle_thread_error() */
static int ratchet_handle_thread_error (lua_State *L)
{
	get_event_base (L, 1);
	get_thread (L, 2, L1);

	/* Get the error handler call stack table. */
	lua_getfenv (L, 1);
	lua_getfield (L, -1, "error_handlers");
	lua_pushvalue (L, 2);
	lua_gettable (L, -2);
	lua_replace (L, 3);
	lua_settop (L, 3);

	if (lua_isnil (L, 3))
	{
		/* Propogate the error. */
		lua_error (L1);
	}
	else
	{
		/* Unpack the call stack and call it. */
		int i;
		for (i=0; !lua_isnil (L, -1); i++)
			lua_rawgeti (L, 3, i+1);
		lua_pop (L, 1);	/* Pop the nil from the end off. */
		lua_xmove (L1, L, 1);
		lua_call (L, i-1, 0);
	}

	return 0;
}
/* }}} */

/* {{{ ratchet_wait_for_write() */
static int ratchet_wait_for_write (lua_State *L)
{
	/* Gather args into usable data. */
	struct event_base *e_b = get_event_base (L, 1);
	get_thread (L, 2, L1);
	int fd = get_fd_from_object (L, 3);
	double timeout = get_timeout_from_object (L, 3);

	/* Build timeout data. */
	struct timeval tv;
	int use_tv = gettimeval (timeout, &tv);

	/* Set the main thread at index 1. */
	lua_settop (L1, 0);
	lua_pushthread (L);
	lua_xmove (L, L1, 1);

	/* Build event. */
	struct event *ev = (struct event *) lua_newuserdata (L1, sizeof (struct event));
	luaL_getmetatable (L1, "ratchet_event_internal_meta");
	lua_setmetatable (L1, -2);

	/* Queue up the event. */
	event_set (ev, fd, EV_WRITE, event_triggered, L1);
	event_base_set (e_b, ev);
	event_add (ev, (use_tv ? &tv : NULL));

	return 0;
}
/* }}} */

/* {{{ ratchet_wait_for_read() */
static int ratchet_wait_for_read (lua_State *L)
{
	/* Gather args into usable data. */
	struct event_base *e_b = get_event_base (L, 1);
	get_thread (L, 2, L1);
	int fd = get_fd_from_object (L, 3);
	double timeout = get_timeout_from_object (L, 3);

	/* Build timeout data. */
	struct timeval tv;
	int use_tv = gettimeval (timeout, &tv);

	/* Set the main thread at index 1. */
	lua_settop (L1, 0);
	lua_pushthread (L);
	lua_xmove (L, L1, 1);

	/* Build event. */
	struct event *ev = (struct event *) lua_newuserdata (L1, sizeof (struct event));
	luaL_getmetatable (L1, "ratchet_event_internal_meta");
	lua_setmetatable (L1, -2);

	/* Queue up the event. */
	event_set (ev, fd, EV_READ, event_triggered, L1);
	event_base_set (e_b, ev);
	event_add (ev, (use_tv ? &tv : NULL));

	return 0;
}
/* }}} */

/* {{{ ratchet_wait_for_timeout() */
static int ratchet_wait_for_timeout (lua_State *L)
{
	/* Gather args into usable data. */
	struct event_base *e_b = get_event_base (L, 1);
	get_thread (L, 2, L1);
	struct timeval tv;
	gettimeval_arg (L, 3, &tv);

	/* Set the main thread at index 1. */
	lua_settop (L1, 0);
	lua_pushthread (L);
	lua_xmove (L, L1, 1);

	/* Build event and queue it up. */
	struct event *ev = (struct event *) lua_newuserdata (L1, sizeof (struct event));
	timeout_set (ev, event_triggered, L1);
	event_base_set (e_b, ev);
	event_add (ev, &tv);

	return 0;
}
/* }}} */

/* ---- Public Functions ---------------------------------------------------- */

/* {{{ luaopen_ratchet() */
int luaopen_ratchet (lua_State *L)
{
	static const luaL_Reg funcs[] = {
		{"new", ratchet_new},
		{"stackdump", ratchet_stackdump},
		{NULL}
	};

	static const luaL_Reg meths[] = {
		/* Documented methods. */
		{"get_method", ratchet_get_method},
		{"set_error_handler", ratchet_set_error_handler},
		{"attach", ratchet_attach},
		{"attach_background", ratchet_attach_background},
		{"wait_all", ratchet_wait_all},
		{"running_thread", ratchet_running_thread},
		{"timer", ratchet_timer},
		{"pause", ratchet_pause},
		{"unpause", ratchet_unpause},
		{"loop", ratchet_loop},
		/* Undocumented, helper methods. */
		{"run_thread", ratchet_run_thread},
		{"yield_thread", ratchet_yield_thread},
		{"handle_thread_error", ratchet_handle_thread_error},
		{"wait_for_write", ratchet_wait_for_write},
		{"wait_for_read", ratchet_wait_for_read},
		{"wait_for_timeout", ratchet_wait_for_timeout},
		{"start_threads_ready", ratchet_start_threads_ready},
		{"start_threads_done_waiting", ratchet_start_threads_done_waiting},
		{NULL}
	};

	static const luaL_Reg metameths[] = {
		{"__gc", ratchet_gc},
		{NULL}
	};

	static const luaL_Reg eventmetameths[] = {
		{"__gc", ratchet_event_gc},
		{NULL}
	};

	luaL_newmetatable (L, "ratchet_event_internal_meta");
	luaI_openlib (L, NULL, eventmetameths, 0);
	lua_pop (L, 1);

	luaL_newmetatable (L, "ratchet_meta");
	lua_newtable (L);
	luaI_openlib (L, NULL, meths, 0);
	lua_setfield (L, -2, "__index");
	luaI_openlib (L, NULL, metameths, 0);
	lua_pop (L, 1);

	luaI_openlib (L, "ratchet", funcs, 0);

#if HAVE_SOCKET
	luaopen_ratchet_socket (L);
	lua_setfield (L, -2, "socket");
#endif
#if HAVE_ZMQ
	luaopen_ratchet_zmqsocket (L);
	lua_setfield (L, -2, "zmqsocket");
#endif
#if HAVE_UDNS
	luaopen_ratchet_dns (L);
	lua_setfield (L, -2, "dns");
#endif
#if HAVE_TIMERFD
	luaopen_ratchet_timerfd (L);
	lua_setfield (L, -2, "timerfd");
#endif

	return 1;
}
/* }}} */

/* {{{ ratchet_version() */
const char *ratchet_version (void)
{
	return PACKAGE_VERSION;
}
/* }}} */

// vim:foldmethod=marker:ai:ts=4:sw=4:
