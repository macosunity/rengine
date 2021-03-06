/*
*# [Lua](http://www.lua.org/home.html) is a popular scripting language for games. 
*# Through Rengine's Lua State you get access to a Lua interpreter, with most of 
*# Rengine's functionality exposed.
*#
*# Links:
*{
** The [Lua Homepage](http://www.lua.org/home.html)
** A great introduction to Lua is the book [Programming In Lua](http://www.lua.org/pil/contents.html)
*}
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#ifdef WIN32
#include <SDL.h>
#include <SDL_mixer.h>
#else
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#endif

#include "bmp.h"
#include "states.h"
#include "tileset.h"
#include "map.h"
#include "game.h"
#include "ini.h"
#include "utils.h"
#include "log.h"
#include "gamedb.h"

#include "resources.h"

#define MAX_TIMEOUTS 20

/*
These are the Lua scripts in the ../scripts/ directory.
The script files are converted to C code strings using the Bace utility.
These strings are then executed through the Lua interpreter to provide
a variety of built in Lua code.
*/
extern const char base_lua[];
extern size_t base_lua_len;

/* Don't tamper with this variable from your Lua scripts. */
#define STATE_DATA_VAR	"___state_data"

struct callback_function {
	int ref;
	struct callback_function *next;
};

struct lustate_data {
	
	struct game_state *state;
	
	struct bitmap *bmp;
	struct map *map;	

	struct _timeout_element {
		int fun;
		int time;
		Uint32 start;
	} timeout[MAX_TIMEOUTS];
	int n_timeout;
	
	struct callback_function *update_fcn, *last_fcn;	
	struct callback_function *atexit_fcn;
	
	int change_state;
	char *next_state;
};

/* LUA FUNCTIONS */

static struct lustate_data *get_state_data(lua_State *L) {
	struct lustate_data *sd;
	lua_getglobal(L, STATE_DATA_VAR);
	if(!lua_islightuserdata(L, -1)) {
		luaL_error(L, "Variable %s got tampered with.", STATE_DATA_VAR);
		return 0; /* satisfy the compiler */
	} else {
		sd = lua_touserdata(L, -1);
	}
	lua_pop(L, 1);
	return sd;
}

/*1 Global functions 
 *# These functions have global scope in the Lua script.
 */

/*@ log(message)
 *# Writes a message to Rengine's log file.
 */
static int l_log(lua_State *L) {
	const char * s = lua_tolstring(L, 1, NULL);
	if(s) {
		sublog("Lua", "%s", s);
	}
	return 0;
}

/*@ import(path)
 *# Loads a Lua script from the resource on the specified path.
 *# This function is needed because the standard Lua functions like
 *# require() and dofile() are disabled in the Rengine sandbox.
 */
static int l_import(lua_State *L) {	
	const char *path = lua_tolstring(L, 1, NULL);
	char * script = re_get_script(path);
	if(!script)
		luaL_error(L, "Could not import %s", path);
	rlog("Imported %s", path);
	if(luaL_dostring(L, script)) {
		sublog("lua", "%s: %s", path, lua_tostring(L, -1));
	}
	free(script);
	return 0;
}

/*@ setTimeout(func, millis)
 *# Waits for {{millis}} milliseconds, then calls {{func}}
 */
static int l_set_timeout(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);

	/* This link was useful: 
	http://stackoverflow.com/questions/2688040/how-to-callback-a-lua-function-from-a-c-function 
	*/
	if(lua_gettop(L) == 2 && lua_isfunction(L, -2) && lua_isnumber(L, -1)) {
		
		if(sd->n_timeout == MAX_TIMEOUTS) {
			luaL_error(L, "Maximum number of timeouts [%d] reached", MAX_TIMEOUTS);
		}
		
		/* Push the callback function on to the top of the stack */
		lua_pushvalue(L, -2);
		
		/* And create a reference to it in the special LUA_REGISTRYINDEX */
		sd->timeout[sd->n_timeout].fun = luaL_ref(L, LUA_REGISTRYINDEX);
		
		sd->timeout[sd->n_timeout].time = luaL_checkinteger(L, 2);		
		sd->timeout[sd->n_timeout].start = SDL_GetTicks();
		
		sd->n_timeout++;
		
	} else {
		luaL_error(L, "setTimeout() requires a function and a time as parameters");
	}
	
	return 0;
}

static void process_timeouts(lua_State *L) {
	struct lustate_data *sd;
	int i = 0;
	
	lua_getglobal(L, STATE_DATA_VAR);
	if(!lua_islightuserdata(L, -1)) {
		/* Can't call LuaL_error here. */
		rerror("Variable %s got tampered with.", STATE_DATA_VAR);
		return;
	} else {
		sd = lua_touserdata(L, -1);
	}
	lua_pop(L, 1);
	
	while(i < sd->n_timeout) {
		Uint32 elapsed = SDL_GetTicks() - sd->timeout[i].start;
		if(elapsed > sd->timeout[i].time) {			
			/* Retrieve the callback */
			lua_rawgeti(L, LUA_REGISTRYINDEX, sd->timeout[i].fun);
			
			/* Call it */
			if(lua_pcall(L, 0, 0, 0)) {
				rerror("Unable to execute setTimeout() callback: %s", lua_tostring(L, -1));				
			}
			/* Release the reference so that it can be collected */
			luaL_unref(L, LUA_REGISTRYINDEX, sd->timeout[i].fun);
			
			/* Now delete this timeout by replacing it with the last one */
			sd->timeout[i] = sd->timeout[--sd->n_timeout];
		} else {
			i++;
		}
	}
}

/*@ onUpdate(func)
 *# Registers the function {{func}} to be called every frame
 *# when Rengine draws the screen.
 */
static int l_onUpdate(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);

	if(lua_gettop(L) == 1 && lua_isfunction(L, -1)) {
		struct callback_function *fn;
				
		fn = malloc(sizeof *fn);
		if(!fn)
			luaL_error(L, "Out of memory");
		
		/* And create a reference to it in the special LUA_REGISTRYINDEX */
		fn->ref = luaL_ref(L, LUA_REGISTRYINDEX);		
		rlog("Registering onUpdate() callback %d", fn->ref);
		
		fn->next = NULL;
		if(!sd->last_fcn) {
			assert(sd->update_fcn == NULL);
			sd->last_fcn = fn;
			sd->update_fcn = fn;
		} else {
			sd->last_fcn->next = fn;
			sd->last_fcn = fn;
		}
		
		lua_pushinteger(L, fn->ref);
		
	} else {
		luaL_error(L, "onUpdate() requires a function as a parameter");
	}
	
	return 1;
}

/*@ atExit(func)
 *# Registers the function {{func}} to be called when the
 *# current state is exited.\n
 *# These functions should not do any drawing.
 */
static int l_atExit(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);

	if(lua_gettop(L) == 1 && lua_isfunction(L, -1)) {
		struct callback_function *fn;
				
		fn = malloc(sizeof *fn);
		if(!fn)
			luaL_error(L, "Out of memory");
		
		/* And create a reference to it in the special LUA_REGISTRYINDEX */
		fn->ref = luaL_ref(L, LUA_REGISTRYINDEX);		
		rlog("Registering atExit() callback %d", fn->ref);
		
		fn->next = sd->atexit_fcn;
		sd->atexit_fcn = fn;
		
		lua_pushinteger(L, fn->ref);		
	} else {
		luaL_error(L, "atExit() requires a function as a parameter");
	}
	
	return 1;
}

