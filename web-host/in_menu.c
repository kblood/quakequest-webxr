/*
 * in_menu.c — VR menu toggle + menu navigation input (Milestone 3, HUD/menus).
 * See in_menu.h for the binding rationale (off-hand thumbstick click = menu).
 *
 * Faithful port of the fork's big-screen branches:
 *  - menu toggle: off-hand thumbstick click -> K_ESCAPE (down+up), the
 *    rebind of the fork's handleTrackedControllerButton(..., xrButton_Enter,
 *    K_ESCAPE) (QuakeQuest_OpenXR.c:912-914) — K_ESCAPE down runs togglemenu.
 *    M3 INTEGRATION CHANGE (reports/09-m3-integration.md): chunk 4
 *    (in_comfort.c) independently bound RECENTER to the same off-hand click
 *    — both chunks' reports justified the pick as "the one input the fork
 *    left free", a collision only visible in the merged build. This module
 *    now owns the whole gesture as its single reader:
 *      short press (release before 600 ms)  -> menu toggle (K_ESCAPE dn+up)
 *      LONG press (held >= 600 ms, in-game) -> WebXRComfort_Recenter()
 *                                              (+ haptic confirm, no menu)
 *    The toggle firing on release instead of press costs ~nothing in menu
 *    latency and keeps recenter reachable in-headset without stealing any
 *    other input. While the menu/console is up (bigScreen != 0) the long
 *    press is disabled — a recenter under the world-anchored menu quad
 *    would yank the quad's anchor; any press just closes the menu.
 *  - while bigScreen != 0 (menu or console up): BOTH thumbsticks emulate a
 *    d-pad with the fork's exact ±0.7 edge thresholds and keys
 *    ('a'/'d' = left/right — the fork's menu.c handles these — and
 *    K_UPARROW/K_DOWNARROW), right-hand A = K_ENTER, right-hand B = K_ESCAPE
 *    (QuakeQuest_OpenXR.c:837-862, 916-933);
 *  - outside bigScreen mode this module emits NOTHING except the toggle
 *    (A=jump, weapon flicks etc. belong to the other M3 chunks).
 *
 * Edge detection uses private previous-state copies, not the shared
 * *TrackedRemoteState_old globals (those are copied new->old by the gameplay
 * chunks per the webxr_input.h contract; using them here would race with
 * whichever handler runs second).
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <emscripten.h> /* emscripten_get_now — long-press timing */

#include "quakedef.h"
#include "keys.h"

#include "webxr_input.h"
#include "in_menu.h"
#include "in_comfort.h"   /* WebXRComfort_Recenter — off-hand click LONG press */
#include "vr_menu_quad.h" /* bigScreen */

void QC_KeyEvent(int state, int key, int character); /* vid_android.c -> Key_Event */
bool WebXRBridge_IsSessionActive(void);              /* webxr_bridge.h */

extern cvar_t cl_righthanded; /* cl_input.c — selects the off-hand */

/* private previous-state copies (see header) */
static WebXRRemoteState s_prev[WEBXR_HAND_COUNT];

/* off-hand thumbstick-click gesture state (short press = menu toggle,
 * long press = recenter — see file header) */
#define MENU_LONGPRESS_MS 600.0
static double s_offClickStartMs = -1.0;  /* <0 = not pressed */
static bool   s_offClickConsumed = false; /* long-press already fired */

/* fork's handleTrackedControllerButton (QuakeQuest_OpenXR.c:595-603):
 * forward both the press and the release edge as key state changes */
static void MenuButton(const WebXRRemoteState *now, const WebXRRemoteState *prev,
                       uint32_t bit, int key)
{
	if ((now->Buttons & bit) != (prev->Buttons & bit))
		QC_KeyEvent((now->Buttons & bit) ? 1 : 0, key, 0);
}

/* fork's joystick d-pad edges (QuakeQuest_OpenXR.c:839-854): threshold has
 * the direction in its sign; key goes down when the stick crosses out and
 * up when it comes back */
