#include <YYToolkit/YYTK_Shared.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace Aurie;
using namespace YYTK;

struct Affix
{
	std::string tier;
	std::string stat;
	double roll = 0;
	bool locked = false;
	double pure = 0;
	// Displayed value, as the panel computed it this frame.
	double shown = 0;
	bool has_shown = false;
};

enum class Goal { MaxRoll, Value, Roll };

struct Target
{
	std::string tier;
	std::string stat;
	Goal goal = Goal::MaxRoll;
	double amount = 0;
};

static YYTKInterface* g_Yytk = nullptr;
static fs::path g_ConfigPath;

static PFUNC_YYGMLScript g_StatFromScore = nullptr;
static PFUNC_YYGMLScript g_Apply = nullptr;

static std::vector<Target> g_Targets;
static int g_MaxAttempts = 50;
static bool g_AllowPureLoss = false;

static bool g_Running = false;
static bool g_KeyWasDown = false;
static int g_Attempts = 0;
static int g_Cooldown = 0;
static std::vector<Affix> g_Item;
static int g_ItemLevel = 0;

// The blacksmith's reforge slot in global.inventory[hero].
static constexpr size_t REFORGE_SLOT = 666;

template <typename... Args>
static void Log(const char* Format, Args... Arguments)
{
	DbgPrintEx(LOG_SEVERITY_INFO, (std::string("[SlormReforger] ") + Format).c_str(), Arguments...);
}

static double ToNumber(const RValue& Value)
{
	if (Value.m_Kind == VALUE_STRING)
	{
		try { return std::stod(Value.ToString()); }
		catch (...) { return 0; }
	}
	return Value.IsNumberConvertible() ? Value.ToDouble() : 0;
}

// Item layout: [header, [tier, stat, roll, locked, pure], ...]
static bool ReadItem(const RValue& Item, std::vector<Affix>& Out)
{
	if (!Item.IsArray())
		return false;
	std::vector<RValue> parts = Item.ToVector();
	if (parts.empty() || !parts[0].IsArray())
		return false;

	std::vector<RValue> header = parts[0].ToVector();
	g_ItemLevel = header.size() > 2 ? static_cast<int>(ToNumber(header[2])) : 0;

	Out.clear();
	for (size_t i = 1; i < parts.size(); i++)
	{
		if (!parts[i].IsArray())
			continue;
		std::vector<RValue> fields = parts[i].ToVector();
		if (fields.size() < 5 || !fields[0].IsString() || !fields[1].IsString())
			continue;
		Affix affix;
		affix.tier = fields[0].ToString();
		affix.stat = fields[1].ToString();
		affix.roll = ToNumber(fields[2]);
		affix.locked = ToNumber(fields[3]) != 0;
		affix.pure = ToNumber(fields[4]);
		Out.push_back(affix);
	}
	return true;
}

static bool IsItem(const RValue& Value)
{
	if (!Value.IsArray())
		return false;
	std::vector<RValue> parts = Value.ToVector();
	return parts.size() > 1 && parts[0].IsArray();
}

// Reads the item in the reforge slot, with the values the game would display.
static bool ReadSlotItem(CInstance* Self, CInstance* Other)
{
	RValue inventory = g_Yytk->CallBuiltin("variable_global_get", { RValue("inventory") });
	if (!inventory.IsArray())
		return false;

	std::vector<RValue> heroes = inventory.ToVector();
	RValue item;
	int found = 0;
	for (const RValue& hero : heroes)
	{
		if (!hero.IsArray())
			continue;
		std::vector<RValue*> slots = const_cast<RValue&>(hero).ToRefVector();
		if (slots.size() > REFORGE_SLOT && slots[REFORGE_SLOT] && IsItem(*slots[REFORGE_SLOT]))
		{
			item = *slots[REFORGE_SLOT];
			found++;
		}
	}
	// More than one hero has an item parked in the slot; can't tell which is open.
	if (found != 1 || !ReadItem(item, g_Item))
		return false;

	std::vector<RValue> parts = item.ToVector();
	size_t affix_index = 0;
	for (size_t i = 1; i < parts.size(); i++)
	{
		if (!parts[i].IsArray() || affix_index >= g_Item.size())
			continue;
		std::vector<RValue> fields = parts[i].ToVector();
		if (fields.size() < 5)
			continue;

		// Arguments: roll, stat, tier, index, level, item.
		RValue arguments[6] = { fields[2], fields[1], fields[0], RValue(static_cast<double>(i)), RValue(static_cast<double>(g_ItemLevel)), item };
		RValue* pointers[6] = { &arguments[0], &arguments[1], &arguments[2], &arguments[3], &arguments[4], &arguments[5] };
		RValue shown;
		g_StatFromScore(Self, Other, shown, 6, pointers);

		g_Item[affix_index].shown = ToNumber(shown);
		g_Item[affix_index].has_shown = true;
		affix_index++;
	}
	return true;
}

