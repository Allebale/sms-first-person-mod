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
#include <sms_mod.hpp>

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

// FLUDD held in view like a gun: api->held_fludd (Allebale's First Person and Remastered builds;
// not in Chase's own yet, where SMS_API_HAS says no). Taken only if its table says so (its magic).
const SmsHeldFluddApi* heldFluddApi(const SmsApi& api)
{
	if (api.size < offsetof(SmsApi, held_fludd) + sizeof(void*) || !api.held_fludd) // SMS_API_HAS
		return nullptr;
	const SmsHeldFluddApi* h = api.held_fludd;
	return h->size >= offsetof(SmsHeldFluddApi, set) + sizeof(void*) && h->magic == SMS_HELD_FLUDD_MAGIC && h->set ? h
	                                                                                                              : nullptr;
}
// the spray weapons (set_weapon's SMS_SPRAY_*), D-pad left and right
const char* const kWeaponNames[] = {"Normal spray", "Shotgun", "Rifle", "Bazooka"};
const int kWeaponCount           = 4;

} // namespace

struct FirstPerson : sms::Mod {
	using sms::Mod::Mod;

	bool enabled   = false;
	int toggleKey  = -1;
	float pitch    = 0;     // degrees, up positive: the C-stick's tilt
	float eyeY     = 0;     // the eye's height, smoothed
	bool haveEye   = false;
	bool viewIsOurs = false; // the camera was put at his eyes this tick
	// Where the view looks across the ground, and where the game's camera does
	// (degrees: 0 is +Z, 90 is +X). The C-stick turns ours; the game's camera
	// still follows Mario, but no longer turns ours, so walls cannot stop it.
	float yaw = 0, gameYaw = 0;
	bool haveYaw = false, haveGameYaw = false;
	bool toldNoYaw = false;
	double toastUntil = 0;
	const char* toastText = "";

	void toast(const char* text)
	{
		toastText  = text;
		toastUntil = core().time() + 1.5;
	}

	void flip()
	{
		enabled = !enabled;
		zoomed  = false;
		haveYaw = false; // from the game camera again when it starts
		if (!enabled) {
			viewIsOurs = false;
			aimFludd(); // lets his body go, if it was turned
			showHead();
			holdFludd();
		}
		pitch   = 0;
		haveEye = false;
		toast(enabled ? "First person" : "Third person");
		info("%s", enabled ? "on" : "off");
	}

	// Whether the view is ours now: on, Mario there, no cutscene or talk.
	bool active() const
	{
		if (!enabled)
			return false;
		if (SMS_API_HAS(&api(), world) && api().world && SMS_API_HAS(api().world, cutscene_playing) &&
		    api().world->cutscene_playing())
			return false;
		SmsPlayer p = SmsPlayer();
		p.size      = sizeof p;
		return api().player->get(&p) != 0;
	}

	bool aiming = false; // set_aim is on
	bool headHidden = false; // set_hidden_parts hides his head and FLUDD

	bool hasHeadApi() const { return SMS_API_HAS(api().player, set_hidden_parts); }

	// What of him shows: his body without his head needs player->set_hidden_parts
	// (mod API 1.5 beta 8); without it, he is hidden.
	int modelMode() const
	{
		const int m = setting("model", MODEL_BODY) == MODEL_HIDDEN ? MODEL_HIDDEN : MODEL_BODY;
		return m == MODEL_BODY && !hasHeadApi() ? MODEL_HIDDEN : m;
	}

