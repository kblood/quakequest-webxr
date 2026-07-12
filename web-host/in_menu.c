/*
 * in_menu.c — VR menu toggle + menu navigation input (Milestone 3, HUD/menus).
 * See in_menu.h for the binding rationale (off-hand thumbstick click = menu).
 *
 * Faithful port of the fork's big-screen branches:
 *  - menu toggle: press/release forwarded as K_ESCAPE, exactly like
 *    handleTrackedControllerButton(..., xrButton_Enter, K_ESCAPE)
 *    (QuakeQuest_OpenXR.c:912-914) — K_ESCAPE down runs togglemenu;
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

#include "quakedef.h"
#include "keys.h"

#include "webxr_input.h"
#include "in_menu.h"
#include "vr_menu_quad.h" /* bigScreen */

void QC_KeyEvent(int state, int key, int character); /* vid_android.c -> Key_Event */
bool WebXRBridge_IsSessionActive(void);              /* webxr_bridge.h */

extern cvar_t cl_righthanded; /* cl_input.c — selects the off-hand */

/* private previous-state copies (see header) */
static WebXRRemoteState s_prev[WEBXR_HAND_COUNT];

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
		return;
	}

	/* menu toggle: off-hand thumbstick click -> K_ESCAPE (rebind of the
	 * fork's unavailable Quest Menu button; see in_menu.h) */
	MenuButton(hand[offHand], &s_prev[offHand], offThumbBit, K_ESCAPE);

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