static void LoadConfig()
{
	g_Targets.clear();
	g_MaxAttempts = 50;
	g_AllowPureLoss = false;

	std::ifstream file(g_ConfigPath);
	if (!file)
	{
		std::ofstream sample(g_ConfigPath);
		sample << "# One target per line: TIER STAT GOAL\n"
			"# TIER is N, D, M, R or E. STAT is a REF from dat_sta.json.\n"
			"# GOAL is max (best possible roll), a number (displayed value to reach), or roll:N.\n"
			"# D dodge_add max\n"
			"# D dodge_add 1500\n"
			"max_attempts 50\n"
			"allow_pure_loss 0\n";
		return;
	}

	std::string line;
	while (std::getline(file, line))
	{
		std::istringstream words(line);
		std::string first;
		if (!(words >> first) || first[0] == '#')
			continue;
		if (first == "max_attempts") { words >> g_MaxAttempts; continue; }
		if (first == "allow_pure_loss") { int flag = 0; words >> flag; g_AllowPureLoss = flag != 0; continue; }
		Target target;
		target.tier = first;
		std::string goal;
		if (!(words >> target.stat >> goal))
			continue;
		try
		{
			if (goal == "max") target.goal = Goal::MaxRoll;
			else if (goal.rfind("roll:", 0) == 0) { target.goal = Goal::Roll; target.amount = std::stod(goal.substr(5)); }
			else { target.goal = Goal::Value; target.amount = std::stod(goal); }
		}
		catch (...) { continue; }
		g_Targets.push_back(target);
	}
}

// Best roll for a tier. Percent stats scale down on low-level items.
static double MaxRoll(const std::string& Tier, const std::string& Stat)
{
	double base = Tier == "N" ? 100 : Tier == "E" ? 40 : 65;
	bool percent = Stat.ends_with("_percent") || Stat.ends_with("_mult");
	if (!percent)
		return base;
	int step = g_ItemLevel >= 52 ? 5 : g_ItemLevel >= 45 ? 4 : g_ItemLevel >= 35 ? 3 : g_ItemLevel >= 20 ? 2 : 1;
	return base * step / 5;
}

static bool Met(const Target& Target, const Affix& Affix)
{
	switch (Target.goal)
	{
	case Goal::MaxRoll: return Affix.roll >= MaxRoll(Target.tier, Target.stat);
	case Goal::Roll: return Affix.roll >= Target.amount;
	default: return Affix.has_shown && Affix.shown >= Target.amount;
	}
}

static void Stop(const char* Reason)
{
	Log("stopped after %d reforges: %s", g_Attempts, Reason);
	g_Running = false;
}

// Recipe layout: [id, label, type (0 scores, 1 stats), materials, number, tier code, ...]
static bool g_AllScores = false;

// Tier "" finds the recipe with no tier code ("Reforge all Scores").
static int FindRecipe(CInstance* Ui, int Type, const std::string& Tier)
{
	std::string code = Tier.empty() ? "" : std::string(1, static_cast<char>(std::tolower(Tier[0])));
	RValue recipes = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Ui), RValue("blacksmith_recipes") });
	if (!recipes.IsArray())
		return -1;

	std::vector<RValue> list = recipes.ToVector();
	for (size_t i = 0; i < list.size(); i++)
	{
		if (!list[i].IsArray())
			continue;
		std::vector<RValue> fields = list[i].ToVector();
		if (fields.size() < 5 || static_cast<int>(ToNumber(fields[2])) != Type)
			continue;
		std::string recipe_code = fields.size() > 5 && fields[5].IsString() ? fields[5].ToString() : "";
		if (recipe_code == code)
			return static_cast<int>(i);
	}
	return -1;
}

