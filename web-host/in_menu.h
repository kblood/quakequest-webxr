/*
 * in_menu.h — VR menu toggle + menu navigation input (Milestone 3, HUD/menus).
 *
 * Ports the fork's big-screen input branches (QuakeQuest_OpenXR.c:837-862,
 * 916-933): thumbstick deflection and face buttons become EMULATED KEYPRESSES
 * into the engine's standard Key_Event path — no laser pointers, no hit
 * testing (reports/06 §3.3).
 *
 * MENU TOGGLE REBIND: the fork bound "open menu" to the Quest Menu button
 * (xrButton_Enter), which browsers reserve and never surface in the Gamepad
 * API (reports/06 §1.4). Rebound here to the OFF-HAND THUMBSTICK CLICK
 * (left stick for the default right-handed config, right stick when
 * cl_righthanded 0). Rationale: the off-hand stick click is the one Touch
 * input the fork left completely unused — the DOMINANT stick click cycles
 * r_lasersight (chunk 2) and X/Y are quicksave/quickload (chunk 4).
 *
 * M3 INTEGRATION: chunk 4 independently bound RECENTER to the same click
 * (same "only free input" rationale — a collision only the merged build
 * could show). This module now owns the whole gesture: SHORT press
 * (release < 600 ms) = menu toggle, LONG press (>= 600 ms, in-game only) =
 * WebXRComfort_Recenter(). See reports/09-m3-integration.md.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */
#ifndef IN_MENU_H_
#define IN_MENU_H_

/* Poll controller state and emit menu-key events. Called once per XR frame
 * (after WebXRInput_Update, before the frame is pumped). Keeps its own
 * previous-state copies for edge detection — it does NOT write the shared
 * *TrackedRemoteState_old globals, so it composes with the other M3 chunks'
 * input handlers regardless of call order. */
void IN_Menu_HandleInput(void);

#endif
