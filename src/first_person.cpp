// First Person: the game seen through Mario's eyes.
//
// V (key.toggle) or a D-pad button (the Mods menu) switches it on and off.
// The C-stick turns and tilts the view, and Mario runs where you look. L
// looks the way he faces.
//
// How: SMS_EV_CAMERA moves what is drawn each tick to Mario's eyes, looking
// the view's own way. The game's camera still follows him behind, but the
// C-stick no longer reaches it (SMS_EV_PAD_READ), so walls it bumps into
// cannot stop the view turning; the control stick is turned by the
// difference between the two, as the game moves Mario relative to its
// camera. SMS_EV_HIDE_PLAYER_MODEL leaves his model and FLUDD out. During
// cutscenes and talks the game's own camera is left as it is.
//
// Split-screen co-op (a game with held_fludd's select_player): each player has a
// first-person view of their own, switched on and off with their own controller's
// button (the keyboard's V and the mouse are player 1's). Everything below that is
// a player's is kept for each (View, vs[player]); `cur` is the player being worked on.
#include <sms_mod.hpp>
#include "recommended_preset.h"

#include <math.h>
#include <string>
#include <stddef.h>

namespace {

const float kPi = 3.14159265f;

// What of Mario shows in first person (the "model" setting; 2, the whole of him,
// is no longer offered: an old setting of it is Body only)
enum { MODEL_BODY = 0, MODEL_HIDDEN = 1 };
const char* const kModels[] = {"Body only (no head or FLUDD)", "Hidden"};

const char* const kButtons[] = {"D-pad Down", "D-pad Up", "D-pad Left", "D-pad Right", "None"};
const uint16_t kButtonBits[] = {SMS_PAD_DOWN, SMS_PAD_UP, SMS_PAD_LEFT, SMS_PAD_RIGHT, 0};

// the crosshair's colors (the "crosshair_color" setting)
const char* const kCrossNames[] = {"White", "Red", "Green", "Blue", "Yellow", "Cyan", "Pink", "Orange", "Grey", "Black"};
const SmsColor kCrossColors[]   = {SMS_RGB(255, 255, 255), SMS_RGB(255, 60, 60),  SMS_RGB(70, 230, 90),
                                   SMS_RGB(70, 150, 255),  SMS_RGB(255, 225, 40), SMS_RGB(60, 235, 235),
                                   SMS_RGB(255, 110, 200), SMS_RGB(255, 150, 40), SMS_RGB(150, 150, 150),
                                   SMS_RGB(20, 20, 20)};
const int kCrossCount = 10;

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// FLUDD held in view like a gun: api->held_fludd (Chase's game from beta 2026.11.06; on an older
// game SMS_API_HAS says no). Taken only if its table says so (its magic).
const SmsHeldFluddApi* heldFluddApi(const SmsApi& api)
{
	if (api.size < offsetof(SmsApi, held_fludd) + sizeof(void*) || !api.held_fludd) // SMS_API_HAS
		return nullptr;
	const SmsHeldFluddApi* h = api.held_fludd;
	return h->size >= offsetof(SmsHeldFluddApi, set) + sizeof(void*) && h->magic == SMS_HELD_FLUDD_MAGIC && h->set ? h
	                                                                                                              : nullptr;
}

// Co-op (held_fludd's select_player, First Person beta 2026.11.10): how many players, the one the
// game runs as now, and making api->player act on one
bool hasPlayers(const SmsHeldFluddApi* h)
{
	return h && h->size >= offsetof(SmsHeldFluddApi, player_view) + sizeof(void*) && h->players && h->player
	       && h->select_player && h->player_view;
}

} // namespace

struct FirstPerson : sms::Mod {
	using sms::Mod::Mod;

	RecommendedPreset preset{*this}; // the "Settings preset" row: Recommended (the defaults) or Custom