struct callback_function *atexit_fcn, *last_atexit_fcn;

/*@ advanceFrame()
 *# Low-level function to advance the animation frame on the screen.\n
 *# It flips the back buffer, retrieves user inputs and processes system 
 *# events.\n
 *# In general you should not call this function directly, since Rengine
 *# does the above automatically. This function is provided for the special
 *# case where you need a Lua script to run in a loop, and during each 
 *# iteration update the screen and get new user input.\n
 *# An example of such a case is drwaing a GUI completely in a Lua script.\n
 *# It does not clear the screen - you'll need to do that yourself.
 *# Timeouts set through {{setTimeout()}} will also be processed.
 */
static int l_advanceFrame(lua_State *L) {	
	process_timeouts(L);
	advanceFrame();
	return 0;
}

/*1 Game object
 *# Functions in the {{Game}} scope
 */

/*@ Game.changeState(newstate)
 *# Changes the game's [[state|State Machine]] to the state identified by {{newstate}}
 */
static int l_changeState(lua_State *L) {
	const char *next_state = luaL_checkstring(L, -1);
	struct lustate_data *sd = get_state_data(L);
	
	sd->next_state = strdup(next_state);
	sd->change_state = 1;
	
	return 0;
}

/*@ Game.getStyle(style)
 *# Retrieves a specific [[Style]] from the [[game.ini]] file.
 */
static int l_getstyle(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	const char * s = luaL_checkstring(L,1);
	lua_pushstring(L, get_style(sd->state, s));
	return 1;
}

static const luaL_Reg game_funcs[] = {
  {"changeState",     l_changeState},
  {"getStyle",        l_getstyle},
  {0, 0}
};

/*1 BmpObj
 *# The Bitmap object encapsulates a bitmap in the engine.
 *# Instances of BmpObj are drawn to the screen with the [[G.blit()|Lua-state#gblitbmp-dx-dy-sx-sy-w-h]] function
 */

/*@ Bmp(filename)
 *# Loads the bitmap file specified by {{filename}} from the
 *# [[Resources|Resource Management]] and returns it
 *# encapsulated within a `BmpObj` instance.
 */
static int new_bmp_obj(lua_State *L) {
	const char *filename = luaL_checkstring(L,1);
	
	struct bitmap **bp = lua_newuserdata(L, sizeof *bp);	
	luaL_setmetatable(L, "BmpObj");
	
	*bp = re_get_bmp(filename);
	if(!*bp) {
		luaL_error(L, "Unable to load bitmap '%s'", filename);
	}
	return 1;
}

/*@ BmpObj:__tostring()
 *# Returns a string representation of the `BmpObj` instance.
 */
static int bmp_tostring(lua_State *L) {	
	struct bitmap **bp = luaL_checkudata(L,1, "BmpObj");
	struct bitmap *b = *bp;
	lua_pushfstring(L, "BmpObj[%dx%d]", b->w, b->h);
	return 1;
}

/*@ BmpObj:__gc()
 *# Garbage collects the `BmpObj` instance.
 */
static int gc_bmp_obj(lua_State *L) {
	/* No need to free the bitmap: It's in the resource cache. */
	return 0;
}

/*@ BmpObj:setMask(color)
 *# Sets the color used as a mask when the bitmap is drawn to the screen.
 */
static int bmp_set_mask(lua_State *L) {	
	struct bitmap **bp = luaL_checkudata(L,1, "BmpObj");
	const char *mask = luaL_checkstring(L, 2);
	bm_set_color_s(*bp, mask);
	return 0;
}

/*@ BmpObj:width()
 *# Returns the width of the bitmap
 */
static int bmp_width(lua_State *L) {	
	struct bitmap **bp = luaL_checkudata(L,1, "BmpObj");
	lua_pushinteger(L, (*bp)->w);
	return 1;
}

/*@ BmpObj:height()
 *# Returns the height of the bitmap
 */
static int bmp_height(lua_State *L) {	
	struct bitmap **bp = luaL_checkudata(L,1, "BmpObj");
	lua_pushinteger(L, (*bp)->h);
	return 1;
}

static void bmp_obj_meta(lua_State *L) {
	/* Create the metatable for MyObj */
	luaL_newmetatable(L, "BmpObj");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index"); /* BmpObj.__index = BmpObj */
	
	/* Add methods */
	lua_pushcfunction(L, bmp_set_mask);
	lua_setfield(L, -2, "setMask");
	lua_pushcfunction(L, bmp_width);
	lua_setfield(L, -2, "width");
	lua_pushcfunction(L, bmp_height);
	lua_setfield(L, -2, "height");
	
	lua_pushcfunction(L, bmp_tostring);
	lua_setfield(L, -2, "__tostring");	
	lua_pushcfunction(L, gc_bmp_obj);
	lua_setfield(L, -2, "__gc");	
	
	/* The global method Bmp() */
	lua_pushcfunction(L, new_bmp_obj);
	lua_setglobal(L, "Bmp");
}

/*1 Map
 *# The Map object provides access to the Map through
 *# a variety of functions.
 *#
 *# These fields are also available:
 *{
 ** {{Map.BACKGROUND}} - Constant for the Map's background layer. See {{Map.render()}}
 ** {{Map.CENTER}} - Constant for the Map's center layer. See {{Map.render()}}
 ** {{Map.FOREGROUND}} - Constant for the Map's foreground layer. See {{Map.render()}}
 ** {{Map.ROWS}} - The number of rows in the map.
 ** {{Map.COLS}} - The number of columns in the map.
 ** {{Map.TILE_WIDTH}} - The width (in pixels) of the cells on the map.
 ** {{Map.TILE_HEIGHT}} - The height (in pixels) of the cells on the map.
 *}
 *# The {{Map}} object is only available if the {{map}}
 *# parameter has been set in the state's configuration
 *# in the {{game.ini}} file.
 */

/*@ Map.render(layer, [scroll_x, scroll_y])
 *# Renders the specified layer of the map (background, center or foreground).
 *# The center layer is where all the action in the game occurs and sprites and so on moves around.
 *# The background is drawn behind the center layer for objects in the background. It needs to be drawn first,
 *# so that the center and foreground layers are drawn over it.
 *# The foreground layer is drawn last and contains objects in the foreground. 
 */
static int render_map(lua_State *L) {
	int layer = luaL_checkint(L,1) - 1;
	int sx = 0, sy = 0;
	struct lustate_data *sd = get_state_data(L);
	
	if(!sd->map) {
		luaL_error(L, "Attempt to render non-existent Map");
	} 
	if(layer < 0 || layer >= 3) {
		luaL_error(L, "Attempt to render non-existent Map layer %d", layer);
	}
	if(!sd->bmp) {
		luaL_error(L, "Attempt to render Map outside of a screen update");	
	}
	
	if(lua_gettop(L) > 2) {
		sx = luaL_checkint(L,1);
		sy = luaL_checkint(L,2);
	}
	
	map_render(sd->map, sd->bmp, layer, sx, sy);
	return 0;
}

