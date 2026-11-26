/*
X360_FORGE_LEFT_OUT.C

Forge's calls into the parts the consoles leave out (HALO_CONSOLE_NO_FORGE:
the game's port/linux/game/forge_ai_tags.c, and game_engine.c's vehicle
homes): forge.c is built here, and the 360's link keeps every call it
makes, so they answer as a game with no AI packs and no layout would.
Nothing here runs while Forge is off.
*/

#include "cseries.h"
#include "halo_port_features.h"

#ifdef HALO_CONSOLE_NO_FORGE

boolean forge_ai_tags_update(void)
{
	return FALSE;
}

boolean forge_ai_tags_pending(void)
{
	return FALSE;
}

char const *forge_ai_tags_source_map(long tag_index)
{
	(void)tag_index;
	return NULL;
}

short forge_ai_tags_faction_count(void)
{
	return 0;
}

boolean forge_ai_tags_faction(short faction, char const **name, char const **map)
{
	(void)faction;
	if (name)
		*name = NULL;
	if (map)
		*map = NULL;
	return FALSE;
}

short forge_ai_tags_faction_category(short faction)
{
	(void)faction;
	return 0;
}

/* (every tag the map's own: Forge offers what it always did) */
boolean forge_ai_tags_offered(long tag_index)
{
	(void)tag_index;
	return TRUE;
}

short forge_ai_tags_faction_of(long tag_index)
{
	(void)tag_index;
	return NONE;
}

void game_engine_vehicle_home_forget(long object_index)
{
	(void)object_index;
}

#endif