	// Each player's view (co-op: two; alone, the first)
	struct View {
		bool enabled   = false;
		float pitch    = 0;     // degrees, up positive: the C-stick's tilt
		float eyeY     = 0;     // the eye's height, smoothed
		bool haveEye   = false;
		bool viewIsOurs = false; // the camera was put at his eyes this tick
		// Where the view looks across the ground, and where the game's camera does
		// (degrees: 0 is +Z, 90 is +X). The C-stick turns ours; the game's camera
		// still follows Mario, but no longer turns ours, so walls cannot stop it.
		float yaw = 0, gameYaw = 0;
		bool haveYaw = false, haveGameYaw = false;
		bool aiming = false;     // set_aim is on
		bool headHidden = false; // set_hidden_parts hides his head and FLUDD
		bool fluddHeld = false;  // held_fludd->set holds it
		float fovNow        = 0.0f;  // the view's field of view now (eased to a zoom and back)
		bool flippedForZoom = false; // first person switched on for a zoom from third person
		bool skipZoom       = false; // switched by hand during one: that one left alone until it ends
		double turnedAt = -1; // when noHardTurn last turned him (core().time())
		bool aWas       = false;
		uint32_t lastStatus = 0;
	};
	View vs[2];
	int cur = 0; // the player being worked on
	View& v() { return vs[cur]; }
	const View& v() const { return vs[cur]; }

	int toggleKey  = -1;
	bool toldNoYaw = false;
	double toastUntil = 0;
	const char* toastText = "";
	int toastPlayer = 0;

	// co-op: the players, and api->player made to act on one (-1: the one the game runs as)
	int players() const
	{
		const SmsHeldFluddApi* h = heldFluddApi(api());
		return hasPlayers(h) ? (h->players() == 2 ? 2 : 1) : 1;
	}
	int playerNow() const
	{
		const SmsHeldFluddApi* h = heldFluddApi(api());
		return hasPlayers(h) && h->player() == 1 ? 1 : 0;
	}
	void select(int player) const
	{
		const SmsHeldFluddApi* h = heldFluddApi(api());
		if (hasPlayers(h))
			h->select_player(player);
	}
	// work on player p (api->player acting on them) for as long as it lives
	struct For {
		FirstPerson& m;
		int saved;
		For(FirstPerson& mod, int p) : m(mod), saved(mod.cur)
		{
			m.cur = p;
			m.select(p);
		}
		~For()
		{
			m.cur = saved;
			m.select(-1);
		}
	};

	void toast(const char* text)
	{
		toastText   = text;
		toastUntil  = core().time() + 1.5;
		toastPlayer = cur;
	}

	void flip(bool quiet = false)
	{
		View& w   = v();
		w.enabled = !w.enabled;
		w.haveYaw = false; // from the game camera again when it starts
		if (!w.enabled) {
			w.viewIsOurs = false;
			aimFludd(); // lets his body go, if it was turned
			showHead();
			holdFludd();
		}
		w.pitch   = 0;
		w.haveEye = false;
		if (!quiet)
			toast(w.enabled ? "First person" : "Third person");
		info("%s%s", cur ? "player 2: " : "", w.enabled ? "on" : "off");
	}

	// Whether the view is ours now: on, Mario there, no cutscene or talk.
	bool active() const
	{
		if (!v().enabled)
			return false;
		if (SMS_API_HAS(&api(), world) && api().world && SMS_API_HAS(api().world, cutscene_playing) &&
		    api().world->cutscene_playing())
			return false;
		SmsPlayer p = SmsPlayer();
		p.size      = sizeof p;
		return api().player->get(&p) != 0;
	}

	bool hasHeadApi() const { return SMS_API_HAS(api().player, set_hidden_parts); }

	// What of him shows: his body without his head needs player->set_hidden_parts
	// (mod API 1.5 beta 8); without it, he is hidden.
	int modelMode() const
	{
		const int m = setting("model", MODEL_BODY) == MODEL_HIDDEN ? MODEL_HIDDEN : MODEL_BODY;
		return m == MODEL_BODY && !hasHeadApi() ? MODEL_HIDDEN : m;
	}

