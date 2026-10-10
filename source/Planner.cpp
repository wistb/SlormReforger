// Reads builds shared from slorm-planner. The whole build is packed into the link:
// six bits per character, fields in the order the planner writes them.
#include "Reforger.hpp"
#include <tuple>

std::vector<Preset> g_Presets;
int g_ActivePreset = -1;

void ImportPreset(const Preset& From, const PresetSlot& Slot)
{
	g_Targets.clear();
	for (const Target& target : Slot.targets)
		if (target.tier != "MA" || From.hero == g_HeroClass) g_Targets.push_back(target);
	// What can roll depends on the item level, so a higher-level build levels the item first.
	if (Slot.level > g_ItemLevel)
	{
		Target level;
		level.tier = "LV";
		level.stat = "level";
		g_Targets.insert(g_Targets.begin(), level);
	}
}

// A build has two rings: the one sharing more stats with the item wins, the first on a tie.
const PresetSlot* BestPresetSlot(const Preset& From)
{
	const PresetSlot* best = nullptr;
	int best_shared = -1;
	for (const PresetSlot& slot : From.slots)
	{
		if (slot.slot != Lower(g_ItemSlot))
			continue;
		int shared = 0;
		for (const Target& target : slot.targets)
			for (const Affix& affix : g_Item)
				if (affix.stat == target.stat) shared++;
		if (shared > best_shared)
		{
			best = &slot;
			best_shared = shared;
		}
	}
	return best;
}

namespace
{
	const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789$";

	struct Reader
	{
		std::vector<bool> bits;
		size_t at = 0;
		bool overrun = false;

		int Take(int Count)
		{
			int value = 0;
			for (int i = 0; i < Count; i++)
			{
				if (at >= bits.size()) overrun = true;
				value = value * 2 + (at < bits.size() && bits[at] ? 1 : 0);
				at++;
			}
			return value;
		}
	};

	const StatInfo* StatById(int Id)
	{
		std::string id = std::to_string(Id);
		for (const StatInfo& stat : g_Stats)
		{
			auto found = stat.columns.find("REF_NB");
			if (found != stat.columns.end() && found->second == id) return &stat;
		}
		return nullptr;
	}

	void AddSpecial(PresetSlot& Slot, const char* Tier, int Type, int Value)
	{
		if (Type == 0)
			return;
		Target target;
		target.tier = Tier;
		target.stat = Lower(Tier) + "_" + std::to_string(Type - 1);
		if (Value >= MaxRoll(Tier, target.stat))
			target.goal = Goal::MaxRoll;
		else
		{
			target.goal = Goal::Value;
			target.amount = Value;
		}
		Slot.targets.push_back(target);
	}
}

const char* HeroName(int Class)
{
	static const char* names[] = { "Knight", "Huntress", "Mage" };
	return Class >= 0 && Class < 3 ? names[Class] : "Unknown";
}

