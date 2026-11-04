// FLUDD Weapons: FLUDD's spray nozzle fires a Rifle, a Submachine gun or a Bazooka.
//
// D-pad left and right choose the weapon (FLUDD's own spray, Rifle, Submachine gun, Bazooka);
// R fires it. In first person (the First Person mod, FLUDD held in view) the weapon's own model is
// held there; in third person (its option) FLUDD stays on Mario's back, or is drawn as the
// weapon's pack. The Rifle zooms on D-pad up, with a sniper scope; from third person that goes
// into first person and back. The Bazooka's burst throws enemies, and Mario too in first person
// (a rocket jump). Long range: enemies and characters drawn and moving further away, to hit there.
//
// How: the game does the firing (api->held_fludd's set_weapon and set_weapon_stats); this mod
// chooses, keeps R from the game's own spray while a weapon fires, and draws the scope and the
// weapon's name. It and the First Person mod read each other through held_fludd's get_state.
#include <sms_mod.hpp>

#include <math.h>
#include <string>
#include <stddef.h>

namespace {

// the game's FLUDD weapons: api->held_fludd (Allebale's First Person and Remastered builds), taken
// only if its table says so (its magic)
const SmsHeldFluddApi* heldFluddApi(const SmsApi& api)
{
	if (api.size < offsetof(SmsApi, held_fludd) + sizeof(void*) || !api.held_fludd) // SMS_API_HAS
		return nullptr;
	const SmsHeldFluddApi* h = api.held_fludd;
	return h->size >= offsetof(SmsHeldFluddApi, set_weapon) + sizeof(void*) && h->magic == SMS_HELD_FLUDD_MAGIC
	               && h->set_weapon
	           ? h
	           : nullptr;
}
bool has(const SmsHeldFluddApi* h, size_t end) { return h && h->size >= end; }
#define HELD_HAS(h, field) has(h, offsetof(SmsHeldFluddApi, field) + sizeof(void*))

const char* const kWeaponNames[] = {"FLUDD", "Rifle", "Submachine gun", "Bazooka"};
const int kWeaponIds[]           = {SMS_SPRAY_NORMAL, SMS_SPRAY_RIFLE, SMS_SPRAY_SMG, SMS_SPRAY_BAZOOKA};
const int kWeaponCount           = 4;

// long range: how much further than the game's own distance enemies and characters are drawn and
// move (the "long_range" setting)
const char* const kRanges[] = {"Off", "Low", "Medium", "High", "Very high", "Insane"};
const float kRangeScale[]   = {1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f};
const int kRangeCount       = 6;

// Shadow Mario: how much of his water meter each weapon hit takes, times the game's own (the
// "shadow_mario" setting)
const char* const kShadowLevels[] = {"Very easy", "Easy", "Normal", "Hard", "Very hard", "Extreme"};
const float kShadowDamage[]       = {2.0f, 1.4f, 1.0f, 0.6f, 0.35f, 0.2f};
const int kShadowCount            = 6;

} // namespace

struct FluddWeapons : sms::Mod {
	using sms::Mod::Mod;

	int weapon    = 0;     // the weapon chosen (kWeaponNames)
	bool rHeld    = false; // R, as the controller has it (kept from the game while a weapon fires)
	bool zoomed   = false; // the Rifle's zoom
	bool zoomFromThird = false; // zoomed from third person (into first person for it)
	bool sawFirst = false;      // ...and first person has come on for it
	double zoomedAt  = 0;       // when the zoom came on (the scope comes in with it)
	bool upWasDown = false, rightWasDown = false, leftWasDown = false;
	bool told      = false, toldNone = false;
	int lastSet    = -1; // the weapon and flags last set, -1 none
	float lastFar  = 1.0f;
	double toastUntil = 0;
	const char* toastText = "";

	// what the First Person mod has set (first person on, FLUDD held in view)
	SmsHeldFluddState state() const
	{
		SmsHeldFluddState s = SmsHeldFluddState();
		s.size              = sizeof s;
		const SmsHeldFluddApi* h = heldFluddApi(api());
		if (!HELD_HAS(h, get_state) || !h->get_state || !h->get_state(&s))
			s = SmsHeldFluddState();
		return s;
	}

	int chosen() const { return weapon >= 0 && weapon < kWeaponCount ? kWeaponIds[weapon] : SMS_SPRAY_NORMAL; }