	// FLUDD in view, held like a gun (the game moves its own model there), while the view is his;
	// a weapon chosen by the FLUDD Weapons mod is held there instead (the game draws its model)
	bool noCrosshair = false; // a stage's opening, or Mario dead: no crosshair
	bool onCoaster   = false; // Pinna Park's roller coaster (area 0x3A, stage 0 or 1)
	bool toldNoHeld = false;
	void holdFludd()
	{
		const SmsHeldFluddApi* h = heldFluddApi(api());
		const bool want          = v().enabled && v().viewIsOurs && setting("fludd_view", 1) && !holding();
		if (h && h->size >= offsetof(SmsHeldFluddApi, set_view) + sizeof(void*) && h->set_view)
			h->set_view(v().enabled && v().viewIsOurs ? 1 : 0); // for the FLUDD Weapons mod
		if (!h) {
			if (want && !toldNoHeld) {
				warn("this game cannot show FLUDD in view (it needs a Remastered or First Person beta build)");
				toldNoHeld = true;
			}
			return;
		}
		if (want) {
			const float d = (float)setting("fludd_distance", 55);
			h->set(1, d * 0.42f, d * 0.40f, d, 0.55f, 1200.0f);
			v().fluddHeld = true;
		} else if (v().fluddHeld) {
			h->set(0, 0, 0, 0, 1, 0);
			v().fluddHeld = false;
		}
	}

	// What the FLUDD Weapons mod asks (held_fludd's get_state): the Rifle's zoom, its scope, and
	// first person for a zoom from third person.
	SmsHeldFluddState weaponsState() const
	{
		SmsHeldFluddState st = SmsHeldFluddState();
		st.size              = sizeof st;
		const SmsHeldFluddApi* h = heldFluddApi(api());
		if (!h || h->size < offsetof(SmsHeldFluddApi, get_state) + sizeof(void*) || !h->get_state || !h->get_state(&st))
			st = SmsHeldFluddState();
		return st;
	}
	// Mouse look: the game's captured mouse (F10) turns and tilts the view, as in a PC shooter
	// (api->held_fludd take_mouse; nothing on a game without it). Player 1's.
	void mouseLook(float slow)
	{
		if (cur != 0)
			return;
		const SmsHeldFluddApi* h = heldFluddApi(api());
		if (!setting("mouse_look", 1) || !h || h->size < offsetof(SmsHeldFluddApi, take_mouse) + sizeof(void*) || !h->take_mouse)
			return;
		float mx = 0.0f, my = 0.0f;
		if (!h->take_mouse(&mx, &my) || (mx == 0.0f && my == 0.0f))
			return;
		const float k = 0.05f * (float)setting("mouse_speed", 100) / 100.0f * slow; // degrees a pixel
		v().yaw -= mx * k; // right turns right: the yaw goes down
		v().pitch = clampf(v().pitch + (setting("mouse_invert", 0) ? my : -my) * k, -75.0f, 75.0f);
	}
	void flipByHand()
	{
		flip();
		v().skipZoom       = v().flippedForZoom || (weaponsState().zoom_flags & SMS_ZOOM_FIRST_PERSON) != 0;
		v().flippedForZoom = false;
	}
	void zoomFirstPerson()
	{
		const bool wants = (weaponsState().zoom_flags & SMS_ZOOM_FIRST_PERSON) != 0;
		View& w = v();
		if (w.skipZoom) {
			w.skipZoom = wants;
			return;
		}
		if (wants && !w.enabled) {
			flip(true);
			w.flippedForZoom = true;
		} else if (!wants && w.flippedForZoom) {
			if (w.enabled)
				flip(true);
			w.flippedForZoom = false;
		}
	}

	// A small crosshair in the middle of the view while FLUDD is held there (the spray and
	// underwater nozzles, off Yoshi): where its water goes