/*@ Map.cell(r,c)
 *# Returns a cell on the map at row {{r}}, column {{c}} as 
 *# a {{CellObj}} instance.\n
 *# {{r}} must be between {{1}} and {{Map.ROWS}} inclusive.
 *# {{c}} must be between {{1}} and {{Map.COLS}} inclusive.
 */
static int get_cell_obj(lua_State *L) {
	
	int r = luaL_checkint(L,1) - 1;
	int c = luaL_checkint(L,2) - 1;	
	struct lustate_data *sd = get_state_data(L);	
	struct map_cell **o;
	
	assert(sd->map);
	
	if(c < 0 || c >= sd->map->nc) 
		luaL_error(L, "Invalid r value in Map.cell()");
	if(r < 0 || r >= sd->map->nr) 
		luaL_error(L, "Invalid c value in Map.cell()");
	
	o = lua_newuserdata(L, sizeof *o);	
	luaL_setmetatable(L, "CellObj");
		
	*o = map_get_cell(sd->map, c, r);
	
	return 1;
}

static const luaL_Reg map_funcs[] = {
  {"render",      	render_map},
  {"cell",      	get_cell_obj},
  {0, 0}
};

/*1 CellObj
 *# The CellObj encapsulates an individual cell on the 
 *# map. You then use the methods of the CellObj to manipulate
 *# the cell.
 *#
 *# Cell Objects are obtained through the {{Map.cell(r,c)}} function.
 */

/*@ CellObj:__tostring()
 *# Returns a string representation of the CellObj
 */
static int cell_tostring(lua_State *L) {	
	luaL_checkudata(L,1, "CellObj");
	lua_pushstring(L, "CellObj");
	return 1;
}

/*@ CellObj:__gc()
 *# Frees the CellObj when it is garbage collected.
 */
static int gc_cell_obj(lua_State *L) {
	/* Nothing to do, since we don't actually own the pointer to the map_cell */
	return 0;
}

/*@ CellObj:set(layer, si, ti)
 *# Sets the specific layer of the cells encapsulated in the CellObj to 
 *# the {{si,ti}} value, where
 *{
 ** {{si}} is the Set Index
 ** {{ti}} is the Tile Index
 *}
 */
static int cell_set(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	struct map_cell **cp = luaL_checkudata(L,1, "CellObj");
	struct map_cell *c = *cp;
	
	int l = luaL_checkint(L,2) - 1;
	int si = luaL_checkint(L,3);
	int ti = luaL_checkint(L,4);
	
	if(l < 0 || l > 2) {
		luaL_error(L, "Invalid level passed to CellObj:set()");
	}
	
	assert(sd->map); /* cant create a CellObj if this is false. */
	if(si < 0 || si >= ts_get_num(&sd->map->tiles)) {
		luaL_error(L, "Invalid si passed to CellObj:set()");
	}
	/* FIXME: error checking on ti? */
	
	/* TODO: Maybe you ought to store this change in some sort of list
	   so that the savedgames can handle changes to the map like this. */
	c->tiles[l].ti = ti;
	c->tiles[l].si = si;
	
	/* Push the CellObj back onto the stack so that other methods can be called on it */
	lua_pushvalue(L, -4);
	
	return 1;
}

/*@ CellObj:getId()
 *# Returns the {/id/} of a cell as defined in the Rengine Editor
 */
static int cell_get_id(lua_State *L) {
	struct map_cell **cp = luaL_checkudata(L,1, "CellObj");
	struct map_cell *c = *cp;	
	lua_pushstring(L, c->id ? c->id : "");
	return 1;
}

/*@ CellObj:getClass()
 *# Returns the {/class/} of a cell as defined in the Rengine Editor
 */
static int cell_get_class(lua_State *L) {
	struct map_cell **cp = luaL_checkudata(L,1, "CellObj");
	struct map_cell *c = *cp;	
	lua_pushstring(L, c->clas ? c->clas : "");
	return 1;
}

/*@ CellObj:isBarrier()
 *# Returns whether the cell is a barrier
 */
static int cell_is_barrier(lua_State *L) {
	struct map_cell **cp = luaL_checkudata(L,1, "CellObj");
	struct map_cell *c = *cp;	
	lua_pushboolean(L, c->flags & TS_FLAG_BARRIER);
	return 1;
}

/*@ CellObj:setBarrier(b)
 *# Sets whether the cell is a barrier
 */
static int cell_set_barrier(lua_State *L) {
	struct map_cell **cp = luaL_checkudata(L,1, "CellObj");
	struct map_cell *c = *cp;	
	luaL_checktype(L, 2, LUA_TBOOLEAN);
	int b = lua_toboolean(L, 2);
	if(b)
		c->flags |= TS_FLAG_BARRIER;
	else
		c->flags &= ~TS_FLAG_BARRIER;
	return 0;
}

/*
	I may have been doing this wrong. See http://lua-users.org/wiki/UserDataWithPointerExample
*/
static void cell_obj_meta(lua_State *L) {
	/* Create the metatable for MyObj */
	luaL_newmetatable(L, "CellObj");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index"); /* CellObj.__index = CellObj */
	
	/* Add methods here */
	lua_pushcfunction(L, cell_set);
	lua_setfield(L, -2, "set");
	lua_pushcfunction(L, cell_get_id);
	lua_setfield(L, -2, "getId");
	lua_pushcfunction(L, cell_get_class);
	lua_setfield(L, -2, "getClass");
	lua_pushcfunction(L, cell_is_barrier);
	lua_setfield(L, -2, "isBarrier");
	lua_pushcfunction(L, cell_set_barrier);
	lua_setfield(L, -2, "setBarrier");
	
	lua_pushcfunction(L, cell_tostring);
	lua_setfield(L, -2, "__tostring");	
	lua_pushcfunction(L, gc_cell_obj);
	lua_setfield(L, -2, "__gc");	
	
	/* The global method C() */
	lua_pushcfunction(L, get_cell_obj);
	lua_setglobal(L, "Cell");
}

/*1 G
 *# {{G}} is the Graphics object that allows you to draw primitives on the screen. \n
 *# You can only call these functions when the screen is being drawn. That's to say, you can only call then inside 
 *# functions registered through {{onUpdate()}}
 *# If you call them elsewhere, you will get the error {/"Call to graphics function outside of a screen update"/}
 *# 
 *# These fields are also available:
 *{
 ** {{G.FPS}} - The configured frames per second of the game (see [[game.ini]]).
 ** {{G.SCREEN_WIDTH}} - The configured width of the screen.
 ** {{G.SCREEN_HEIGHT}} - The configured height of the screen.
 *}
 */

/* 
The "Call to graphics function outside of a screen update" error happens because
sd->bmp is not yet set when the script is first executed in the state's initialisation
function (lus_init()).
*/

/*@ G.setColor(color)
 *# Sets the [[color|Colors]] used to draw the graphics primitives.
 */