	// Holding on to something (a fence, a ladder, a bar, a wire, a ceiling's net): no weapon.
	bool holding() const
	{
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m))
			return false;
		return (m.status & 0x30000000u) != 0 || (m.status & 0x200000u) != 0;
	}

	// first person: FLUDD held in view (the First Person mod); third person: its option
	bool firstPerson() const { return state().first_person != 0; }
	bool thirdPersonWeapons() const { return !firstPerson() && setting("third_person_weapons", 1); }
	bool weaponsHere() const
	{
		if (!setting("weapons", 1))
			return false;
		const SmsHeldFluddState s = state();
		return s.first_person ? s.held != 0 : setting("third_person_weapons", 1) != 0;
	}

	// the weapon that fires now: with FLUDD held (or the third person option) and its spray nozzle out
	int weaponNow() const
	{
		if (!weaponsHere() || chosen() == SMS_SPRAY_NORMAL || holding())
			return 0;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m) || !m.has_fludd || m.on_yoshi || m.current_nozzle != 0)
			return 0;
		return chosen();
	}

	void toast(const char* text)
	{
		toastText  = text;
		toastUntil = core().time() + 1.5;
	}

	// D-pad left and right: the weapon
	void switchWeapon(const SmsPad& p)
	{
		const bool right = (p.held & SMS_PAD_RIGHT) != 0, left = (p.held & SMS_PAD_LEFT) != 0;
		const bool newRight = right && !rightWasDown, newLeft = left && !leftWasDown;
		rightWasDown = right;
		leftWasDown  = left;
		if (!weaponsHere() || (!newRight && !newLeft))
			return;
		weapon = (weapon + (newRight ? 1 : kWeaponCount - 1)) % kWeaponCount;
		if (zoomed && !zoomFromThird)
			zoomed = false;
		toast(kWeaponNames[weapon]);
		info("weapon: %s", kWeaponNames[weapon]);
	}

	// D-pad up: the Rifle's zoom on and off in first person (FLUDD in view); in third person with
	// its weapons, into first person zoomed (the First Person mod switches) and back out again
	bool canZoom() const
	{
		const SmsHeldFluddState s = state();
		return setting("weapons", 1) && chosen() == SMS_SPRAY_RIFLE && s.first_person && s.held;
	}
	void toggleZoom(const SmsPad& p)
	{
		const bool fp = firstPerson();
		if (zoomFromThird && fp)
			sawFirst = true;
		if (zoomFromThird && sawFirst && !fp) // left first person some other way
			zoomed = zoomFromThird = sawFirst = false;
		if (zoomed && !zoomFromThird && !canZoom())
			zoomed = false;
		const bool down  = (p.held & SMS_PAD_UP) != 0;
		const bool fresh = down && !upWasDown; // once a press
		upWasDown        = down;
		if (!fresh)
			return;
		if (zoomFromThird) { // back to third person
			zoomed = zoomFromThird = sawFirst = false;
			info("rifle zoom off, back to third person");
			return;
		}
		if (!fp && chosen() == SMS_SPRAY_RIFLE && thirdPersonWeapons() && weaponNow() == SMS_SPRAY_RIFLE) {
			zoomed = zoomFromThird = true; // into first person for it
			sawFirst = false;
			zoomedAt = core().time();
			info("rifle zoom on, from third person");
			return;
		}
		if (!canZoom())
			return;
		zoomed   = !zoomed;
		zoomedAt = core().time();
		info("rifle zoom %s", zoomed ? "on" : "off");
	}

	// The weapons' numbers from the menu, to the game.
	void sendStats(const SmsHeldFluddApi* h)
	{
		if (!HELD_HAS(h, set_weapon_stats) || !h->set_weapon_stats)
			return;
		const int r       = setting("long_range", 5);
		const float far   = kRangeScale[r >= 0 && r < kRangeCount ? r : 0];
		if (far != lastFar) {
			info("long range: %s (%.1f times as far)", kRanges[r >= 0 && r < kRangeCount ? r : 0], far);
			lastFar = far;
		}
		const int sh      = setting("shadow_mario", 5);
		const float boss  = kShadowDamage[sh >= 0 && sh < kShadowCount ? sh : 2];
		SmsSprayStats ns  = SmsSprayStats(); // FLUDD's own spray: only its time to defeat
		ns.size           = sizeof ns;
		ns.every          = (float)setting("normal_defeat_time", 4) / 10.0f;
		ns.far_scale      = far;
		ns.shadow_damage = boss;
		h->set_weapon_stats(SMS_SPRAY_NORMAL, &ns);

		SmsSprayStats rf = SmsSprayStats(); // the Rifle: one big, fast blob of water, dead straight
		rf.size          = sizeof rf;
		rf.every         = (float)setting("rifle_rate", 13) / 10.0f;
		rf.push          = (float)setting("rifle_push", 300) / 100.0f;
		rf.range         = 1.0f;
		rf.to_defeat     = setting("rifle_defeat", 2);
		rf.water         = setting("rifle_water", 16);
		rf.hit_size      = (float)setting("rifle_hit", 300) / 100.0f;
		rf.drop_size     = (float)setting("rifle_blob", 50) / 100.0f;
		rf.burst         = 1.0f;
		rf.view_distance = (float)setting("rifle_distance", 50);
		rf.far_scale     = far;
		rf.shadow_damage = boss;
		h->set_weapon_stats(SMS_SPRAY_RIFLE, &rf);

		SmsSprayStats sm = SmsSprayStats(); // the Submachine gun: the Rifle's kind of shot, faster and softer
		sm.size          = sizeof sm;
		sm.every         = (float)setting("smg_rate", 14) / 100.0f;
		sm.push          = (float)setting("smg_push", 90) / 100.0f;
		sm.range         = 1.0f;
		sm.to_defeat     = setting("smg_defeat", 6);
		sm.water         = setting("smg_water", 16);
		sm.hit_size      = (float)setting("smg_hit", 100) / 100.0f;
		sm.drop_size     = (float)setting("smg_blob", 20) / 100.0f;
		sm.burst         = 1.0f;
		sm.view_distance = (float)setting("smg_distance", 45);
		sm.far_scale     = far;
		sm.shadow_damage = boss;
		h->set_weapon_stats(SMS_SPRAY_SMG, &sm);

		SmsSprayStats bz = SmsSprayStats(); // the Bazooka: a burst where it lands
		bz.size          = sizeof bz;
		bz.every         = (float)setting("bazooka_rate", 9) / 10.0f;
		bz.push          = (float)setting("bazooka_push", 150) / 100.0f;
		bz.range         = (float)setting("bazooka_range", 100) / 100.0f;
		bz.to_defeat     = setting("bazooka_defeat", 3);
		bz.water         = setting("bazooka_water", 60);
		bz.hit_size      = 1.0f;
		bz.drop_size     = 1.0f;
		bz.burst         = (float)setting("bazooka_burst", 150) / 100.0f;
		bz.lift          = (float)setting("bazooka_lift", 60) / 100.0f;
		bz.self_jump     = setting("rocket_jump", 1) ? (float)setting("rocket_jump_power", 150) / 100.0f : 0.0f;
		bz.view_distance = (float)setting("bazooka_distance", 55);
		bz.far_scale     = far;
		bz.shadow_damage = boss;
		h->set_weapon_stats(SMS_SPRAY_BAZOOKA, &bz);
	}

	// Each frame: the weapon (or none) to the game, its numbers, and the zoom for the First Person mod.
	void update()
	{
		const SmsHeldFluddApi* h = heldFluddApi(api());
		if (!h) {
			if (setting("weapons", 1) && !toldNone) {
				warn("this game cannot fire FLUDD weapons (it needs a Remastered or First Person beta build)");
				toldNone = true;
			}
			return;
		}
		if (!told) {
			info("D-pad left and right choose the weapon%s", HELD_HAS(h, get_state) ? "" : " (this game is too old for the First Person mod's view)");
			told = true;
		}
		const SmsHeldFluddState s = state();
		const int w               = weaponNow();
		const int defeat          = (setting("weapons_defeat", 1) ? SMS_SPRAY_DEFEAT : 0) | (rHeld ? 2 : 0);
		const int normal          = setting("normal_defeat", 1) ? SMS_SPRAY_DEFEAT_NORMAL : 0;
		int flags                 = 0;
		if (w && s.first_person)
			flags = defeat | normal;
		else if (w) // third person: FLUDD on his back, or the weapon's pack
			flags = defeat | normal | SMS_SPRAY_THIRD_PERSON | (setting("third_person_models", 1) ? SMS_SPRAY_THIRD_PERSON_MODEL : 0);
		else if (s.first_person && s.held) // FLUDD's own spray, held in view
			flags = normal;
		const int set = w * 1000 + flags;
		if (w || flags || lastSet > 0) {
			h->set_weapon(w, flags);
			if (w || flags)
				sendStats(h);
		}
		lastSet = set;
		if (HELD_HAS(h, set_zoom) && h->set_zoom)
			h->set_zoom(zoomed ? (float)setting("rifle_zoom", 25) : 0.0f,
			            zoomed ? (zoomFromThird ? SMS_ZOOM_FIRST_PERSON : 0) | (scopeShown() ? SMS_ZOOM_SCOPE : 0) : 0);
	}

	bool scopeShown() const
	{
		const SmsHeldFluddState s = state();
		return zoomed && setting("rifle_scope", 1) && s.first_person && s.held && chosen() == SMS_SPRAY_RIFLE;
	}

	// The Rifle's sniper scope while it is zoomed: black round a round sight, thin cross hairs with
	// marks along them and a red dot in the middle, coming in with the zoom
	void drawScope()
	{
		if (!scopeShown() || state().game_scope)
			return; // the game draws it itself, under its HUD
		float in = (float)((core().time() - zoomedAt) / 0.3);
		in       = in < 0.0f ? 0.0f : in > 1.0f ? 1.0f : in;
		if (in < 0.05f)
			return;
		float w = 0, h = 0;
		draw().canvas(&w, &h);
		const float x = w * 0.5f, y = h * 0.5f, rad = h * 0.46f;
		const uint8_t a = (uint8_t)(255.0f * in);
		const float out = sqrtf(x * x + y * y) + 4.0f; // past the corners
		draw().circle_outline(x, y, (rad + out) * 0.5f, out - rad, SMS_RGBA(0, 0, 0, a)); // all round the sight
		draw().circle_outline(x, y, rad, h * 0.012f, SMS_RGBA(20, 20, 20, a));          // its rim
		const SmsColor hair = SMS_RGBA(0, 0, 0, (uint8_t)(230.0f * in));
		const float t = h * 0.0025f, gap = h * 0.02f;
		draw().line(x - rad, y, x - gap, y, t, hair); // the cross hairs, open in the middle
		draw().line(x + gap, y, x + rad, y, t, hair);
		draw().line(x, y - rad, x, y - gap, t, hair);
		draw().line(x, y + gap, x, y + rad, t, hair);
		draw().line(x - rad, y, x - rad * 0.55f, y, t * 3.0f, hair); // thicker at the edge
		draw().line(x + rad * 0.55f, y, x + rad, y, t * 3.0f, hair);
		draw().line(x, y + rad * 0.55f, x, y + rad, t * 3.0f, hair);
		for (int i = 1; i <= 4; i++) { // the marks
			const float d = rad * 0.1f * (float)i, m = h * 0.008f;
			draw().line(x - d, y - m, x - d, y + m, t, hair);
			draw().line(x + d, y - m, x + d, y + m, t, hair);
			draw().line(x - m, y + d, x + m, y + d, t, hair);
		}
		draw().circle(x, y, h * 0.003f, SMS_RGBA(230, 30, 30, a));
	}

	int load() override
	{
		optionBool("weapons", "FLUDD weapons, D-pad left and right", true);
		optionBool("weapons_defeat", "FLUDD weapons can defeat enemies", true);
		optionBool("third_person_weapons", "FLUDD weapons in third person too", true);
		optionBool("third_person_models", "Weapon packs in place of FLUDD in third person", true);
		optionBool("normal_defeat", "FLUDD can defeat enemies", true);
		optionNumber("normal_defeat_time", "FLUDD time to defeat (tenths of a second)", 4, 1, 100, 1);
		optionChoice("long_range", "Long range (enemies and characters seen and hit further away)",
		             std::vector<const char*>(kRanges, kRanges + kRangeCount), 5);
		optionChoice("shadow_mario", "Shadow Mario difficulty (how many hits to take him down)",
		             std::vector<const char*>(kShadowLevels, kShadowLevels + kShadowCount), 5);
		optionNumber("rifle_distance", "Rifle distance from the camera", 50, 30, 120, 5);
		optionNumber("smg_distance", "Submachine gun distance from the camera", 45, 30, 120, 5);
		optionNumber("bazooka_distance", "Bazooka distance from the camera", 55, 30, 120, 5);
		optionNumber("rifle_rate", "Rifle fire rate (tenths of a second)", 13, 1, 100, 1);
		optionNumber("rifle_push", "Rifle push (percent)", 300, 0, 400, 10);
		optionNumber("rifle_defeat", "Rifle shots to defeat", 2, 1, 10, 1);
		optionNumber("rifle_water", "Rifle water per shot", 16, 0, 200, 2);
		optionNumber("rifle_zoom", "Rifle zoom, D-pad up (field of view)", 25, 5, 60, 5);
		optionBool("rifle_scope", "Rifle sniper scope while zoomed", true);
		optionNumber("rifle_hit", "Rifle hit size (percent, how near it must pass an enemy)", 300, 20, 500, 10);
		optionNumber("rifle_blob", "Rifle blob size (percent)", 50, 20, 400, 10);
		optionNumber("smg_rate", "Submachine gun fire rate (hundredths of a second)", 14, 1, 100, 1);
		optionNumber("smg_push", "Submachine gun push (percent, under 100 weaker than FLUDD)", 90, 0, 400, 10);
		optionNumber("smg_defeat", "Submachine gun shots to defeat", 6, 1, 10, 1);
		optionNumber("smg_water", "Submachine gun water per shot", 16, 0, 200, 2);
		optionNumber("smg_hit", "Submachine gun hit size (percent)", 100, 20, 500, 10);
		optionNumber("smg_blob", "Submachine gun blob size (percent)", 20, 20, 400, 10);
		optionNumber("bazooka_rate", "Bazooka fire rate (tenths of a second)", 9, 1, 200, 1);
		optionNumber("bazooka_push", "Bazooka push (percent)", 150, 0, 400, 10);
		optionNumber("bazooka_range", "Bazooka range (percent)", 100, 10, 100, 10);
		optionNumber("bazooka_defeat", "Bazooka shots to defeat", 3, 1, 10, 1);
		optionNumber("bazooka_water", "Bazooka water per shot", 60, 0, 300, 5);
		optionNumber("bazooka_burst", "Bazooka burst size (percent)", 150, 20, 400, 10);
		optionNumber("bazooka_lift", "Bazooka air height (percent)", 60, 0, 400, 10);
		optionBool("rocket_jump", "Bazooka rocket jumps in first person (shoot near your feet)", true);
		optionNumber("rocket_jump_power", "Bazooka rocket jump strength (percent)", 150, 10, 300, 10);

		// the D-pad, once a frame
		on<SmsEvFrame>(SMS_EV_FRAME, [this](SmsEvFrame& f) {
			if (f.mode != SMS_MODE_STAGE)
				return;
			switchWeapon(pad(0));
			toggleZoom(pad(0));
		});

		// R: the weapon's while one can fire (no stream, pump or its sound from the game); the game
		// fires it each time it is ready while R is held
		on<SmsEvPadRead>(SMS_EV_PAD_READ, [this](SmsEvPadRead& r) {
			if (!r.pads)
				return;
			SmsPadStatus& s = r.pads[0];
			rHeld           = (s.buttons & SMS_PAD_R) || s.trigger_r >= 60;
			if (weaponNow() != 0) {
				s.buttons &= (uint16_t)~SMS_PAD_R;
				s.trigger_r = 0;
			}
		});

		on<SmsEvCamera>(SMS_EV_CAMERA, [this](SmsEvCamera&) { update(); });

		on<SmsEvDraw>(SMS_EV_DRAW, [this](SmsEvDraw& d) {
			if (d.mode != SMS_MODE_STAGE)
				return;
			drawScope();
			if (core().time() > toastUntil)
				return;
			const float w = draw().text_width(14, toastText) + 20;
			SmsRect r;
			if (!draw().hud_slot(handle(), SMS_ANCHOR_TOP, w, 26, &r))
				return;
			draw().rect(r.x, r.y, r.w, r.h, SMS_RGBA(0, 0, 0, 170));
			draw().text(r.x + 10, r.y + 6, 14, SMS_RGB(255, 255, 255), SMS_TEXT_SHADOW, toastText);
		});

		info("loaded");
		return 0;
	}
};

SMS_MOD_CLASS(FluddWeapons)