	void drawCrosshair()
	{
		if (!v().fluddHeld || !v().viewIsOurs || noCrosshair || !setting("crosshair", 1) || (weaponsState().zoom_flags & SMS_ZOOM_SCOPE))
			return; // (none under the FLUDD Weapons mod's scope)
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m) || !m.has_fludd || m.on_yoshi || (m.current_nozzle != 0 && m.current_nozzle != 2))
			return;
		if (m.current_nozzle == 2 && !weaponsState().held)
			return; // the underwater nozzle as a jet pack (plain FLUDD, on his back): nothing to aim; a weapon there holds it
		float w = 0, h = 0;
		draw().canvas(&w, &h);
		// the middle of this player's view (co-op: their part of the screen)
		float vx = 0.0f, vy = 0.0f, vw = 1.0f, vh = 1.0f;
		const SmsHeldFluddApi* hf = heldFluddApi(api());
		if (hasPlayers(hf) && players() == 2)
			hf->player_view(cur, &vx, &vy, &vw, &vh);
		const float x = w * (vx + vw * 0.5f), y = h * (vy + vh * 0.5f), r = h * 0.009f;
		const int ci     = setting("crosshair_color", 8);
		const SmsColor c = kCrossColors[ci >= 0 && ci < kCrossCount ? ci : 0];
		// an outline round it, dark (light round a black one), so it shows on any ground
		const SmsColor edge = ci == kCrossCount - 1 ? SMS_RGBA(255, 255, 255, 120) : SMS_RGBA(0, 0, 0, 110);
		draw().circle_outline(x, y, r, r * 0.55f, edge);
		draw().circle_outline(x, y, r, r * 0.25f, c);
		draw().circle(x, y, r * 0.22f, c);
	}

	// His head and FLUDD out while the view is his and his body shows; back otherwise.
	void showHead()
	{
		const bool want = v().enabled && v().viewIsOurs && modelMode() == MODEL_BODY;
		if (want != v().headHidden && hasHeadApi()) {
			api().player->set_hidden_parts(want ? SMS_PLAYER_PART_HEAD | SMS_PLAYER_PART_FLUDD : 0);
			v().headHidden = want;
		}
	}

	// Standing still with his body showing, he turns to face the view, so
	// looking down shows him the right way round (and he sets off ahead).
	void faceView(const SmsPad& p)
	{
		if (modelMode() == MODEL_HIDDEN || v().aiming || !SMS_API_HAS(api().player, set_yaw))
			return;
		if (fabsf(p.stick_x) > 0.2f || fabsf(p.stick_y) > 0.2f)
			return;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (api().player->get(&m) && m.on_ground && !m.in_water && fabsf(m.forward_speed) < 1.0f)
			api().player->set_yaw(v().yaw);
	}

	// No hard turns: running, a stick the other way would have him skid round
	// (his facing far from where the stick points, the game's turnaround). In
	// first person the view turns, not him, so he turns that way at once, as
	// the game would after the skid, and runs on. a, b: the stick as read
	// (up the way the view looks).
	// A side flip comes out of the skid (A during the turnaround); without the
	// skid, A just after the turn starts it all the same.
	void sideFlip(bool aDown)
	{
		const bool pressed = aDown && !v().aWas;
		v().aWas           = aDown;
		if (!pressed || v().turnedAt < 0 || core().time() - v().turnedAt > 0.3)
			return;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (api().player->get(&m) && m.on_ground && !m.in_water && !m.on_yoshi) {
			api().player->set_status(0x887, 0); // MARIO_STATUS_U_TURN_JUMP: the side flip
			v().turnedAt = -1;
		}
	}

	void noHardTurn(float a, float b)
	{
		if (a * a + b * b < 40.0f * 40.0f || !SMS_API_HAS(api().player, set_yaw))
			return;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m) || !m.on_ground || m.in_water || m.forward_speed < 4.0f)
			return;
		// the stick's angle, clockwise from up, turns the yaw down (right is -90)
		const float want = v().yaw - atan2f(a, b) * 180.0f / kPi;
		float diff = fmodf(want - m.yaw, 360.0f);
		if (diff > 180.0f)
			diff -= 360.0f;
		if (diff < -180.0f)
			diff += 360.0f;
		if (fabsf(diff) > 90.0f) {
			api().player->set_yaw(want);
			v().turnedAt = core().time();
		}
	}

	// Holding on to something: a fence or ladder, a bar, a wire (his status's flags 0x10000000
	// and 0x20000000), or a net on a ceiling (0x200000: under Pianta Village). His body is left
	// to the game there: turned to the view, he was turned off the fence and let go, or walked
	// off the ceiling's net the wrong way.
	// on his belly or diving (a belly slide, a dive, the jump out of one): his body turns over
	// round the camera, so his legs would come into the view; all of him left out then
	bool lyingDown() const
	{
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m))
			return false;
		const uint32_t st = m.status;
		// belly slides, dives and slips (0x800000), the roll up out of a belly slide (0x2000889)
		const bool lying = (st & 0x800000u) != 0 || st == 0x891u || st == 0x2000889u;
		if (st != v().lastStatus && v().enabled && v().viewIsOurs) { // which ones, in the log (to find any still missing)
			const_cast<View&>(v()).lastStatus = st;
			fprintf(stderr, "[mod:first-person] status 0x%X%s\n", (unsigned)st, lying ? " (his body left out)" : "");
		}
		return lying;
	}
	bool holding() const
	{
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m))
			return false;
		return (m.status & 0x30000000u) != 0 || (m.status & 0x200000u) != 0;
	}
	// ...and of those, the ones the game moves by the stick as it is (up climbs, left and right
	// along it): fences and ladders, bars, wires. On a ceiling's net he goes where the stick
	// points, as walking (the view turns the stick there, as on the ground).
	bool rawStick() const
	{
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		return api().player->get(&m) && (m.status & 0x30000000u) != 0;
	}

	// FLUDD's spray nozzle out, or Yoshi: R sprays (other nozzles use R their own way)
	bool spraysWithR() const
	{
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m))
			return false;
		return m.on_yoshi || (m.has_fludd && m.current_nozzle == 0);
	}

	// With his body showing, his body always faces where the view looks, so
	// looking down he walks, runs and jumps ahead of you, whichever way the
	// stick moves him (sideways and back too), never turned across the view
	// (on Yoshi too, Yoshi with him; not swimming: there he turns as the game
	// turns him).
	bool bodyFacesView() const
	{
		if (!v().enabled || !v().viewIsOurs || !v().haveYaw || modelMode() != MODEL_BODY || holding())
			return false;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		return api().player->get(&m) && !m.in_water;
	}

	// FLUDD's underwater nozzle (the jet pack under the sea): the stick steers it and R pushes him,
	// FLUDD on his back as ever: not aimed by the view, nor held in it
	bool underwaterNozzle() const
	{
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		return api().player->get(&m) && m.has_fludd && m.current_nozzle == 2;
	}

	// While R is held (FLUDD sprays, Yoshi spits), Mario's body and FLUDD
	// face where the view looks, so the water goes straight ahead, while the
	// stick still walks him any way: sideways too (player->set_aim, mod API
	// 1.5 beta 8). Standing still, he turns that way as well.
	void aimFludd()
	{
		const SmsPad p = pad(cur);
		View& w        = v();
		const bool want = w.enabled && w.viewIsOurs && w.haveYaw && setting("aim_fludd", 1)
		                  && ((p.held & SMS_PAD_R) || p.trigger_r >= 0.1f) && !holding() && !underwaterNozzle();
		const bool hasAim = SMS_API_HAS(api().player, set_aim);
		if (!want && hasAim && bodyFacesView()) {  // his body the view's way, FLUDD level
			api().player->set_aim(1, w.yaw, 0.0f);
			w.aiming = true;
			return;
		}
		if (!want) {
			if (w.aiming && hasAim)
				api().player->set_aim(0, 0.0f, 0.0f);
			w.aiming = false;
			return;
		}
		if (hasAim) {
			api().player->set_aim(1, w.yaw, w.pitch);
			w.aiming = true;
		}
		const bool still = fabsf(p.stick_x) < 0.2f && fabsf(p.stick_y) < 0.2f;
		if (SMS_API_HAS(api().player, set_yaw) && (still || !hasAim)) {
			api().player->set_yaw(w.yaw);
		} else if (!hasAim && !toldNoYaw) {
			warn("this game cannot turn Mario (player->set_aim, mod API 1.5 beta 8): FLUDD sprays the way he faces");
			toldNoYaw = true;
		}
	}

	int load() override
	{
		toggleKey = bindKey("toggle", "V");
		preset.addRow();
		preset.optionChoice("button", "Controller toggle button", std::vector<const char*>(kButtons, kButtons + 5), 0);
		preset.optionNumber("fov", "Field of view (degrees, up and down)", 85, 40, 100, 5);
		preset.optionNumber("eye_height", "Eye height", 120, 60, 200, 5);
		preset.optionNumber("turn_speed", "C-stick turn speed", 210, 30, 480, 15);
		preset.optionNumber("tilt_speed", "C-stick tilt speed", 200, 0, 240, 10);
		preset.optionBool("invert_x", "Invert left and right", true);
		preset.optionBool("invert", "Invert up and down", true);
		preset.optionBool("mouse_look", "Mouse look (F10 captures the mouse)", true);
		preset.optionNumber("mouse_speed", "Mouse look speed (percent)", 100, 10, 400, 10);
		preset.optionBool("mouse_invert", "Invert mouse up and down", false);
		preset.optionChoice("model", "Mario in first person", std::vector<const char*>(kModels, kModels + 2), MODEL_BODY);
		preset.optionBool("aim_fludd", "FLUDD sprays where you look", true);
		preset.optionBool("run_spray", "R all the way down: keep moving while spraying", true);
		preset.optionBool("fludd_view", "FLUDD in view, held like a gun", true);
		preset.optionNumber("fludd_distance", "FLUDD distance from the camera", 55, 30, 120, 5);
		preset.optionBool("crosshair", "Crosshair while FLUDD is in view", true);
		preset.optionChoice("crosshair_color", "Crosshair color", std::vector<const char*>(kCrossNames, kCrossNames + kCrossCount), 8);
		preset.check();

		// no crosshair from a stage's loading to its opening's end (its camera cutscene, the
		// episode's name, "Go!"), nor once Mario has lost his life
		on<SmsEvStage>(SMS_EV_STAGE_LOADED, [this](SmsEvStage& e) {
			noCrosshair = true;
			onCoaster   = e.area == 0x3A && (e.episode == 0 || e.episode == 1); // Mecha-Bowser's roller coaster
		});
		on<SmsEvStage>(SMS_EV_STAGE_START, [this](SmsEvStage&) { noCrosshair = false; });
		on<SmsEvPlayerDeath>(SMS_EV_PLAYER_DEATH, [this](SmsEvPlayerDeath&) { noCrosshair = true; });
		on<SmsEvKey>(SMS_EV_KEY, [this](SmsEvKey& k) {
			if (k.scancode != toggleKey)
				return SMS_CONTINUE;
			if (k.down && !k.repeat) { // the keyboard's: player 1's
				For w(*this, 0);
				flipByHand();
			}
			return SMS_CANCEL;
		});

		// each player's controller's button, once a frame
		on<SmsEvFrame>(SMS_EV_FRAME, [this](SmsEvFrame& f) {
			if (f.mode != SMS_MODE_STAGE)
				return;
			const int b = setting("button", 0);
			const uint16_t bit = b >= 0 && b < 5 ? kButtonBits[b] : 0;
			for (int pl = 0; pl < players(); pl++) {
				For w(*this, pl);
				if (bit && (pad(pl).pressed & bit))
					flipByHand();
				zoomFirstPerson(); // a zoom from third person (the FLUDD Weapons mod)
			}
		});

		// the C-stick turns and tilts the view; L looks the way Mario faces (each player's own)
		on<SmsEvTick>(SMS_EV_TICK, [this](SmsEvTick& t) {
			for (int pl = 0; pl < players(); pl++) {
				For w(*this, pl);
				tick(t, pl);
			}
		});

		// each player's controller (co-op: player 2's the second)
		on<SmsEvPadRead>(SMS_EV_PAD_READ, [this](SmsEvPadRead& r) {
			if (!r.pads)
				return;
			for (int pl = 0; pl < players(); pl++) {
				For w(*this, pl);
				padRead(r.pads[pl]);
			}
		});

		// each player's camera (co-op: the game runs as its player then)
		on<SmsEvCamera>(SMS_EV_CAMERA, [this](SmsEvCamera& c) {
			cur = playerNow();
			camera(c);
			cur = 0;
		});

		// his model out of the view (the camera is inside his head)
		// (the game asks twice a frame: FLUDD, then Mario's own models; co-op: in his own view)
		on<SmsEvObject>(SMS_EV_HIDE_PLAYER_MODEL, [this](SmsEvObject&) {
			cur = playerNow();
			const bool hide = v().enabled && v().viewIsOurs && (modelMode() == MODEL_HIDDEN || lyingDown());
			cur = 0;
			return hide ? SMS_CANCEL : SMS_CONTINUE;
		});

		on<SmsEvDraw>(SMS_EV_DRAW, [this](SmsEvDraw& d) {
			if (d.mode == SMS_MODE_STAGE)
				for (int pl = 0; pl < players(); pl++) {
					For w(*this, pl);
					drawCrosshair();
				}
			if (d.mode != SMS_MODE_STAGE || core().time() > toastUntil)
				return;
			char text[64];
			snprintf(text, sizeof text, "%s%s", players() < 2 ? "" : toastPlayer ? "Player 2 " : "Player 1 ", toastText);
			const float w = draw().text_width(14, text) + 20;
			SmsRect r;
			if (!draw().hud_slot(handle(), SMS_ANCHOR_TOP, w, 26, &r))
				return;
			draw().rect(r.x, r.y, r.w, r.h, SMS_RGBA(0, 0, 0, 170));
			draw().text(r.x + 10, r.y + 6, 14, SMS_RGB(255, 255, 255), SMS_TEXT_SHADOW, text);
		});

		info("loaded; %s or the controller's button switches first person", input().key_name(toggleKey));
		return 0;
	}

	// A tick: player `pl`'s controller turns and tilts their view; L looks the way he faces
	void tick(const SmsEvTick& t, int pl)
	{
		View& w = v();
		if (!w.enabled || !w.viewIsOurs)
			return;
		const SmsPad p = pad(pl);
		const float dt = 1.0f / (t.ticks_per_second > 0 ? (float)t.ticks_per_second : 30.0f);
		const float x  = setting("invert_x", 1) ? -p.cstick_x : p.cstick_x;
		const float y  = setting("invert", 1) ? -p.cstick_y : p.cstick_y;
		// zoomed, the view turns as much slower as it is narrower, so the aim stays steady
		const float slow = w.fovNow > 0.0f && setting("fov", 85) > 0 ? w.fovNow / (float)setting("fov", 85) : 1.0f;
		if (fabsf(x) > 0.2f) // right turns right: the yaw goes down
			w.yaw -= x * (float)setting("turn_speed", 210) * slow * dt;
		if (fabsf(y) > 0.2f)
			w.pitch = clampf(w.pitch + y * (float)setting("tilt_speed", 200) * slow * dt, -75.0f, 75.0f);
		mouseLook(slow);
		aimFludd();
		faceView(p);
		if (p.pressed & SMS_PAD_L) {
			SmsPlayer m = SmsPlayer();
			m.size      = sizeof m;
			if (api().player->get(&m))
				w.yaw = m.yaw;
			w.pitch = 0;
		}
	}

	// The game moves Mario relative to its own camera (stick up runs the way it looks). Turn the
	// stick by the difference, so up runs the way the view looks; and keep the C-stick from the
	// game's camera. `s`: this player's controller.
	void padRead(SmsPadStatus& s)
	{
		const View& w = v();
		if (!w.enabled || !w.viewIsOurs || !w.haveYaw || !w.haveGameYaw)
			return;
		const float yaw = w.yaw, gameYaw = w.gameYaw;
		s.cstick_x = s.cstick_y = 0;
		// On a fence, a ladder, a bar or a wire: the game takes the stick as it is, up
		// climbing up, left and right along it, the sides swapped when its camera looks
		// at his front. Left as it is, only swapped when the game's camera and the view
		// disagree on that, so that right is the view's right.
		SmsPlayer hm = SmsPlayer();
		hm.size      = sizeof hm;
		if (rawStick() && api().player->get(&hm)) {
			auto apart = [](float a, float b) {
				float d = fmodf(a - b, 360.0f);
				if (d > 180.0f)
					d -= 360.0f;
				if (d < -180.0f)
					d += 360.0f;
				return fabsf(d) > 90.0f;
			};
			if (apart(gameYaw, hm.yaw) != apart(yaw, hm.yaw))
				s.stick_x = (int8_t)(s.stick_x == -128 ? 127 : -s.stick_x);
			return;
		}
		// the stick's angle, clockwise from up, gives the world yaw
		// (the camera's) minus it: so turn it clockwise by game - ours
		const float d  = (gameYaw - yaw) * kPi / 180.0f;
		const float cd = cosf(d), sd = sinf(d);
		const float a = s.stick_x, b = s.stick_y;
		if (!holding()) {  // on a ceiling's net the game turns him itself
			noHardTurn(a, b);
			sideFlip((s.buttons & SMS_PAD_A) != 0);
		}
		const float na = a * cd + b * sd, nb = -a * sd + b * cd;
		s.stick_x = (int8_t)clampf(roundf(na), -127.0f, 127.0f);
		s.stick_y = (int8_t)clampf(roundf(nb), -127.0f, 127.0f);
		// R all the way down plants him to aim (the stance): in first person
		// the view aims, so keep him moving, spraying at full strength. The
		// stance comes from R's click; its analog part is the strength.
		// standing still it stays: R all the way down crouches him, and A then back flips
		const bool moving = a * a + b * b >= 25.0f * 25.0f;
		// (not on Pinna Park's roller coaster: R's click fires its rocket)
		if (moving && !onCoaster && (s.buttons & SMS_PAD_R) && setting("run_spray", 1) && spraysWithR()) {
			s.buttons &= (uint16_t)~SMS_PAD_R;
			if (s.trigger_r < 255)
				s.trigger_r = 255;
		}
	}

	// A camera as the game places it: at his eyes while this player's first person is on
	void camera(SmsEvCamera& c)
	{
		View& w = v();
		w.viewIsOurs = false;
		if (!active()) {
			w.haveEye = false;
			aimFludd(); // lets his body go, if it was turned
			showHead();
			holdFludd();
			return;
		}
		w.viewIsOurs = true;
		SmsPlayer p = SmsPlayer();
		p.size      = sizeof p;
		api().player->get(&p);

		// which way the game camera looks, across the ground
		const float gx = c.target.x - c.position.x, gz = c.target.z - c.position.z;
		if (gx * gx + gz * gz > 0.0001f) {
			w.gameYaw     = atan2f(gx, gz) * 180.0f / kPi;
			w.haveGameYaw = true;
		}
		if (!w.haveYaw) { // just switched on: look the way the game camera did
			w.yaw     = w.haveGameYaw ? w.gameYaw : p.yaw;
			w.haveYaw = true;
		}
		w.yaw = fmodf(w.yaw, 360.0f);
		const float dx = sinf(w.yaw * kPi / 180.0f), dz = cosf(w.yaw * kPi / 180.0f);

		// the eyes: above his feet (higher on Yoshi), a little smoothed up and down
		float height  = (float)setting("eye_height", 120) + (p.on_yoshi ? 80.0f : 0.0f);
		const float y = p.position.y + height;
		w.eyeY        = w.haveEye ? w.eyeY + (y - w.eyeY) * 0.5f : y;
		w.haveEye     = true;

		const float tilt = w.pitch * kPi / 180.0f;
		const float cp = cosf(tilt), sp = sinf(tilt);
		c.position.x = p.position.x + dx * 10.0f;
		c.position.y = w.eyeY;
		c.position.z = p.position.z + dz * 10.0f;
		c.target.x   = c.position.x + dx * cp * 1000.0f;
		c.target.y   = c.position.y + sp * 1000.0f;
		c.target.z   = c.position.z + dz * cp * 1000.0f;
		// a zoom (the FLUDD Weapons mod's Rifle): the view narrows (smoothly) while it is on
		const float wide = (float)setting("fov", 85);
		const float zoom = weaponsState().zoom_fovy;
		const float want = zoom > 0.0f ? zoom : wide;
		w.fovNow         = w.fovNow > 0.0f ? w.fovNow + (want - w.fovNow) * 0.25f : want;
		c.fovy           = w.fovNow;
		aimFludd(); // his model too, before it is drawn
		showHead();
		holdFludd();
	}
};

SMS_MOD_CLASS(FirstPerson)