static int gr_setcolor(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");	
	if(lua_gettop(L) > 0) {
		const char *c = luaL_checkstring(L,1);
		bm_set_color_s(sd->bmp, c);
	} else {
		bm_set_color_s(sd->bmp, get_style(sd->state, "forEGrouND"));
	}
	return 0;
}

/*@ G.clip(x0,y0, x1,y1)
 *# Sets the clipping rectangle when drawing primitives.
 */
static int gr_clip(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");	
	int x0 = luaL_checkinteger(L,1);
	int y0 = luaL_checkinteger(L,2);
	int x1 = luaL_checkinteger(L,3);
	int y1 = luaL_checkinteger(L,4);
	bm_clip(sd->bmp, x0, y0, x1, y1);
	return 0;
}

/*@ G.unclip()
 *# Resets the clipping rectangle when drawing primitives.
 */
static int gr_unclip(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	bm_unclip(sd->bmp);
	return 0;
}
/*@ G.pixel(x,y)
 *# Plots a pixel at {{x,y}} on the screen.
 */
static int gr_putpixel(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x = luaL_checkint(L,1);
	int y = luaL_checkint(L,2);
	bm_putpixel(sd->bmp, x, y);
	return 0;
}

/*@ G.line(x0, y0, x1, y1)
 *# Draws a line from {{x0,y0}} to {{x1,y1}}
 */
static int gr_line(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	
	int x0 = luaL_checkint(L,1);
	int y0 = luaL_checkint(L,2);
	int x1 = luaL_checkint(L,3);
	int y1 = luaL_checkint(L,4);
	bm_line(sd->bmp, x0, y0, x1, y1);
	return 0;
}

/*@ G.rect(x0, y0, x1, y1)
 *# Draws a rectangle from {{x0,y0}} to {{x1,y1}}
 */
static int gr_rect(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x0 = luaL_checkint(L,1);
	int y0 = luaL_checkint(L,2);
	int x1 = luaL_checkint(L,3);
	int y1 = luaL_checkint(L,4);
	bm_rect(sd->bmp, x0, y0, x1, y1);
	return 0;
}

/*@ G.fillRect(x0, y0, x1, y1)
 *# Draws a filled rectangle from {{x0,y0}} to {{x1,y1}}
 */
static int gr_fillrect(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x0 = luaL_checkint(L,1);
	int y0 = luaL_checkint(L,2);
	int x1 = luaL_checkint(L,3);
	int y1 = luaL_checkint(L,4);
	bm_fillrect(sd->bmp, x0, y0, x1, y1);
	return 0;
}

/*@ G.circle(x, y, r)
 *# Draws a circle centered at {{x,y}} with radius {{r}}
 */
static int gr_circle(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x = luaL_checkint(L,1);
	int y = luaL_checkint(L,2);
	int r = luaL_checkint(L,3);
	bm_circle(sd->bmp, x, y, r);
	return 0;
}

/*@ G.fillCircle(x, y, r)
 *# Draws a filled circle centered at {{x,y}} with radius {{r}}
 */
static int gr_fillcircle(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x = luaL_checkint(L,1);
	int y = luaL_checkint(L,2);
	int r = luaL_checkint(L,3);
	bm_fillcircle(sd->bmp, x, y, r);
	return 0;
}

/*@ G.ellipse(x0, y0, x1, y1)
 *# Draws an ellipse from {{x0,y0}} to {{x1,y1}}
 */
static int gr_ellipse(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x0 = luaL_checkint(L,1);
	int y0 = luaL_checkint(L,2);
	int x1 = luaL_checkint(L,3);
	int y1 = luaL_checkint(L,4);
	bm_ellipse(sd->bmp, x0, y0, x1, y1);
	return 0;
}

/*@ G.roundRect(x0, y0, x1, y1, r)
 *# Draws a rectangle from {{x0,y0}} to {{x1,y1}}
 *# with rounded corners of radius {{r}}
 */
static int gr_roundrect(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x0 = luaL_checkint(L,1);
	int y0 = luaL_checkint(L,2);
	int x1 = luaL_checkint(L,3);
	int y1 = luaL_checkint(L,4);
	int r = luaL_checkint(L,5);
	bm_roundrect(sd->bmp, x0, y0, x1, y1, r);
	return 0;
}

/*@ G.fillRoundRect(x0, y0, x1, y1, r)
 *# Draws a rectangle from {{x0,y0}} to {{x1,y1}}
 *# with rounded corners of radius {{r}}
 */
static int gr_fillroundrect(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x0 = luaL_checkint(L,1);
	int y0 = luaL_checkint(L,2);
	int x1 = luaL_checkint(L,3);
	int y1 = luaL_checkint(L,4);
	int r = luaL_checkint(L,5);
	bm_fillroundrect(sd->bmp, x0, y0, x1, y1, r);
	return 0;
}

/*@ G.curve(x0, y0, x1, y1, x2, y2)
 *# Draws a Bezier curve from {{x0,y0}} to {{x2,y2}}
 *# with {{x1,y1}} as the control point.
 *# Note that it doesn't pass through {{x1,y1}}
 */
static int gr_bezier3(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x0 = luaL_checkint(L,1);
	int y0 = luaL_checkint(L,2);
	int x1 = luaL_checkint(L,3);
	int y1 = luaL_checkint(L,4);
	int x2 = luaL_checkint(L,5);
	int y2 = luaL_checkint(L,6);
	bm_bezier3(sd->bmp, x0, y0, x1, y1, x2, y2);
	return 0;
}

/*@ G.lerp(col1, col2, frac)
 *# Returns a [[color|Colors]] that is some fraction {{frac}} along 
 *# the line from {{col1}} to {{col2}}.
 *# For example, `G.lerp("red", "blue", 0.33)` will return a color
 *# that is 1/3rd of the way from red to blue.
 */
static int gr_lerp(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	const char *c1 = luaL_checkstring(L,1);
	const char *c2 = luaL_checkstring(L,2);
	lua_Number v = luaL_checknumber(L,3);	
	int col = bm_lerp(bm_color_atoi(c1), bm_color_atoi(c2), v);
	bm_set_color_i(sd->bmp, col);
	
	lua_pushinteger(L, col);
	return 1;
}

/*@ G.setFont(font)
 *# Sets the [[font|Fonts]] used for the {{G.print()}} function. 
 */
static int gr_setfont(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	enum bm_fonts font;	
	if(lua_gettop(L) > 0) {
		const char *name = luaL_checkstring(L,1);
		font = bm_font_index(name);
	} else {
		font = bm_font_index(get_style(sd->state, "font"));
	}
	bm_std_font(sd->bmp, font);	
	return 0;
}

/*@ G.print(x,y,text)
 *# Prints the {{text}} to the screen, with its top left position at {{x,y}}.
 */
static int gr_print(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	const char *s = luaL_checkstring(L, 3);
	
	bm_puts(sd->bmp, x, y, s);
	
	return 0;
}

/*@ G.textDims(text)
 *# Returns the width,height in pixels that the {{text}}
 *# will occupy on the screen.
 *X local w,h = G.textDims(message);
 */
static int gr_textdims(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	const char *s = luaL_checkstring(L, 1);
	
	lua_pushinteger(L, bm_text_width(sd->bmp, s));
	lua_pushinteger(L, bm_text_height(sd->bmp, s));
	
	return 2;
}