	// FLUDD in view, held like a gun (the game moves its own model there), while the view is his
	bool fluddHeld = false;
	void holdFludd()
	{
		const SmsHeldFluddApi* h = heldFluddApi(api());
		// with weapons on, the gun shows while the spray nozzle is out, the normal spray too (not
		// the hover, rocket or turbo nozzle, nor on Yoshi); without them, always
		const bool shown = !setting("weapons", 0) || sprayNozzleOut();
		const bool want  = enabled && viewIsOurs && setting("fludd_view", 0) && !holding() && shown;
		if (!h) {
			if (want && !toldNoHeld) {
				warn("this game cannot show FLUDD in view (it needs a Remastered or First Person beta build)");
				toldNoHeld = true;
			}
			return;
		}
		const bool weapons = h->size >= offsetof(SmsHeldFluddApi, set_weapon) + sizeof(void*) && h->set_weapon;
		if (want) {
			const float d = (float)setting("fludd_distance", 55);
			h->set(1, d * 0.42f, d * 0.40f, d, 0.55f, 1200.0f);
			fluddHeld = true;
			if (weapons)
				h->set_weapon(weaponNow(), (setting("weapons_defeat", 0) ? 1 : 0) | (rHeld ? 2 : 0));
			sendWeaponStats(h);
		} else if (fluddHeld) {
			h->set(0, 0, 0, 0, 1, 0);
			if (weapons)
				h->set_weapon(0, 0);
			fluddHeld = false;
		}
	}
	bool toldNoHeld = false;
	// The weapons' numbers from the menu, to the game (newer builds: set_weapon_stats).
	void sendWeaponStats(const SmsHeldFluddApi* h)
	{
		if (h->size < offsetof(SmsHeldFluddApi, set_weapon_stats) + sizeof(void*) || !h->set_weapon_stats)
			return;
		static const char* const kKeys[] = {"shotgun", "rifle", "bazooka"};
		for (int w = SMS_SPRAY_SHOTGUN; w <= SMS_SPRAY_BAZOOKA; w++) {
			const std::string k = kKeys[w - 1];
			SmsSprayStats st    = SmsSprayStats();
			st.size             = sizeof st;
			st.every            = (float)setting((k + "_rate").c_str(), 10) / 10.0f;
			st.push             = (float)setting((k + "_push").c_str(), 100) / 100.0f;
			st.range            = (float)setting((k + "_range").c_str(), 100) / 100.0f;
			st.to_defeat        = setting((k + "_defeat").c_str(), 1);
			st.water            = setting((k + "_water").c_str(), 12);
			st.hit_size         = (float)setting("rifle_hit", 100) / 100.0f;
			st.burst            = (float)setting("bazooka_burst", 100) / 100.0f;
			h->set_weapon_stats(w, &st);
		}
	}

	bool zoomed   = false; // the rifle's zoom (D-pad up)
	bool upWasDown = false, rightWasDown = false, leftWasDown = false;
	float fovNow  = 0.0f;  // the view's field of view now (eased to the zoom and back)

	// D-pad up: the rifle's zoom on and off, while it is chosen and in view (not when D-pad up is
	// the controller's first person toggle)
	void toggleZoom(const SmsPad& p)
	{
		const int b      = setting("button", 0);
		const uint16_t t = b >= 0 && b < 5 ? kButtonBits[b] : 0;
		if (zoomed && (weaponNow() != SMS_SPRAY_RIFLE || !fluddHeld))
			zoomed = false;
		// once a press: the moment D-pad up goes down (its "pressed" can come again while held)
		const bool down  = (p.held & SMS_PAD_UP) != 0;
		const bool fresh = down && !upWasDown;
		upWasDown        = down;
		if (!fresh || t == SMS_PAD_UP || weaponNow() != SMS_SPRAY_RIFLE || !fluddHeld)
			return;
		zoomed = !zoomed;
		info("rifle zoom %s", zoomed ? "on" : "off");
	}

	int weapon = 0; // the spray weapon chosen (kWeaponNames)
	bool rHeld = false; // R, as the controller has it (kept from the game while a weapon fires)

	bool sprayNozzleOut() const
	{
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		return api().player->get(&m) && m.has_fludd && !m.on_yoshi && m.current_nozzle == 0;
	}

