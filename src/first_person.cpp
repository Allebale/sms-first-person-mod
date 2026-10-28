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

namespace {

const float kPi = 3.14159265f;

// What of Mario shows in first person (the "model" setting; 2, the whole of him,
// is no longer offered: an old setting of it is Body only)
enum { MODEL_BODY = 0, MODEL_HIDDEN = 1 };
const char* const kModels[] = {"Body only (no head or FLUDD)", "Hidden"};

const char* const kButtons[] = {"D-pad Down", "D-pad Up", "D-pad Left", "D-pad Right", "None"};
const uint16_t kButtonBits[] = {SMS_PAD_DOWN, SMS_PAD_UP, SMS_PAD_LEFT, SMS_PAD_RIGHT, 0};

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

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
		haveYaw = false; // from the game camera again when it starts
		if (!enabled) {
			viewIsOurs = false;
			aimFludd(); // lets his body go, if it was turned
			showHead();
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
	void noHardTurn(float a, float b)
	{
		if (a * a + b * b < 40.0f * 40.0f || !SMS_API_HAS(api().player, set_yaw))
			return;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		if (!api().player->get(&m) || !m.on_ground || m.in_water || m.on_yoshi || m.forward_speed < 4.0f)
			return;
		// the stick's angle, clockwise from up, turns the yaw down (right is -90)
		const float want = yaw - atan2f(a, b) * 180.0f / kPi;
		float diff = fmodf(want - m.yaw, 360.0f);
		if (diff > 180.0f)
			diff -= 360.0f;
		if (diff < -180.0f)
			diff += 360.0f;
		if (fabsf(diff) > 90.0f)
			api().player->set_yaw(want);
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
	// (not swimming or on Yoshi: there he turns as the game turns him).
	bool bodyFacesView() const
	{
		if (!enabled || !viewIsOurs || !haveYaw || modelMode() != MODEL_BODY)
			return false;
		SmsPlayer m = SmsPlayer();
		m.size      = sizeof m;
		return api().player->get(&m) && !m.in_water && !m.on_yoshi;
	}

	// While R is held (FLUDD sprays, Yoshi spits), Mario's body and FLUDD
	// face where the view looks, so the water goes straight ahead, while the
	// stick still walks him any way: sideways too (player->set_aim, mod API
	// 1.5 beta 8). Standing still, he turns that way as well.
	void aimFludd()
	{
		const SmsPad p = pad(0);
		const bool want = enabled && viewIsOurs && haveYaw && setting("aim_fludd", 1)
		                  && ((p.held & SMS_PAD_R) || p.trigger_r >= 0.1f);
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
		});

		// the C-stick turns and tilts the view; L looks the way Mario faces
		on<SmsEvTick>(SMS_EV_TICK, [this](SmsEvTick& t) {
			if (!enabled || !viewIsOurs)
				return;
			const SmsPad p = pad(0);
			const float dt = 1.0f / (t.ticks_per_second > 0 ? (float)t.ticks_per_second : 30.0f);
			const float x  = setting("invert_x", 0) ? -p.cstick_x : p.cstick_x;
			const float y  = setting("invert", 0) ? -p.cstick_y : p.cstick_y;
			if (fabsf(x) > 0.2f) // right turns right: the yaw goes down
				yaw -= x * (float)setting("turn_speed", 180) * dt;
			if (fabsf(y) > 0.2f)
				pitch = clampf(pitch + y * (float)setting("tilt_speed", 90) * dt, -75.0f, 75.0f);
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
			// the stick's angle, clockwise from up, gives the world yaw
			// (the camera's) minus it: so turn it clockwise by game - ours
			const float d  = (gameYaw - yaw) * kPi / 180.0f;
			const float cd = cosf(d), sd = sinf(d);
			const float a = s.stick_x, b = s.stick_y;
			noHardTurn(a, b);
			const float na = a * cd + b * sd, nb = -a * sd + b * cd;
			s.stick_x = (int8_t)clampf(roundf(na), -127.0f, 127.0f);
			s.stick_y = (int8_t)clampf(roundf(nb), -127.0f, 127.0f);
			// R all the way down plants him to aim (the stance): in first person
			// the view aims, so keep him moving, spraying at full strength. The
			// stance comes from R's click; its analog part is the strength.
			if ((s.buttons & SMS_PAD_R) && setting("run_spray", 1) && spraysWithR()) {
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
			c.fovy       = (float)setting("fov", 60);
			aimFludd(); // his model too, before it is drawn
			showHead();
		});

		// his model out of the view (the camera is inside his head)
		// (the game asks twice a frame: FLUDD, then Mario's own models)
		on<SmsEvObject>(SMS_EV_HIDE_PLAYER_MODEL, [this](SmsEvObject&) {
			return enabled && viewIsOurs && modelMode() == MODEL_HIDDEN ? SMS_CANCEL : SMS_CONTINUE;
		});


		on<SmsEvDraw>(SMS_EV_DRAW, [this](SmsEvDraw& d) {
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