/*@ G.blit(bmp, dx, dy, [sx], [sy], [w], [h])
 *# Draws an instance {{bmp}} of {{BmpObj}} to the screen at {{dx, dy}}.
 *# {{sx,sy}} specify the source x,y position and {{w,h}} specifies the
 *# width and height of the source to draw.
 *# {{sx,sy}} defaults to {{0,0}} and {{w,h}} defaults to the entire 
 *# source bitmap.
 */
static int gr_blit(lua_State *L) {
	struct lustate_data *sd = get_state_data(L);
	if(!sd->bmp)
		luaL_error(L, "Call to graphics function outside of a screen update");
	struct bitmap **bp = luaL_checkudata(L, 1, "BmpObj");
	
	int dx = luaL_checkinteger(L, 2);
	int dy = luaL_checkinteger(L, 3);
	
	int sx = 0, sy = 0, w = (*bp)->w, h = (*bp)->h;
	
	if(lua_gettop(L) >= 4)
		sx = luaL_checkinteger(L, 4);
	if(lua_gettop(L) >= 5)
		sy = luaL_checkinteger(L, 5);
	if(lua_gettop(L) >= 6)
		w = luaL_checkinteger(L, 6);
	if(lua_gettop(L) >= 7)
		h = luaL_checkinteger(L, 7);
	
	bm_maskedblit(sd->bmp, dx, dy, *bp, sx, sy, w, h);
	
	return 0;
}

static const luaL_Reg graphics_funcs[] = {
  {"setColor",      gr_setcolor},
  {"clip", 			gr_clip},
  {"unclip", 		gr_unclip},
  {"pixel",         gr_putpixel},
  {"line",          gr_line},
  {"rect",          gr_rect},
  {"fillRect",      gr_fillrect},
  {"circle",        gr_circle},
  {"fillCircle",    gr_fillcircle},
  {"ellipse",    	gr_ellipse},
  {"roundRect",    	gr_roundrect},
  {"fillRoundRect", gr_fillroundrect},
  {"curve",         gr_bezier3},
  {"lerp",          gr_lerp},
  {"print",         gr_print},
  {"setFont",       gr_setfont},
  {"textDims",      gr_textdims},
  {"blit",          gr_blit},
  {0, 0}
};

/*1 Keyboard
 *# Keyboard Input Functions */

/*@ Keyboard.down([key])
 *# Checks whether a key is down on the keyboard.
 *# The parameter {{key}} is the name of specific key.
 *# See http://wiki.libsdl.org/SDL_Scancode for the names of all the possible keys.
 *# If {{key}} is omitted, the function returns true if _any_ key is down.
 */
static int in_kbhit(lua_State *L) {	
	if(lua_gettop(L) > 0) {
		const char *name = luaL_checkstring(L, 1);
		/* Names of the keys are listed on
			http://wiki.libsdl.org/SDL_Scancode
		*/
		lua_pushboolean(L, keys[SDL_GetScancodeFromName(name)]);
	} else {
		lua_pushboolean(L, kb_hit());
	}	
	return 1;
}

/*@ Keyboard.reset()
 *# Resets the keyboard input.
 */
static int in_reset_keys(lua_State *L) {	
	reset_keys();
	return 0;
}

static const luaL_Reg keyboard_funcs[] = {
  {"down",   in_kbhit},
  {"reset",  in_reset_keys},
  {0, 0}
};

/*1 Mouse
 *# Mouse input functions.
 *#
 *# These constants are used with {{Mouse.down()}} and {{Mouse.click()}}
 *# to identify specific mouse buttons:
 *{
 ** {{Mouse.LEFT}}
 ** {{Mouse.MIDDLE}}
 ** {{Mouse.RIGHT}}
 *}
 */

/*@ Mouse.position()
 *# Returns the {{x,y}} position of the mouse.
 *X local x,y = Mouse.position();
 */
static int in_mousepos(lua_State *L) {	
	lua_pushinteger(L, mouse_x);
	lua_pushinteger(L, mouse_y);
	return 2;
}

/*@ Mouse.down(btn)
 *# Returns true if the button {{btn}} is down.
 */
static int in_mousedown(lua_State *L) {	
	int btn = luaL_checkinteger(L, 1);
	lua_pushboolean(L, mouse_btns & SDL_BUTTON(btn));
	return 1;
}

/*@ Mouse.click(btn)
 *# Returns true if the button {{btn}} was clicked.
 *# A button is considered clicked if it was down the
 *# previous frame and is not anymore this frame.
 */
static int in_mouseclick(lua_State *L) {	
	int btn = luaL_checkinteger(L, 1);
	lua_pushboolean(L, mouse_clck & SDL_BUTTON(btn));
	return 1;
}

static const luaL_Reg mouse_funcs[] = {
  {"position",  in_mousepos},
  {"down",  in_mousedown},
  {"click",  in_mouseclick},
  {0, 0}
};

/*1 SndObj
 *# The sound object that encapsulates a sound (WAV) file in the engine.
 */

/*@ Wav(filename)
 *# Loads the WAV file specified by {{filename}} from the
 *# [[Resources|Resource Management]] and returns it
 *# encapsulated within a `SndObj` instance.
 */
static int new_wav_obj(lua_State *L) {
	const char *filename = luaL_checkstring(L,1);
	
	struct Mix_Chunk **cp = lua_newuserdata(L, sizeof *cp);	
	luaL_setmetatable(L, "SndObj");
	
	*cp = re_get_wav(filename);
	if(!*cp) {
		luaL_error(L, "Unable to load WAV file '%s'", filename);
	}
	return 1;
}

/*@ SndObj:__tostring()
 *# Returns a string representation of the `SndObj` instance.
 */
static int wav_tostring(lua_State *L) {	
	struct Mix_Chunk **cp = luaL_checkudata(L,1, "SndObj");
	struct Mix_Chunk *c = *cp;
	lua_pushfstring(L, "SndObj[%p]", c->abuf);
	return 1;
}

/*@ SndObj:__gc()
 *# Garbage collects the `SndObj` instance.
 */
static int gc_wav_obj(lua_State *L) {
	/* No need to free the Mix_Chunk: It's in the resource cache. */
	return 0;
}

static void wav_obj_meta(lua_State *L) {
	/* Create the metatable for MyObj */
	luaL_newmetatable(L, "SndObj");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index"); /* SndObj.__index = SndObj */
			
	lua_pushcfunction(L, wav_tostring);
	lua_setfield(L, -2, "__tostring");	
	lua_pushcfunction(L, gc_wav_obj);
	lua_setfield(L, -2, "__gc");	
	
	/* The global method Wav() */
	lua_pushcfunction(L, new_wav_obj);
	lua_setglobal(L, "Wav");
}

/*1 Sound
 *# The {{Sound}} object exposes functions through which 
 *# sounds can be played an paused in your games.
 */

/*@ Sound.play(sound)
 *# Plays a sound.
 *# {{sound}} is a {{SndObj}} object previously loaded through
 *# the {{Wav(filename)}} function.
 *# It returns the {{channel}} the sound is playing on, which can be used
 *# with other {{Sound}} functions.
 */