// Temporary: find where material counts are kept.
static void LogStockCandidates()
{
	CInstance* global_instance = nullptr;
	if (!AurieSuccess(g_Yytk->GetGlobalInstance(&global_instance)))
		return;
	g_Yytk->EnumInstanceMembers(RValue(global_instance), [](const char* Name, RValue* Value) -> bool
	{
		std::string name = Name;
		bool wanted = name.find("slorm") != std::string::npos || name.find("invent") != std::string::npos
			|| name.find("shared") != std::string::npos || name.find("gold") != std::string::npos;
		if (!wanted || !Value)
			return false;
		std::string text;
		if (Value->IsArray())
		{
			std::vector<RValue> items = Value->ToVector();
			text = "array[" + std::to_string(items.size()) + "]";
			for (size_t i = 0; i < items.size() && i < 12; i++)
			{
				if (items[i].IsArray())
				{
					std::vector<RValue> inner = items[i].ToVector();
					text += " [" + std::to_string(inner.size()) + ":";
					for (size_t j = 0; j < inner.size() && j < 12; j++)
						text += " " + (inner[j].IsArray() ? "[..]" : inner[j].ToString());
					text += "]";
				}
				else text += " " + items[i].ToString();
			}
		}
		else if (Value->m_Kind == VALUE_REAL || Value->m_Kind == VALUE_STRING || Value->m_Kind == VALUE_INT32 || Value->m_Kind == VALUE_INT64)
			text = Value->ToString();
		else
			text = "<" + Value->GetKindName() + ">";
		Log("global.%s = %.700s", Name, text.c_str());
		return false;
	});
}

// No config targets: take the scores recipe selected in the panel and aim every stat it rerolls at max.
static bool TargetsFromSelectedRecipe(CInstance* Self, CInstance* Other)
{
	RValue recipes = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_recipes") });
	RValue selected = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_selected_recipe") });
	if (!recipes.IsArray() || !selected.IsNumberConvertible() || !ReadSlotItem(Self, Other))
	{
		Log("open the reforge panel with an item in the slot first");
		return false;
	}

	std::vector<RValue> list = recipes.ToVector();
	size_t index = static_cast<size_t>(selected.ToDouble());
	if (index >= list.size() || !list[index].IsArray())
		return false;
	std::vector<RValue> fields = list[index].ToVector();
	if (fields.size() < 5 || static_cast<int>(ToNumber(fields[2])) != 0)
	{
		Log("select a 'Reforge Scores' recipe first (selected: %s)", fields.empty() ? "?" : fields[0].ToString().c_str());
		return false;
	}

	std::string tier;
	if (fields.size() > 5 && fields[5].IsString())
		tier = std::string(1, static_cast<char>(std::toupper(fields[5].ToString()[0])));
	g_AllScores = tier.empty();

	for (const Affix& affix : g_Item)
	{
		if (affix.locked || (!tier.empty() && affix.tier != tier))
			continue;
		if (affix.tier != "N" && affix.tier != "D" && affix.tier != "M" && affix.tier != "R" && affix.tier != "E")
			continue;
		Target target;
		target.tier = affix.tier;
		target.stat = affix.stat;
		target.goal = Goal::MaxRoll;
		g_Targets.push_back(target);
	}
	Log("using selected recipe '%s'", fields[0].ToString().c_str());
	return !g_Targets.empty();
}