static void MenuJoyEdge(float cur, float prev, float threshold, int key)
{
	int now = (threshold > 0.0f) ? (cur > threshold) : (cur < threshold);
	int was = (threshold > 0.0f) ? (prev > threshold) : (prev < threshold);
	if (now != was)
		QC_KeyEvent(now, key, 0);
}

void IN_Menu_HandleInput(void)
{
	const WebXRRemoteState *hand[WEBXR_HAND_COUNT] = {
		&leftTrackedRemoteState_new, &rightTrackedRemoteState_new
	};
	int offHand = cl_righthanded.integer ? WEBXR_HAND_LEFT : WEBXR_HAND_RIGHT;
	uint32_t offThumbBit = (offHand == WEBXR_HAND_LEFT) ? xrButton_LThumb : xrButton_RThumb;
	int i;

	if (!WebXRBridge_IsSessionActive())
	{
		memset(s_prev, 0, sizeof(s_prev));
		s_offClickStartMs = -1.0;
		s_offClickConsumed = false;
		return;
	}

	/* off-hand thumbstick click gesture (rebind of the fork's unavailable
	 * Quest Menu button; see file header for the short/long-press split):
	 *   short press -> menu toggle (K_ESCAPE down+up on release);
	 *   long press (>= MENU_LONGPRESS_MS, in-game only) -> recenter. */
	{
		bool down = (hand[offHand]->Buttons & offThumbBit) != 0;
		bool was  = (s_prev[offHand].Buttons & offThumbBit) != 0;
		double now = emscripten_get_now();

		if (down && !was)
		{
			s_offClickStartMs = now;
			s_offClickConsumed = false;
		}
		else if (down && !s_offClickConsumed && s_offClickStartMs >= 0.0 &&
		         (now - s_offClickStartMs) >= MENU_LONGPRESS_MS &&
		         bigScreen == 0 /* with the menu up, never recenter (the quad
		                         * anchor would yank) — let the release fall
		                         * through to the short-press toggle instead,
		                         * so any click while in a menu closes it */)
		{
			s_offClickConsumed = true;
			WebXRComfort_Recenter();
			/* haptic confirm on the pressing (off-)hand: an eyes-free cue
			 * that the long press registered as recenter, not menu */
			WebXRInput_Vibrate(150, (offHand == WEBXR_HAND_LEFT) ? 1 : 2, 0.7f);
		}
		else if (!down && was)
		{
			if (!s_offClickConsumed)
			{
				/* short press: the fork forwarded press+release of K_ESCAPE
				 * (handleTrackedControllerButton); firing the pair on release
				 * preserves those Key_Event semantics exactly */
				QC_KeyEvent(1, K_ESCAPE, 0);
				QC_KeyEvent(0, K_ESCAPE, 0);
			}
			s_offClickStartMs = -1.0;
			s_offClickConsumed = false;
		}
	}

	if (bigScreen != 0)
	{
		/* d-pad emulation on BOTH sticks (fork lines 837-854 + 916-932) */
		for (i = 0; i < WEBXR_HAND_COUNT; i++)
		{
			MenuJoyEdge(hand[i]->Joystick.x, s_prev[i].Joystick.x,  0.7f, 'd');
			MenuJoyEdge(hand[i]->Joystick.x, s_prev[i].Joystick.x, -0.7f, 'a');
			MenuJoyEdge(hand[i]->Joystick.y, s_prev[i].Joystick.y,  0.7f, K_UPARROW);
			MenuJoyEdge(hand[i]->Joystick.y, s_prev[i].Joystick.y, -0.7f, K_DOWNARROW);
		}

		/* select / back on the right hand (fork lines 856-862) */
		MenuButton(hand[WEBXR_HAND_RIGHT], &s_prev[WEBXR_HAND_RIGHT], xrButton_A, K_ENTER);
		MenuButton(hand[WEBXR_HAND_RIGHT], &s_prev[WEBXR_HAND_RIGHT], xrButton_B, K_ESCAPE);
	}

	s_prev[WEBXR_HAND_LEFT]  = leftTrackedRemoteState_new;
	s_prev[WEBXR_HAND_RIGHT] = rightTrackedRemoteState_new;
}