static int sound_play(lua_State *L) {
	struct Mix_Chunk **cp = luaL_checkudata(L,1, "SndObj");
	struct Mix_Chunk *c = *cp;
	int ch = Mix_PlayChannel(-1, c, 0);
	if(ch < 0) {
		luaL_error(L, "Sound.play(): %s", Mix_GetError());
	}
	lua_pushinteger(L, ch);
	return 1;
}

/*@ Sound.loop(sound, [n])
 *# Loops a sound {{n}} times. 
 *# If {{n}} is omitted, the sound will loop indefinitely.
 *# {{sound}} is a {{SndObj}} object previously loaded through
 *# the {{Wav(filename)}} function.
 *# It returns the {{channel}} the sound is playing on, which can be used
 *# with other {{Sound}} functions.
 */
static int sound_loop(lua_State *L) {
	struct Mix_Chunk **cp = luaL_checkudata(L,1, "SndObj");
	struct Mix_Chunk *c = *cp;
	
	int n = -1;
	if(lua_gettop(L) > 0) {
		n = luaL_checkinteger(L, 2);
	}
	if(n < 1) n = 1;
	
	int ch = Mix_PlayChannel(-1, c, n - 1);
	if(ch < 0) {
		luaL_error(L, "Sound.loop(): %s", Mix_GetError());
	}
	lua_pushinteger(L, ch);
	return 1;
}

/*@ Sound.pause([channel])
 *# Pauses a sound.
 *# {{channel}} is the channel previously returned by
 *# {{Sound.play()}}. If it's omitted, all sound will be paused.
 */
static int sound_pause(lua_State *L) {
	int channel = -1;
	if(lua_gettop(L) > 0) {
		channel = luaL_checkinteger(L, 1);
	}
	Mix_Pause(channel);
	return 0;
}

/*@ Sound.resume([channel])
 *# Resumes a paused sound on a specific channel.
 *# {{channel}} is the channel previously returned by
 *# {{Sound.play()}}. If it's omitted, all sound will be resumed.
 */
static int sound_resume(lua_State *L) {
	int channel = -1;
	if(lua_gettop(L) > 0) {
		channel = luaL_checkinteger(L, 1);
	}
	Mix_Resume(channel);
	return 0;
}

/*@ Sound.halt([channel])
 *# Halts a sound playing on a specific channel.
 *# {{channel}} is the channel previously returned by
 *# {{Sound.play()}}. If it's omitted, all sound will be halted.
 */
static int sound_halt(lua_State *L) {
	int channel = -1;
	if(lua_gettop(L) > 0) {
		channel = luaL_checkinteger(L, 1);
	}
	if(Mix_HaltChannel(channel) < 0) {
		luaL_error(L, "Sound.pause(): %s", Mix_GetError());
	}
	return 0;
}

/*@ Sound.playing([channel])
 *# Returns whether the sound on {{channel}} is currently playing.
 *# If {{channel}} is omitted, it will return an integer containing the
 *# number of channels currently playing.
 */
static int sound_playing(lua_State *L) {
	if(lua_gettop(L) > 0) {
		int channel = luaL_checkinteger(L, 1);
		lua_pushboolean(L, Mix_Playing(channel));
	}
	lua_pushinteger(L, Mix_Playing(-1));
	return 1;
}

/*@ Sound.paused([channel])
 *# Returns whether the sound on {{channel}} is currently paused.
 *# If {{channel}} is omitted, it will return an integer containing the
 *# number of channels currently paused.
 */
static int sound_paused(lua_State *L) {
	if(lua_gettop(L) > 0) {
		int channel = luaL_checkinteger(L, 1);
		lua_pushboolean(L, Mix_Paused(channel));
	}
	lua_pushinteger(L, Mix_Paused(-1));
	return 1;
}

/*@ Sound.volume([channel,] v)
 *# Sets the volume 
 *# {{v}} should be a value between {{0}} (minimum volume) and {{1}} (maximum volume).
 *# If {{channel}} is omitted, the volume of all channels will be set.
 *# It returns the volume of the {{channel}}. If the channel is not specified, it
 *# will return the average volume of all channels. To get the volume of a channel
 *# without changing it, use a negative value for {{v}}.
 */
static int sound_volume(lua_State *L) {
	int channel = -1;
	double v = -1;
	int vol = -1;
	if(lua_gettop(L) > 1) {
		channel = luaL_checkinteger(L, 1);
		v = luaL_checknumber(L, 2);	
	} else {
		v = luaL_checknumber(L, 1);	
	}
	if(v < 0) {
		vol = -1;
	} else {
		if(v > 1.0){
			v = 1.0;
		}
		vol = v * MIX_MAX_VOLUME;
	}		
	lua_pushinteger(L, Mix_Volume(channel, vol));
	return 1;
}

static const luaL_Reg sound_funcs[] = {
  {"play",  sound_play},
  {"loop",  sound_loop},
  {"pause",  sound_pause},
  {"resume",  sound_resume},
  {"halt",  sound_halt},
  {"playing",  sound_playing},
  {"paused",  sound_paused},
  {"volume",  sound_volume},
  {0, 0}
};

/*1 MusicObj
 *# The music object that encapsulates a music file in the engine.
 */

/*@ Music(filename)
 *# Loads the music file specified by {{filename}} from the
 *# [[Resources|Resource Management]] and returns it
 *# encapsulated within a `MusObj` instance.
 */
static int new_mus_obj(lua_State *L) {
	const char *filename = luaL_checkstring(L,1);
	
	Mix_Music **mp = lua_newuserdata(L, sizeof *mp);	
	luaL_setmetatable(L, "MusicObj");
	
	*mp = re_get_mus(filename);
	if(!*mp) {
		luaL_error(L, "Unable to load music file '%s'", filename);
	}
	return 1;
}

/*@ MusicObj:play([loops])
 *# Plays a piece of music.
 */
static int mus_play(lua_State *L) {	
	Mix_Music **mp = luaL_checkudata(L,1, "MusicObj");
	Mix_Music *m = *mp;
	
	int loops = -1;
	
	if(lua_gettop(L) > 1) {
		loops = luaL_checkinteger(L, 2);
	}
	
	if(Mix_PlayMusic(m, loops) == -1) {
		rerror("Unable to play music: %s", Mix_GetError());
	}	
		
	return 0;
}

/*@ MusicObj:halt()
 *# Stops music.
 */
static int mus_halt(lua_State *L) {	
	luaL_checkudata(L,1, "MusicObj");
	
	Mix_HaltMusic();
		
	return 0;
}

/*@ MusicObj:__tostring()
 *# Returns a string representation of the `MusicObj` instance.
 */
static int mus_tostring(lua_State *L) {	
	Mix_Music **mp = luaL_checkudata(L,1, "MusicObj");
	Mix_Music *m = *mp;
	lua_pushfstring(L, "MusicObj[%p]", m);
	return 1;
}

/*@ MusicObj:__gc()
 *# Garbage collects the `MusObj` instance.
 */
static int gc_mus_obj(lua_State *L) {
	/* No need to free the Mix_Music: It's in the resource cache. */
	return 0;
}

