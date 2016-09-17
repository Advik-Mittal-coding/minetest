/*
Minetest
Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "scripting_player_physics.h"
#include "cpp_api/s_internal.h"
#include "lua_api/l_util.h"
#include "lua_api/l_player_physics.h"
#include "common/c_content.h"
#include "server.h"
#include "log.h"
#include "settings.h"
#include "client.h"
#include "filesys.h"

extern "C" {
#include "lualib.h"
}

PlayerPhysicsScripting::PlayerPhysicsScripting(Client *client)
{
	setClient(client);

	SCRIPTAPI_PRECHECKHEADER

	// Always initialize security
	initializeSecurity();

	lua_getglobal(L, "core");
	int top = lua_gettop(L);

	// Initialize our lua_api modules
	InitializeModApi(L, top);
	lua_pop(L, 1);

	// Push builtin initialization type
	lua_pushstring(L, "local_player_physics");
	lua_setglobal(L, "INIT");

	// Run builtin stuff
	std::string script = porting::path_share + DIR_DELIM "builtin" + DIR_DELIM "init.lua";
	loadMod(script, BUILTIN_MOD_NAME);
}

void PlayerPhysicsScripting::loadScriptContent(const std::string &script_content)
{
	verbosestream<<"PlayerPhysicsScripting::loadScriptContent: \""<<script_content
			<<"\""<<std::endl;

	lua_State *L = getStack();

	int error_handler = PUSH_ERROR_HANDLER(L);

	bool ok = ScriptApiSecurity::safeLoadContent(
			L, "player_physics_script", script_content);
	ok = ok && !lua_pcall(L, 0, 0, error_handler);
	if (!ok) {
		std::string error_msg = lua_tostring(L, -1);
		lua_pop(L, 2); // Pop error message and error handler
		throw ModError("Failed to load and run player physics script:\n"+error_msg);
	}
	lua_pop(L, 1); // Pop error handler
}

void PlayerPhysicsScripting::apply_control(float dtime, const PlayerControl &control)
{
	lua_State *L = getStack();

	int error_handler = PUSH_ERROR_HANDLER(L);

	lua_getglobal(L, "core");
	lua_getfield(L, -1, "registered_local_player_physics_apply_control");
	if (lua_isnil(L, -1)){
		lua_pop(L, 2);
		return;
	}
	lua_remove(L, -2); // Remove core

	if (lua_type(L, -1) != LUA_TFUNCTION) {
		return;
	}
	lua_pushnumber(L, dtime);
	push_player_control(L, control);
	PCALL_RES(lua_pcall(L, 2, 0, error_handler));
	lua_pop(L, 1); // Pop error handler
}

void PlayerPhysicsScripting::move(float dtime)
{
}

void PlayerPhysicsScripting::InitializeModApi(lua_State *L, int top)
{
	// Initialize mod api modules
	ModApiUtil::Initialize(L, top);
	ModApiPlayerPhysics::Initialize(L, top);

	// Register reference classes (userdata)
	// (none)
}