	// the weapon that fires now: with weapons on, FLUDD held and its spray nozzle out
	int weaponNow() const
	{
		if (!enabled || !viewIsOurs || !setting("fludd_view", 0) || !setting("weapons", 0) || weapon == 0 || holding())
			return 0;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m) || !m.has_fludd || m.on_yoshi || m.current_nozzle != 0)
			return 0;
		return weapon;
	}
	bool toldWeapons = false;

	// D-pad left and right: the spray weapon, while FLUDD is in view (not a button the
	// controller toggle uses)
	void switchWeapon(const SmsPad& p)
	{
		if (!enabled || !viewIsOurs || !setting("fludd_view", 0) || !setting("weapons", 0))
			return;
		if (!toldWeapons) {
			const SmsHeldFluddApi* h = heldFluddApi(api());
			info("weapons on: D-pad left and right switch them%s",
			     h && h->size >= offsetof(SmsHeldFluddApi, set_weapon) + sizeof(void*) && h->set_weapon
			         ? "" : " (this game cannot fire them: it needs a newer Remastered or First Person beta build)");
			toldWeapons = true;
		}
		const int b      = setting("button", 0);
		const uint16_t t = b >= 0 && b < 5 ? kButtonBits[b] : 0;
		int step         = 0;
		// once a press each: the moment the button goes down
		const bool right = (p.held & SMS_PAD_RIGHT) != 0, left = (p.held & SMS_PAD_LEFT) != 0;
		const bool newRight = right && !rightWasDown, newLeft = left && !leftWasDown;
		rightWasDown = right;
		leftWasDown  = left;
		if (newRight && t != SMS_PAD_RIGHT)
			step = 1;
		else if (newLeft && t != SMS_PAD_LEFT)
			step = kWeaponCount - 1;
		if (!step)
			return;
		weapon = (weapon + step) % kWeaponCount;
		zoomed = false;
		toast(kWeaponNames[weapon]);
		info("weapon: %s", kWeaponNames[weapon]);
	}

	// A small crosshair in the middle of the view while FLUDD is held there (the spray and
	// underwater nozzles, off Yoshi): where its water goes
	void drawCrosshair()
	{
		if (!fluddHeld || !viewIsOurs || !setting("crosshair", 0))
			return;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m) || !m.has_fludd || m.on_yoshi || (m.current_nozzle != 0 && m.current_nozzle != 2))
			return;
		float w = 0, h = 0;
		draw().canvas(&w, &h);
		const float x = w * 0.5f, y = h * 0.5f, r = h * 0.009f;
		const int ci     = setting("crosshair_color", 0);
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
		const bool want = enabled && viewIsOurs && modelMode() == MODEL_BODY;
		if (want != headHidden && hasHeadApi()) {
			api().player->set_hidden_parts(want ? SMS_PLAYER_PART_HEAD | SMS_PLAYER_PART_FLUDD : 0);
			headHidden = want;
		}
	}

	// Standing still with his body showing, he turns to face the view, so
	// looking down shows him the right way round (and he sets off ahead).
	void faceView(const SmsPad& p)
	{
		if (modelMode() == MODEL_HIDDEN || aiming || !SMS_API_HAS(api().player, set_yaw))
			return;
		if (fabsf(p.stick_x) > 0.2f || fabsf(p.stick_y) > 0.2f)
			return;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (api().player->get(&m) && m.on_ground && !m.in_water && fabsf(m.forward_speed) < 1.0f)
			api().player->set_yaw(yaw);
	}

	// No hard turns: running, a stick the other way would have him skid round
	// (his facing far from where the stick points, the game's turnaround). In
	// first person the view turns, not him, so he turns that way at once, as
	// the game would after the skid, and runs on. a, b: the stick as read
	// (up the way the view looks).
	double turnedAt = -1; // when noHardTurn last turned him (core().time())
	bool aWas       = false;

	// A side flip comes out of the skid (A during the turnaround); without the
	// skid, A just after the turn starts it all the same.
	void sideFlip(bool aDown)
	{
		const bool pressed = aDown && !aWas;
		aWas               = aDown;
		if (!pressed || turnedAt < 0 || core().time() - turnedAt > 0.3)
			return;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (api().player->get(&m) && m.on_ground && !m.in_water && !m.on_yoshi) {
			api().player->set_status(0x887, 0); // MARIO_STATUS_U_TURN_JUMP: the side flip
			turnedAt = -1;
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
		const float want = yaw - atan2f(a, b) * 180.0f / kPi;
		float diff = fmodf(want - m.yaw, 360.0f);
		if (diff > 180.0f)
			diff -= 360.0f;
		if (diff < -180.0f)
			diff += 360.0f;
		if (fabsf(diff) > 90.0f) {
			api().player->set_yaw(want);
			turnedAt = core().time();
		}
	}

	// Holding on to something: a fence or ladder, a bar, a wire (his status's flags 0x10000000
	// and 0x20000000), or a net on a ceiling (0x200000: under Pianta Village). His body is left
	// to the game there: turned to the view, he was turned off the fence and let go, or walked
	// off the ceiling's net the wrong way.
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
		if (!enabled || !viewIsOurs || !haveYaw || modelMode() != MODEL_BODY || holding())
			return false;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		return api().player->get(&m) && !m.in_water;
	}

	// While R is held (FLUDD sprays, Yoshi spits), Mario's body and FLUDD
	// face where the view looks, so the water goes straight ahead, while the
	// stick still walks him any way: sideways too (player->set_aim, mod API
	// 1.5 beta 8). Standing still, he turns that way as well.
	void aimFludd()
	{
		const SmsPad p = pad(0);
		const bool want = enabled && viewIsOurs && haveYaw && setting("aim_fludd", 1)
		                  && ((p.held & SMS_PAD_R) || p.trigger_r >= 0.1f) && !holding();
		const bool hasAim = SMS_API_HAS(api().player, set_aim);
		if (!want && hasAim && bodyFacesView()) {  // his body the view's way, FLUDD level
			api().player->set_aim(1, yaw, 0.0f);
			aiming = true;
			return;
		}
		if (!want) {
			if (aiming && hasAim)
				api().player->set_aim(0, 0.0f, 0.0f);
			aiming = false;
			return;
		}
		if (hasAim) {
			api().player->set_aim(1, yaw, pitch);
			aiming = true;
		}
		const bool still = fabsf(p.stick_x) < 0.2f && fabsf(p.stick_y) < 0.2f;
		if (SMS_API_HAS(api().player, set_yaw) && (still || !hasAim)) {
			api().player->set_yaw(yaw);
		} else if (!hasAim && !toldNoYaw) {
			warn("this game cannot turn Mario (player->set_aim, mod API 1.5 beta 8): FLUDD sprays the way he faces");
			toldNoYaw = true;
		}
	}

	int load() override
	{
		toggleKey = bindKey("toggle", "V");
		optionChoice("button", "Controller toggle button", std::vector<const char*>(kButtons, kButtons + 5), 0);
		optionNumber("fov", "Field of view (degrees, up and down)", 60, 40, 100, 5);
		optionNumber("eye_height", "Eye height", 130, 60, 200, 5);
		optionNumber("turn_speed", "C-stick turn speed", 180, 30, 480, 15);
		optionNumber("tilt_speed", "C-stick tilt speed", 90, 0, 240, 10);
		optionBool("invert_x", "Invert left and right", false);
		optionBool("invert", "Invert up and down", false);
		optionChoice("model", "Mario in first person", std::vector<const char*>(kModels, kModels + 2), MODEL_BODY);
		optionBool("aim_fludd", "FLUDD sprays where you look", true);
		optionBool("run_spray", "R all the way down: keep moving while spraying", true);
		optionBool("fludd_view", "FLUDD in view, held like a gun (Allebale's builds)", false);
		optionNumber("fludd_distance", "FLUDD in view distance", 55, 30, 120, 5);
		optionBool("crosshair", "Crosshair while FLUDD is in view", false);
		optionBool("weapons", "FLUDD weapons, D-pad left and right (Allebale's builds)", false);
		optionBool("weapons_defeat", "FLUDD weapons can defeat enemies", false);
		// each weapon's numbers (the game's own by default)
		optionNumber("shotgun_rate", "Shotgun fire rate (tenths of a second)", 10, 1, 100, 1);
		optionNumber("shotgun_push", "Shotgun push (percent)", 100, 0, 400, 10);
		optionNumber("shotgun_range", "Shotgun range (percent)", 100, 20, 400, 10);
		optionNumber("shotgun_defeat", "Shotgun shots to defeat", 2, 1, 10, 1);
		optionNumber("shotgun_water", "Shotgun water per shot", 24, 0, 200, 2);
		optionNumber("rifle_rate", "Rifle fire rate (tenths of a second)", 10, 1, 100, 1);
		optionNumber("rifle_push", "Rifle push (percent)", 100, 0, 400, 10);
		optionNumber("rifle_range", "Rifle range (percent)", 100, 10, 100, 10);
		optionNumber("rifle_defeat", "Rifle shots to defeat", 1, 1, 10, 1);
		optionNumber("rifle_water", "Rifle water per shot", 12, 0, 200, 2);
		optionNumber("rifle_hit", "Rifle hit size (percent)", 100, 0, 400, 10);
		optionNumber("rifle_zoom", "Rifle zoom, D-pad up (field of view)", 25, 5, 60, 5);
		optionNumber("bazooka_rate", "Bazooka fire rate (tenths of a second)", 50, 1, 200, 5);
		optionNumber("bazooka_push", "Bazooka push (percent)", 100, 0, 400, 10);
		optionNumber("bazooka_range", "Bazooka range (percent)", 100, 10, 100, 10);
		optionNumber("bazooka_defeat", "Bazooka shots to defeat", 1, 1, 10, 1);
		optionNumber("bazooka_water", "Bazooka water per shot", 60, 0, 300, 5);
		optionNumber("bazooka_burst", "Bazooka burst size (percent)", 100, 20, 400, 10);
		optionChoice("crosshair_color", "Crosshair color", std::vector<const char*>(kCrossNames, kCrossNames + kCrossCount), 0);

		on<SmsEvKey>(SMS_EV_KEY, [this](SmsEvKey& k) {
			if (k.scancode != toggleKey)
				return SMS_CONTINUE;
			if (k.down && !k.repeat)
				flip();
			return SMS_CANCEL;
		});

		// the controller's button, once a frame
		on<SmsEvFrame>(SMS_EV_FRAME, [this](SmsEvFrame& f) {
			if (f.mode != SMS_MODE_STAGE)
				return;
			const int b = setting("button", 0);
			const uint16_t bit = b >= 0 && b < 5 ? kButtonBits[b] : 0;
			if (bit && (pad(0).pressed & bit))
				flip();
			switchWeapon(pad(0));
			toggleZoom(pad(0));
		});

		// the C-stick turns and tilts the view; L looks the way Mario faces
		on<SmsEvTick>(SMS_EV_TICK, [this](SmsEvTick& t) {
			if (!enabled || !viewIsOurs)
				return;
			const SmsPad p = pad(0);
			const float dt = 1.0f / (t.ticks_per_second > 0 ? (float)t.ticks_per_second : 30.0f);
			const float x  = setting("invert_x", 0) ? -p.cstick_x : p.cstick_x;
			const float y  = setting("invert", 0) ? -p.cstick_y : p.cstick_y;
			// zoomed, the view turns as much slower as it is narrower, so the aim stays steady
			const float slow = fovNow > 0.0f && setting("fov", 60) > 0 ? fovNow / (float)setting("fov", 60) : 1.0f;
			if (fabsf(x) > 0.2f) // right turns right: the yaw goes down
				yaw -= x * (float)setting("turn_speed", 180) * slow * dt;
			if (fabsf(y) > 0.2f)
				pitch = clampf(pitch + y * (float)setting("tilt_speed", 90) * slow * dt, -75.0f, 75.0f);
			aimFludd();
			faceView(p);
			if (p.pressed & SMS_PAD_L) {
				SmsPlayer m = SmsPlayer();
				m.size      = sizeof m;
				if (api().player->get(&m))
					yaw = m.yaw;
				pitch = 0;
			}
		});

		// The game moves Mario relative to its own camera (stick up runs the
		// way it looks). Turn the stick by the difference, so up runs the way
		// the view looks; and keep the C-stick from the game's camera.
		on<SmsEvPadRead>(SMS_EV_PAD_READ, [this](SmsEvPadRead& r) {
			if (!enabled || !viewIsOurs || !haveYaw || !haveGameYaw || !r.pads)
				return;
			SmsPadStatus& s = r.pads[0];
			s.cstick_x = s.cstick_y = 0;
			// a weapon chosen: R is the weapon's, not the game's (no stream, pump or its sound);
			// the game fires the weapon each time it is ready while R is held
			rHeld = (s.buttons & SMS_PAD_R) || s.trigger_r >= 60;
			if (weaponNow() != 0) {
				s.buttons &= (uint16_t)~SMS_PAD_R;
				s.trigger_r = 0;
			}
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
			if (moving && (s.buttons & SMS_PAD_R) && setting("run_spray", 1) && spraysWithR()) {
				s.buttons &= (uint16_t)~SMS_PAD_R;
				if (s.trigger_r < 255)
					s.trigger_r = 255;
			}
		});

		on<SmsEvCamera>(SMS_EV_CAMERA, [this](SmsEvCamera& c) {
			viewIsOurs = false;
			if (!active()) {
				haveEye = false;
				aimFludd(); // lets his body go, if it was turned
				showHead();
				holdFludd();
				return;
			}
			viewIsOurs = true;
			SmsPlayer p = SmsPlayer();
			p.size      = sizeof p;
			api().player->get(&p);

			// which way the game camera looks, across the ground
			const float gx = c.target.x - c.position.x, gz = c.target.z - c.position.z;
			if (gx * gx + gz * gz > 0.0001f) {
				gameYaw     = atan2f(gx, gz) * 180.0f / kPi;
				haveGameYaw = true;
			}
			if (!haveYaw) { // just switched on: look the way the game camera did
				yaw     = haveGameYaw ? gameYaw : p.yaw;
				haveYaw = true;
			}
			yaw = fmodf(yaw, 360.0f);
			const float dx = sinf(yaw * kPi / 180.0f), dz = cosf(yaw * kPi / 180.0f);

			// the eyes: above his feet (higher on Yoshi), a little smoothed up and down
			float height  = (float)setting("eye_height", 130) + (p.on_yoshi ? 80.0f : 0.0f);
			const float y = p.position.y + height;
			eyeY          = haveEye ? eyeY + (y - eyeY) * 0.5f : y;
			haveEye       = true;

			const float tilt = pitch * kPi / 180.0f;
			const float cp = cosf(tilt), sp = sinf(tilt);
			c.position.x = p.position.x + dx * 10.0f;
			c.position.y = eyeY;
			c.position.z = p.position.z + dz * 10.0f;
			c.target.x   = c.position.x + dx * cp * 1000.0f;
			c.target.y   = c.position.y + sp * 1000.0f;
			c.target.z   = c.position.z + dz * cp * 1000.0f;
			// the rifle's zoom: the view narrows (smoothly) while it is on
			const float wide = (float)setting("fov", 60);
			const float want = zoomed ? (float)setting("rifle_zoom", 25) : wide;
			fovNow           = fovNow > 0.0f ? fovNow + (want - fovNow) * 0.25f : want;
			c.fovy           = fovNow;
			aimFludd(); // his model too, before it is drawn
			showHead();
			holdFludd();
		});

		// his model out of the view (the camera is inside his head)
		// (the game asks twice a frame: FLUDD, then Mario's own models)
		on<SmsEvObject>(SMS_EV_HIDE_PLAYER_MODEL, [this](SmsEvObject&) {
			return enabled && viewIsOurs && modelMode() == MODEL_HIDDEN ? SMS_CANCEL : SMS_CONTINUE;
		});


		on<SmsEvDraw>(SMS_EV_DRAW, [this](SmsEvDraw& d) {
			if (d.mode == SMS_MODE_STAGE)
				drawCrosshair();
			if (d.mode != SMS_MODE_STAGE || core().time() > toastUntil)
				return;
			const float w = draw().text_width(14, toastText) + 20;
			SmsRect r;
			if (!draw().hud_slot(handle(), SMS_ANCHOR_TOP, w, 26, &r))
				return;
			draw().rect(r.x, r.y, r.w, r.h, SMS_RGBA(0, 0, 0, 170));
			draw().text(r.x + 10, r.y + 6, 14, SMS_RGB(255, 255, 255), SMS_TEXT_SHADOW, toastText);
		});

		info("loaded; %s or the controller's button switches first person", input().key_name(toggleKey));
		return 0;
	}
};

SMS_MOD_CLASS(FirstPerson)