static void mus_obj_meta(lua_State *L) {
	/* Create the metatable for MusicObj */
	luaL_newmetatable(L, "MusicObj");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index"); /* MusicObj.__index = MusicObj */
			
	lua_pushcfunction(L, mus_play);
	lua_setfield(L, -2, "play");	
	lua_pushcfunction(L, mus_halt);
	lua_setfield(L, -2, "halt");	
	
	lua_pushcfunction(L, mus_tostring);
	lua_setfield(L, -2, "__tostring");	
	lua_pushcfunction(L, gc_mus_obj);
	lua_setfield(L, -2, "__gc");	
	
	/* The global method Music() */
	lua_pushcfunction(L, new_mus_obj);
	lua_setglobal(L, "Music");
}

/*1 GameDB
 *# The {/Game Database/}. 
 *# These functions are used to store key-value pairs in a
 *# globally available memory area. The purpose is twofold:
 *{
 ** To share data between different game states.
 ** To save game-specific variables in the "Save Game"/"Load Game" functionality.
 *}
 */

/*@ GameDB.set(key, value)
 *# Saves a key-value pair in the game database.\n
 *# {{key}} and {{value}} are converted to strings internally.
 */
static int gamedb_set(lua_State *L) {	
	const char *key = luaL_checkstring(L, 1);
	const char *val = luaL_checkstring(L, 2);
	gdb_put(key, val);
	return 0;
}

/*@ GameDB.get(key, [default])
 *# Retrieves the value associated with the {{key}} from the game database (as a string).\n
 *# If the key does not exist in the database it will either return {{default}} 
 *# if it is provided, otherwise {{nil}}.
 */
static int gamedb_get(lua_State *L) {	
	const char *key = luaL_checkstring(L, 1);
	const char *val = gdb_get_null(key);
	if(val)
		lua_pushstring(L, val);
	else {
		if(lua_gettop(L) > 1) 
			lua_pushvalue(L, 2);
		else
			lua_pushnil(L);
	}
	return 1;
}

/*@ GameDB.has(key)
 *# Returns {{true}} if the {{key}} is stored in the Game database.
 */
static int gamedb_has(lua_State *L) {	
	const char *key = luaL_checkstring(L, 1);
	lua_pushboolean(L, gdb_has(key));
	return 1;
}

/*@ LocalDB.set(key, value)
 *# Saves a key-value pair in the game database.\n
 *# {{key}} and {{value}} are converted to strings internally.
 */
static int gamedb_local_set(lua_State *L) {	
	const char *key = luaL_checkstring(L, 1);
	const char *val = luaL_checkstring(L, 2);
	gdb_local_put(key, val);
	return 0;
}

/*@ LocalDB.get(key, [default])
 *# Retrieves the value associated with the {{key}} from the game database (as a string).\n
 *# If the key does not exist in the database it will either return {{default}} 
 *# if it is provided, otherwise {{nil}}.
 */
static int gamedb_local_get(lua_State *L) {	
	const char *key = luaL_checkstring(L, 1);
	const char *val = gdb_local_get_null(key);
	if(val)
		lua_pushstring(L, val);
	else {
		if(lua_gettop(L) > 1) 
			lua_pushvalue(L, 2);
		else
			lua_pushnil(L);
	}
	return 1;
}

/*@ LocalDB.has(key)
 *# Returns {{true}} if the {{key}} is stored in the Game database.
 */
static int gamedb_local_has(lua_State *L) {	
	const char *key = luaL_checkstring(L, 1);
	lua_pushboolean(L, gdb_local_has(key));
	return 1;
}

static const luaL_Reg gdb_funcs[] = {
  {"set",  gamedb_set},
  {"get",  gamedb_get},
  {"has",  gamedb_has},
  {0, 0}
};

static const luaL_Reg gdb_local_funcs[] = {
  {"set",  gamedb_local_set},
  {"get",  gamedb_local_get},
  {"has",  gamedb_local_has},
  {0, 0}
};

/* STATE FUNCTIONS */

#define GLOBAL_FUNCTION(name, fun)	lua_pushcfunction(L, fun); lua_setglobal(L, name);
#define SET_TABLE_INT_VAL(k, v)     lua_pushstring(L, k); lua_pushinteger(L, v); lua_rawset(L, -3);

/*
Based on the discussion at http://stackoverflow.com/a/966778/115589
I've copied the code from Lua's linit.c and removed the functions
deemed unsafe.
*/
static const luaL_Reg sandbox_libs[] = {
  {"_G", luaopen_base},
  /*{LUA_LOADLIBNAME, luaopen_package}, */
  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_TABLIBNAME, luaopen_table},
  /*{LUA_IOLIBNAME, luaopen_io},*/
  /*{LUA_OSLIBNAME, luaopen_os},*/
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_BITLIBNAME, luaopen_bit32},
  {LUA_MATHLIBNAME, luaopen_math},
  /*{LUA_DBLIBNAME, luaopen_debug},*/
  {NULL, NULL}
};

void open_sandbox_libs (lua_State *L) {
	const luaL_Reg *lib;
	for (lib = sandbox_libs; lib->func; lib++) {
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);  /* remove lib */
	}
}