bool ParsePreset(const std::string& Text, Preset& Out, std::string& Error)
{
	// A full link or only its last part.
	std::string key = Text;
	while (!key.empty() && isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
	size_t slash = key.find_last_of('/');
	if (slash != std::string::npos)
		key.erase(0, slash + 1);
	while (!key.empty() && isspace(static_cast<unsigned char>(key.front()))) key.erase(0, 1);
	if (key.size() < 20)
	{
		Error = "that is not a planner link";
		return false;
	}

	Reader reader;
	for (char c : key)
	{
		const char* found = strchr(ALPHABET, c);
		if (!found || !c)
		{
			Error = "that is not a planner link";
			return false;
		}
		int value = static_cast<int>(found - ALPHABET);
		for (int bit = 5; bit >= 0; bit--) reader.bits.push_back((value >> bit) & 1);
	}

	int major = reader.Take(4), minor = reader.Take(4), fix = reader.Take(6);
	auto since = [&](int Major, int Minor, int Fix) { return std::tie(major, minor, fix) >= std::tie(Major, Minor, Fix); };

	Out = {};
	Out.key = key;
	Out.hero = reader.Take(2);
	Out.level = reader.Take(since(0, 4, 0) ? 7 : 6);

	// Reaper, runes, ancestral legacies and skills come before the gear; skip them.
	int reaper = reader.Take(10);
	bool primordial = reader.Take(1) != 0;
	reader.Take(7);
	if (since(0, 2, 0)) reader.Take(7);
	if (reaper == 90 && primordial) reader.Take(10);
	if (reaper == 114) reader.Take(25);
	if (since(0, 7, 0)) reader.Take(7);
	if (since(0, 2, 0))
		for (int i = 0; i < 3; i++)
			if (reader.Take(1)) reader.Take(11);
	if (since(0, 5, 0) && reader.Take(1)) reader.Take(10);
	for (int i = reader.Take(4); i > 0; i--) reader.Take(10);
	for (int i = reader.Take(4); i > 0; i--) reader.Take(14);
	for (int i = reader.Take(4); i > 0; i--)
	{
		reader.Take(15);
		if (reader.Take(1))
			for (int j = reader.Take(4); j > 0; j--) reader.Take(14);
	}

	static const char* slots[] = { "helm", "body", "shoulder", "bracer", "glove", "boot", "ring", "ring", "amulet", "belt", "cape" };
	static const char* labels[] = { "Helm", "Body", "Shoulder", "Bracer", "Glove", "Boot", "Left ring", "Right ring", "Amulet", "Belt", "Cape" };
	static const char* tiers[] = { "N", "D", "M", "R", "E" };
	for (int i = reader.Take(4); i > 0; i--)
	{
		int index = reader.Take(5);
		if (index > 10 || reader.overrun)
		{
			Error = "could not read the gear in this link";
			return false;
		}
		PresetSlot slot;
		slot.slot = slots[index];
		slot.label = labels[index];
		slot.level = reader.Take(since(0, 4, 1) ? 7 : 6);
		reader.Take(8);
		if (slot.level > 100) reader.Take(4);
		for (int j = reader.Take(4); j > 0; j--)
		{
			int rarity = reader.Take(3);
			const StatInfo* stat = StatById(reader.Take(9));
			int roll = reader.Take(9);
			if (reader.Take(1)) reader.Take(8);
			if (!stat || rarity > 6)
			{
				Error = "this link has a stat the game does not know";
				return false;
			}
			if (rarity > 4)
				continue;
			Target target;
			target.tier = tiers[rarity];
			target.stat = stat->ref;
			// The planner stores rolls on the full scale: 100 normal, 65 defense to rare, 40 epic.
			// Low-level items roll percent stats on a smaller one, so keep a lower roll as a share.
			double top = rarity == 0 ? 100 : rarity == 4 ? 40 : 65;
			if (roll >= top)
				target.goal = Goal::MaxRoll;
			else
			{
				target.goal = Goal::Share;
				target.amount = roll * 100.0 / top;
			}
			slot.targets.push_back(target);
		}
		int attribute = reader.Take(4), attribute_value = reader.Take(2);
		int smith = reader.Take(4), smith_value = reader.Take(3);
		int skill = reader.Take(4), skill_value = reader.Take(3);
		slot.legendary = reader.Take(10) - 1;
		int legendary_value = reader.Take(8);
		AddSpecial(slot, "RP", smith, smith_value);
		AddSpecial(slot, "MA", skill, skill_value);
		AddSpecial(slot, "AT", attribute, attribute_value);
		if (slot.legendary >= 0)
		{
			Target target;
			target.tier = "L";
			target.stat = "leg_" + std::to_string(slot.legendary);
			target.goal = legendary_value >= 100 ? Goal::MaxRoll : Goal::Roll;
			target.amount = legendary_value;
			slot.targets.push_back(target);
		}
		Out.slots.push_back(std::move(slot));
	}
	if (reader.overrun || Out.slots.empty())
	{
		Error = Out.slots.empty() ? "this build has no gear" : "could not read the gear in this link";
		return false;
	}
	Out.name = std::string(HeroName(Out.hero)) + " level " + std::to_string(Out.level);
	return true;
}
