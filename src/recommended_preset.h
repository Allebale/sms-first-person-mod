// The "Settings preset" row at the top of a mod's options: Recommended or Custom.
// Recommended is every option's default. Choosing it sets every option back to its default;
// changing any option to something else shows Custom (and back to Recommended when all match).
#pragma once
#include "sms_mod.hpp"

#include <string>
#include <vector>

class RecommendedPreset {
public:
	explicit RecommendedPreset(sms::Mod& mod) : mod_(mod) {}

	// The row itself: add it before the other options.
	void addRow(const char* label = "Settings preset")
	{
		static const char* const kNames[] = {"Recommended", "Custom"};
		SmsOption o    = make(key(), label, SMS_OPTION_CHOICE);
		o.def          = 0.0f;
		o.choices      = kNames;
		o.choice_count = 2;
		o.on_change    = &RecommendedPreset::presetChanged;
		mod_.config().add_option(mod_.handle(), &o);
	}

	void optionBool(const char* key, const char* label, bool def)
	{
		SmsOption o = make(key, label, SMS_OPTION_BOOL);
		o.def       = def ? 1.0f : 0.0f;
		add(o);
	}
	void optionNumber(const char* key, const char* label, float def, float min, float max, float step)
	{
		SmsOption o = make(key, label, SMS_OPTION_INT);
		o.def       = def;
		o.min       = min;
		o.max       = max;
		o.step      = step;
		add(o);
	}
	void optionChoice(const char* key, const char* label, const std::vector<const char*>& choices, int def = 0)
	{
		SmsOption o    = make(key, label, SMS_OPTION_CHOICE);
		o.def          = (float)def;
		o.choices      = choices.data();
		o.choice_count = (int)choices.size();
		add(o);
	}

	// Recommended if every option is at its default, else Custom (as the mod loads, for settings
	// kept from before).
	void check()
	{
		bool all = true;
		for (size_t i = 0; i < keys_.size() && all; i++)
			all = mod_.setting(keys_[i].c_str(), defs_[i]) == defs_[i];
		if (mod_.setting(key(), 0) != (all ? 0 : 1)) {
			mod_.config().set_int(mod_.handle(), key(), all ? 0 : 1);
			mod_.config().save(mod_.handle());
		}
	}

private:
	static const char* key() { return "settings_preset"; }

	SmsOption make(const char* key, const char* label, int type)
	{
		SmsOption o = SmsOption();
		o.size      = sizeof o;
		o.key       = key;
		o.label     = label;
		o.type      = type;
		o.user      = this;
		return o;
	}
	void add(SmsOption& o)
	{
		o.on_change = &RecommendedPreset::optionChanged;
		keys_.push_back(o.key);
		defs_.push_back((int)o.def);
		mod_.config().add_option(mod_.handle(), &o);
	}
	static void optionChanged(SmsMod*, const char*, void* user) { static_cast<RecommendedPreset*>(user)->check(); }
	static void presetChanged(SmsMod*, const char*, void* user)
	{
		RecommendedPreset& p = *static_cast<RecommendedPreset*>(user);
		if (p.mod_.setting(key(), 0) != 0)
			return; // Custom: the options stay as they are
		for (size_t i = 0; i < p.keys_.size(); i++)
			p.mod_.config().set_int(p.mod_.handle(), p.keys_[i].c_str(), p.defs_[i]);
		p.mod_.config().save(p.mod_.handle());
	}

	sms::Mod& mod_;
	std::vector<std::string> keys_;
	std::vector<int> defs_;
};