static int lus_init(struct game_state *s) {
	
	const char *map_file, *script_file;
	char *map_text, *script;
	lua_State *L = NULL;
	struct lustate_data *sd;
		
	rlog("Initializing Lua state '%s'", s->name);
	
	/* Load the Lua script */
	script_file = ini_get(game_ini, s->name, "script", NULL);	
	if(!script_file) {
		rerror("Lua state '%s' doesn't specify a script file.", s->name);
		return 0;
	}
	script = re_get_script(script_file);
	if(!script) {
		rerror("Script %s was not found (state %s).", script_file, s->name);
		return 0;
	}
	
	/* Create the Lua interpreter */
	L = luaL_newstate();
	if(!L) { 
		rerror("Couldn't create Lua state.");
		return 0;
	}
	s->data = L;
	
	/* Sandbox Lua instead of calling luaL_openlibs(L); */
	open_sandbox_libs(L);
	
	/* Create and init the State Data that the interpreter carries with it. */
	sd = malloc(sizeof *sd);
	if(!sd)
		return 0;
	
	sd->state = s;	
	sd->update_fcn = NULL;	
	sd->last_fcn = NULL;
	sd->atexit_fcn = NULL;	
	
	sd->n_timeout = 0;
	sd->map = NULL;
	sd->bmp = NULL;
		
	sd->change_state = 0;
	sd->next_state = NULL;
	
	/* Store the State Data in the interpreter */
	lua_pushlightuserdata(L, sd);		
	lua_setglobal(L, STATE_DATA_VAR);
	
	/* Load the map, if one is specified. */
	map_file = ini_get(game_ini, s->name, "map", NULL);
	if(map_file) {
		map_text = re_get_script(map_file);
		if(!map_text) {
			rerror("Unable to retrieve map resource '%s' (state %s).", map_file, s->name);
			return 0;		
		}
		
		sd->map = map_parse(map_text, 0);
		if(!sd->map) {
			rerror("Unable to parse map '%s' (state %s).", map_file, s->name);
			return 0;		
		}
		free(map_text);
		
		luaL_newlib(L, map_funcs);
		SET_TABLE_INT_VAL("BACKGROUND", 1);
		SET_TABLE_INT_VAL("CENTER", 2);
		SET_TABLE_INT_VAL("FOREGROUND", 3);
		SET_TABLE_INT_VAL("ROWS", sd->map->nr);
		SET_TABLE_INT_VAL("COLS", sd->map->nc);
		SET_TABLE_INT_VAL("TILE_WIDTH", sd->map->tiles.tw);
		SET_TABLE_INT_VAL("TILE_HEIGHT", sd->map->tiles.th);
	} else {
		rlog("Lua state %s does not specify a map file.", s->name);
		lua_pushnil(L);
	}
	lua_setglobal(L, "Map");
	
	GLOBAL_FUNCTION("log", l_log);
	GLOBAL_FUNCTION("setTimeout", l_set_timeout);
	GLOBAL_FUNCTION("onUpdate", l_onUpdate);
	GLOBAL_FUNCTION("atExit", l_atExit);
	GLOBAL_FUNCTION("import", l_import);
	GLOBAL_FUNCTION("advanceFrame", l_advanceFrame);
	
	luaL_newlib(L, game_funcs);
	/* Register some Lua variables. */	
	lua_setglobal(L, "Game");
	
	/* The graphics object G gives you access to all the 2D drawing functions */
	luaL_newlib(L, graphics_funcs);
	SET_TABLE_INT_VAL("FPS", fps);
	SET_TABLE_INT_VAL("SCREEN_WIDTH", 0);
	SET_TABLE_INT_VAL("SCREEN_HEIGHT", 0);
	lua_setglobal(L, "G");
	
	/* The input object Input gives you access to the keyboard and mouse. */
	luaL_newlib(L, keyboard_funcs);
	lua_setglobal(L, "Keyboard");
	
	luaL_newlib(L, mouse_funcs);
	SET_TABLE_INT_VAL("LEFT", 1);
	SET_TABLE_INT_VAL("MIDDLE", 2);
	SET_TABLE_INT_VAL("RIGHT", 3);
	lua_setglobal(L, "Mouse");
	
	luaL_newlib(L, gdb_funcs);
	lua_setglobal(L, "GameDB");	
	luaL_newlib(L, gdb_local_funcs);
	lua_setglobal(L, "LocalDB");
	
	luaL_newlib(L, sound_funcs);
	lua_setglobal(L, "Sound");	
	
	/* The Bitmap object is constructed through the Bmp() function that loads 
		a bitmap through the resources module that can be drawn with G.blit() */
	bmp_obj_meta(L);
	
	wav_obj_meta(L);
	
	mus_obj_meta(L);
	
	/* There is a function C('selector') that returns a CellObj object that
		gives you access to the cells on the map. */
	cell_obj_meta(L);
	
	if(luaL_dostring(L, base_lua)) {
		rerror("Unable load base library.");
		sublog("lua", "%s", lua_tostring(L, -1));
		free(script);				
		return 0;
	}
	
	/* Load the Lua script itself, and execute it. */
	if(luaL_loadstring(L, script)) {		
		rerror("Unable to load script %s (state %s).", script_file, s->name);
		sublog("lua", "%s", lua_tostring(L, -1));
		free(script);				
		return 0;
	}
	free(script);
	
	rlog("Running script %s", script_file);	
	if(lua_pcall(L, 0, 0, 0)) {
		rerror("Unable to execute script %s (state %s).", script_file, s->name);
		sublog("lua", "%s", lua_tostring(L, -1));
		return 0;
	}
		
	return 1;
}

static int lus_update(struct game_state *s, struct bitmap *bmp) {
	struct lustate_data *sd = NULL;
	lua_State *L = s->data;
	struct callback_function *fn;
	
	assert(L);
	
	lua_getglobal(L, STATE_DATA_VAR);
	if(!lua_isnil(L,-1)) {
		if(!lua_islightuserdata(L, -1)) {
			rerror("Variable %s got tampered with (lus_update)", STATE_DATA_VAR);
			return 0;
		}
		sd = lua_touserdata(L, -1);
	} else {
		rerror("Variable %s got tampered with (lus_update)", STATE_DATA_VAR);
		return 0;
	}
	lua_pop(L, 1);
	
	sd->bmp = bmp;
	
	lua_getglobal (L, "G");
	SET_TABLE_INT_VAL("SCREEN_WIDTH", bmp->w);
	SET_TABLE_INT_VAL("SCREEN_HEIGHT", bmp->h);
	
	/* TODO: Maybe background colour metadata in the map file? */
	bm_set_color_s(bmp, "black");
	bm_clear(bmp);
	
	process_timeouts(L);
	
	fn = sd->update_fcn;
	while(fn) {				
		/* Retrieve the callback */
		lua_rawgeti(L, LUA_REGISTRYINDEX, fn->ref);
		
		/* Call it */
		if(lua_pcall(L, 0, 0, 0)) {
			rerror("Unable to execute onUpdate() callback (%d)", fn->ref);
			sublog("lua", "%s", lua_tostring(L, -1));
			/* Should we remove it maybe? */
		}
		
		fn = fn->next;
	}
	
	if(sd->change_state) {
		if(!sd->next_state) {
			rwarn("Lua script didn't specify a next state; terminating...");
			change_state(NULL);
		} else {	
			rlog("Lua script changing state to %s", sd->next_state);
			set_state(sd->next_state);
		}
	}
	
	return 1;
}

static int lus_deinit(struct game_state *s) {
	lua_State *L = s->data;
	struct lustate_data *sd;
	
	if(!L)
		return 0;
		
	lua_getglobal(L, STATE_DATA_VAR);
	if(!lua_isnil(L,-1)) {
		if(!lua_islightuserdata(L, -1)) {
			rerror("Variable %s got tampered with (lus_deinit)", STATE_DATA_VAR);
		} else {
			struct callback_function *fn;
			
			sd = lua_touserdata(L, -1);			
			
			/* Execute the atExit() callbacks */
			fn = sd->atexit_fcn;
			while(fn) {				
				struct callback_function *old = fn;
				lua_rawgeti(L, LUA_REGISTRYINDEX, fn->ref);
				if(lua_pcall(L, 0, 0, 0)) {
					rerror("Unable to execute atExit() callback (%d)", fn->ref);
					sublog("lua", "%s", lua_tostring(L, -1));
				}
				fn = fn->next;
				free(old);
			}
			
			/* Remove the map */
			map_free(sd->map);
			
			while(sd->update_fcn) {
				fn = sd->update_fcn;
				sd->update_fcn = sd->update_fcn->next;
				free(fn);
			}
			
			if(sd->next_state);
				free(sd->next_state);
			
			free(sd);
		}
	}
	lua_pop(L, 1);
	
	/* Stop all sounds */
	Mix_HaltChannel(-1);
	Mix_HaltMusic();
	
	lua_close(L);
	L = NULL;
	
	return 1;
}

struct game_state *get_lua_state(const char *name) {
	struct game_state *state = malloc(sizeof *state);
	if(!state)
		return NULL;
	
	state->data = NULL;
	
	state->init = lus_init;
	state->update = lus_update;
	state->deinit = lus_deinit;
	
	return state;
}