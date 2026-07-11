/*
 * sys_web.c — Emscripten/browser replacement for sys_linux.c
 *
 * Provides the Sys_* platform functions the darkplaces engine expects.
 * The process entry point (main) lives in main_web.c; this file is only
 * the console/error/terminal plumbing.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "quakedef.h"

void Sys_Shutdown (void)
{
	fflush(stdout);
}

void Sys_Error (const char *error, ...)
{
	va_list argptr;
	char string[MAX_INPUTLINE];

	va_start (argptr, error);
	dpvsnprintf (string, sizeof (string), error, argptr);
	va_end (argptr);

	fprintf(stderr, "Quake Error: %s\n", string);

	Host_Shutdown ();
	exit (1);
}

void Sys_PrintToTerminal(const char *text)
{
	/* Emscripten routes stdout to the browser console (line-buffered). */
	fputs(text, stdout);
}

char *Sys_ConsoleInput(void)
{
	/* No interactive terminal in the browser; the in-game console is used. */
	return NULL;
}

char *Sys_GetClipboardData (void)
{
	return NULL;
}

void Sys_InitConsole (void)
{
}

qboolean sys_supportsdlgetticks = false;
unsigned int Sys_SDL_GetTicks (void)
{
	Sys_Error("Called Sys_SDL_GetTicks on non-SDL target");
	return 0;
}
void Sys_SDL_Delay (unsigned int milliseconds)
{
	Sys_Error("Called Sys_SDL_Delay on non-SDL target");
}
