/*
X360_DEBUG_TOOLS.C

What the console's build says for a developer tool that is not in it
(halo_xbox360_prefix.h, configure.py --console-debug-tools): a console
command or script that reaches the tool prints that, and the game goes on.
As on the original Xbox (warthog-xbox's port/xbox/game/console_debug_tools.c).
*/

#include "cseries.h"
#include "main/console.h"

#ifdef HALO_CONSOLE_NO_DEBUG_TOOLS

void console_debug_tools_unavailable(char const *name)
{
	console_printf(FALSE, "%s: not in this build (configure.py --console-debug-tools)", name);
	return;
}

#endif