static void Tick(CInstance* Self, CInstance* Other)
{
	bool key_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
	if (key_down && !g_KeyWasDown)
	{
		if (g_Running)
		{
			Stop("F6 pressed");
		}
		else
		{
			LoadConfig();
			g_AllScores = false;
			if (g_Targets.empty() && !TargetsFromSelectedRecipe(Self, Other))
			{
				Log("nothing to do");
			}
			else
			{
				g_Running = true;
				g_Attempts = 0;
				g_Cooldown = 0;
				Log("started, %d targets, max %d reforges", static_cast<int>(g_Targets.size()), g_MaxAttempts);
				LogStockCandidates();
				RValue recipes = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_recipes") });
				if (recipes.IsArray())
				{
					for (const RValue& recipe : recipes.ToVector())
					{
						std::string fields;
						if (recipe.IsArray())
							for (const RValue& field : recipe.ToVector()) fields += field.ToString() + " ; ";
						Log("recipe: %s", fields.c_str());
					}
				}
			}
		}
	}
	g_KeyWasDown = key_down;

	if (!g_Running)
		return;
	if (g_Cooldown > 0)
	{
		g_Cooldown--;
		return;
	}
	if (!ReadSlotItem(Self, Other))
		return Stop("no single item found in the reforge slot");

	for (const Affix& affix : g_Item)
		Log("  %s %s roll %g shown %g%s", affix.tier.c_str(), affix.stat.c_str(), affix.roll, affix.shown, affix.pure > 100 ? " pure" : "");

	// First unmet target decides the next recipe.
	const Target* pending = nullptr;
	int recipe_type = 0;
	for (const Target& target : g_Targets)
	{
		const Affix* match = nullptr;
		for (const Affix& affix : g_Item)
			if (affix.tier == target.tier && affix.stat == target.stat) match = &affix;

		if (match && Met(target, *match))
			continue;
		pending = &target;
		recipe_type = match ? 0 : 1;
		break;
	}
	if (!pending)
		return Stop("all targets met");
	if (g_Attempts >= g_MaxAttempts)
		return Stop("max_attempts reached");

	// Both recipe types reroll every unlocked stat of the tier.
	for (const Affix& affix : g_Item)
	{
		if ((!g_AllScores && affix.tier != pending->tier) || affix.locked)
			continue;
		if (affix.pure > 100 && !g_AllowPureLoss)
			return Stop("tier has a pure stat (set allow_pure_loss 1 to reroll it)");
	}

	int recipe = FindRecipe(Self, recipe_type, g_AllScores ? "" : pending->tier);
	if (recipe < 0)
		return Stop("recipe not offered for this item");

	g_Yytk->CallBuiltin("variable_instance_set", { RValue(Self), RValue("blacksmith_selected_recipe"), RValue(recipe) });

	RValue result;
	g_Apply(Self, Other, result, 0, nullptr);
	g_Attempts++;
	g_Cooldown = 3;

	RValue applied = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_last_applied_recipe") });
	if (!applied.IsArray())
		return Stop("the game did not apply the recipe");

	Log("reforge %d: %s %s for %s", g_Attempts, pending->tier.c_str(), recipe_type ? "stats" : "scores", pending->stat.c_str());
}

// Runs once per frame, after the UI object's Step event.
static void CodeCallback(FWCodeEvent& Event)
{
	auto& [self, other, code, argument_count, arguments] = Event.Arguments();
	if (!code || !code->GetName() || std::string_view(code->GetName()) != "gml_Object_obj_ui_next_gen_Step_0")
		return;

	static bool announced = false;
	if (!announced)
	{
		announced = true;
		Log("step callback active");
	}

	Event.Call();
	Tick(self, other);
}

static PFUNC_YYGMLScript FindScript(const char* Name)
{
	CScript* script = nullptr;
	AurieStatus status = g_Yytk->GetNamedRoutinePointer(Name, reinterpret_cast<PVOID*>(&script));
	if (!AurieSuccess(status) || !script || !script->m_Functions)
		return nullptr;
	return script->m_Functions->m_ScriptFunction;
}

EXPORTED AurieStatus ModuleInitialize(
	IN AurieModule* Module,
	IN const fs::path& ModulePath
)
{
	g_Yytk = YYTK::GetInterface();
	if (!g_Yytk)
		return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;

	g_ConfigPath = ModulePath.parent_path() / "SlormReforger.txt";
	LoadConfig();

	g_StatFromScore = FindScript("gml_Script_scr_loot_stat_from_score");
	g_Apply = FindScript("gml_Script_scr_blacksmith_recipe_apply");
	if (!g_StatFromScore || !g_Apply)
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[SlormReforger] game scripts not found");
		return AURIE_OBJECT_NOT_FOUND;
	}

	AurieStatus status = g_Yytk->CreateCallback(Module, EVENT_OBJECT_CALL, CodeCallback, 0);
	if (!AurieSuccess(status))
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[SlormReforger] callback failed: %s", AurieStatusToString(status));
		return status;
	}

	Log("loaded, F6 at the reforge panel starts/stops, config %s", g_ConfigPath.string().c_str());
	return AURIE_SUCCESS;
}
