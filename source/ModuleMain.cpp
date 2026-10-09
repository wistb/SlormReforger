#include "Reforger.hpp"
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace Aurie;
using namespace YYTK;

static YYTKInterface* g_Yytk = nullptr;
static fs::path g_ConfigPath;

static PFUNC_YYGMLScript g_StatFromScore = nullptr;
static PFUNC_YYGMLScript g_Apply = nullptr;
static PFUNC_YYGMLScript g_CurrencySpend = nullptr;

std::recursive_mutex g_Lock;

std::vector<Target> g_Targets;
int g_MaxAttempts = 50;
bool g_AllowPureLoss = false;
int g_MinStock = 0;

bool g_Visible = true;
bool g_Running = false;
bool g_WantStart = false;
bool g_WantStop = false;
int g_Attempts = 0;
std::string g_Status = "idle";
std::deque<std::string> g_Notes;

bool g_HasItem = false;
static bool g_PanelOpen = false;
std::vector<Affix> g_Item;
std::string g_ItemSlot;
int g_ItemLevel = 0;
std::vector<Recipe> g_Recipes;
double g_Stock[MATERIAL_COUNT] = {};
double g_Gold = 0;

// Targets of the current run; g_Targets, or derived from the selected recipe.
static std::vector<Target> g_RunTargets;
static bool g_KeyWasDown = false;
static int g_Cooldown = 0;
static int g_Refresh = 0;

// The blacksmith's reforge slot in global.inventory[hero].
static constexpr size_t REFORGE_SLOT = 666;

template <typename... Args>
static void Log(const char* Format, Args... Arguments)
{
	DbgPrintEx(LOG_SEVERITY_INFO, (std::string("[SlormReforger] ") + Format).c_str(), Arguments...);
}

// Also shown in the overlay.
static void Note(const char* Format, ...)
{
	char text[256];
	va_list arguments;
	va_start(arguments, Format);
	vsnprintf(text, sizeof(text), Format, arguments);
	va_end(arguments);
	Log("%s", text);
	g_Notes.emplace_back(text);
	if (g_Notes.size() > 60)
		g_Notes.pop_front();
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
	g_ItemSlot = header.size() > 1 ? header[1].ToString() : "";

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
		// Fetch the one slot; copying all 674 is slow.
		if (ToNumber(g_Yytk->CallBuiltin("array_length", { hero })) <= REFORGE_SLOT)
			continue;
		RValue slot = g_Yytk->CallBuiltin("array_get", { hero, RValue(static_cast<double>(REFORGE_SLOT)) });
		if (IsItem(slot))
		{
			item = slot;
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
	g_MinStock = 0;

	std::ifstream file(g_ConfigPath);
	if (!file)
		return;

	std::string line;
	while (std::getline(file, line))
	{
		std::istringstream words(line);
		std::string first;
		if (!(words >> first) || first[0] == '#')
			continue;
		if (first == "max_attempts") { words >> g_MaxAttempts; continue; }
		if (first == "min_stock") { words >> g_MinStock; continue; }
		if (first == "allow_pure_loss") { int flag = 0; words >> flag; g_AllowPureLoss = flag != 0; continue; }
		Target target;
		target.tier = first;
		std::string goal;
		if (!(words >> target.stat))
			continue;
		if (!(words >> goal))
			goal = "any";
		try
		{
			if (goal == "any") target.goal = Goal::Any;
			else if (goal == "max") target.goal = Goal::MaxRoll;
			else if (goal.rfind("roll:", 0) == 0) { target.goal = Goal::Roll; target.amount = std::stod(goal.substr(5)); }
			else { target.goal = Goal::Value; target.amount = std::stod(goal); }
		}
		catch (...) { continue; }
		g_Targets.push_back(target);
	}
}

void SaveConfig()
{
	std::ofstream file(g_ConfigPath);
	file << "# Written by the overlay. One target per line: TIER STAT GOAL\n"
		"# GOAL is any, max, a number (displayed value to reach), or roll:N.\n";
	for (const Target& target : g_Targets)
	{
		file << target.tier << ' ' << target.stat << ' ';
		switch (target.goal)
		{
		case Goal::Any: file << "any"; break;
		case Goal::MaxRoll: file << "max"; break;
		case Goal::Roll: file << "roll:" << target.amount; break;
		default: file << target.amount; break;
		}
		file << '\n';
	}
	file << "max_attempts " << g_MaxAttempts << '\n'
		<< "min_stock " << g_MinStock << '\n'
		<< "allow_pure_loss " << (g_AllowPureLoss ? 1 : 0) << '\n';
}

// Best roll for a tier. Percent stats scale down on low-level items.
double MaxRoll(const std::string& Tier, const std::string& Stat)
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
	case Goal::Any: return true;
	case Goal::MaxRoll: return Affix.roll >= MaxRoll(Target.tier, Target.stat);
	case Goal::Roll: return Affix.roll >= Target.amount;
	default: return Affix.has_shown && Affix.shown >= Target.amount;
	}
}

// 0 met, 1 still to do, 2 cannot be reached by reforging.
int TargetState(const Target& Target, std::string& Text)
{
	const Affix* match = nullptr;
	const Affix* elsewhere = nullptr;
	int in_tier = 0, unlocked = 0;
	for (const Affix& affix : g_Item)
	{
		if (affix.tier == Target.tier)
		{
			in_tier++;
			if (!affix.locked) unlocked++;
			if (affix.stat == Target.stat) match = &affix;
		}
		else if (affix.stat == Target.stat) elsewhere = &affix;
	}

	char text[96];
	if (match)
	{
		if (Met(Target, *match))
		{
			Text = match->locked ? "met [locked]" : "met";
			return 0;
		}
		if (match->locked)
		{
			Text = "locked, its roll cannot change";
			return 2;
		}
		snprintf(text, sizeof(text), "roll %g of %g", match->roll, MaxRoll(Target.tier, Target.stat));
		Text = text;
		return 1;
	}
	if (elsewhere)
	{
		Text = std::string("already on the item as a ") + TierName(elsewhere->tier) + " stat";
		return 2;
	}
	if (in_tier == 0)
	{
		Text = std::string("the item has no ") + TierName(Target.tier) + " stat to reroll";
		return 2;
	}
	if (unlocked == 0)
	{
		Text = std::string("every ") + TierName(Target.tier) + " stat is locked";
		return 2;
	}
	Text = "not on the item yet, stats will be rerolled";
	return 1;
}

static void Stop(const char* Reason)
{
	Note("stopped after %d reforges: %s", g_Attempts, Reason);
	g_Status = Reason;
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

const char* MaterialName(int Id)
{
	static const char* names[] = {
		"Normal Slormeline", "Magic Slormeline", "Rare Slormeline", "Epic Slormeline", "Legendary Slormeline",
		"Slormandrite of Fate", "Slormandrite of Negation", "Slormandrite of True Potential",
		"Slormandrite of Aptitude", "Slormandrite of Harmony", "Slormandrite of Virtue",
	};
	return Id >= 0 && Id < MATERIAL_COUNT ? names[Id] : "unknown material";
}

// With the third argument set the game only reports the count; the panel calls it this way every frame.
static double MaterialStock(CInstance* Self, CInstance* Other, int Id)
{
	RValue arguments[4] = { RValue(static_cast<double>(Id)), RValue(1.0), RValue(1.0), RValue("slormite") };
	RValue* pointers[4] = { &arguments[0], &arguments[1], &arguments[2], &arguments[3] };
	RValue count;
	g_CurrencySpend(Self, Other, count, 4, pointers);
	return ToNumber(count);
}

// Recipe materials are ids joined by '|', one of each; the fifth field is the goldus cost.
static bool CanAfford(CInstance* Self, CInstance* Other, int Recipe, std::string& Why)
{
	RValue recipes = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_recipes") });
	if (!recipes.IsArray())
		return false;
	std::vector<RValue> list = recipes.ToVector();
	if (Recipe < 0 || static_cast<size_t>(Recipe) >= list.size() || !list[Recipe].IsArray())
		return false;
	std::vector<RValue> fields = list[Recipe].ToVector();
	if (fields.size() < 5)
		return false;

	std::string materials = fields[3].ToString();
	if (materials.find('*') != std::string::npos)
	{
		Why = "recipe cost format not understood: " + materials;
		return false;
	}

	std::istringstream ids(materials);
	std::string id_text;
	while (std::getline(ids, id_text, '|'))
	{
		int id = 0;
		try { id = std::stoi(id_text); }
		catch (...) { Why = "recipe cost format not understood: " + materials; return false; }

		double stock = MaterialStock(Self, Other, id);
		if (stock - 1 < g_MinStock)
		{
			Why = std::string("not enough ") + MaterialName(id) + " (have " + std::to_string(static_cast<long long>(stock)) + ", keeping " + std::to_string(g_MinStock) + ")";
			return false;
		}
	}

	double cost = ToNumber(fields[4]);
	double gold = ToNumber(g_Yytk->CallBuiltin("variable_global_get", { RValue("gold") }));
	if (gold < cost)
	{
		Why = "not enough goldus";
		return false;
	}
	return true;
}

// No targets set: take the scores recipe selected in the panel and aim every stat it rerolls at max.
static bool TargetsFromSelectedRecipe(CInstance* Self, CInstance* Other)
{
	RValue recipes = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_recipes") });
	RValue selected = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_selected_recipe") });
	if (!recipes.IsArray() || !selected.IsNumberConvertible() || !ReadSlotItem(Self, Other))
	{
		Note("open the reforge panel with an item in the slot first");
		return false;
	}

	std::vector<RValue> list = recipes.ToVector();
	size_t index = static_cast<size_t>(selected.ToDouble());
	if (index >= list.size() || !list[index].IsArray())
		return false;
	std::vector<RValue> fields = list[index].ToVector();
	if (fields.size() < 5 || static_cast<int>(ToNumber(fields[2])) != 0)
	{
		Note("add a target, or select a 'Reforge Scores' recipe (selected: %s)", fields.empty() ? "?" : fields[0].ToString().c_str());
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
		g_RunTargets.push_back(target);
	}
	Note("using selected recipe '%s'", fields[0].ToString().c_str());
	return !g_RunTargets.empty();
}

// What the overlay shows: item, recipes, stock.
static void Refresh(CInstance* Self, CInstance* Other)
{
	// The slot keeps its item when the menu closes, so also require Friedrich's reforge tab.
	RValue citizen = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("menu_citizen") });
	RValue panel = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("merchant_panel") });
	g_PanelOpen = citizen.IsString() && citizen.ToString() == "blacksmith" && ToNumber(panel) == 0;
	g_HasItem = g_PanelOpen && ReadSlotItem(Self, Other);
	g_Recipes.clear();
	if (!g_HasItem)
		return;

	RValue recipes = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_recipes") });
	if (recipes.IsArray())
	{
		for (const RValue& entry : recipes.ToVector())
		{
			if (!entry.IsArray())
				continue;
			std::vector<RValue> fields = entry.ToVector();
			if (fields.size() < 5)
				continue;
			Recipe recipe;
			recipe.label = fields[0].ToString();
			recipe.type = static_cast<int>(ToNumber(fields[2]));
			recipe.materials = fields[3].ToString();
			recipe.gold = ToNumber(fields[4]);
			if (fields.size() > 5 && fields[5].IsString())
				recipe.tier = fields[5].ToString();
			g_Recipes.push_back(recipe);
		}
	}

	// Only ask about materials the reforge recipes use.
	for (const Recipe& recipe : g_Recipes)
	{
		if (recipe.type != 0 && recipe.type != 1)
			continue;
		std::istringstream ids(recipe.materials);
		std::string id_text;
		while (std::getline(ids, id_text, '|'))
		{
			int id = atoi(id_text.c_str());
			if (id_text.find_first_not_of("0123456789") == std::string::npos && !id_text.empty() && id < MATERIAL_COUNT)
				g_Stock[id] = MaterialStock(Self, Other, id);
		}
	}
	g_Gold = ToNumber(g_Yytk->CallBuiltin("variable_global_get", { RValue("gold") }));
}

static void Start(CInstance* Self, CInstance* Other)
{
	g_AllScores = false;
	g_RunTargets = g_Targets;
	if (g_RunTargets.empty() && !TargetsFromSelectedRecipe(Self, Other))
	{
		g_Status = "nothing to do";
		return;
	}
	g_Running = true;
	g_Attempts = 0;
	g_Cooldown = 0;
	g_Status = "running";
	Note("started, %d targets, max %d reforges", static_cast<int>(g_RunTargets.size()), g_MaxAttempts);
}

static void Tick(CInstance* Self, CInstance* Other)
{
	std::lock_guard guard(g_Lock);

	bool key_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
	if (key_down && !g_KeyWasDown && GetForegroundWindow() == GetActiveWindow())
	{
		g_Visible = !g_Visible;
		Log("overlay %s", g_Visible ? "shown" : "hidden");
	}
	g_KeyWasDown = key_down;

	if (g_WantStop && g_Running)
		Stop("stopped by you");
	if (g_WantStart && !g_Running)
		Start(Self, Other);
	g_WantStart = g_WantStop = false;

	if (!g_Running)
	{
		if (g_Visible && --g_Refresh <= 0)
		{
			g_Refresh = 20;
			Refresh(Self, Other);
		}
		return;
	}
	if (g_Cooldown > 0)
	{
		g_Cooldown--;
		return;
	}
	Refresh(Self, Other);
	if (!g_PanelOpen)
		return Stop("the reforge panel was closed");
	if (!g_HasItem)
		return Stop("no single item found in the reforge slot");

	for (const Affix& affix : g_Item)
		Log("  %s %s roll %g shown %g%s", affix.tier.c_str(), affix.stat.c_str(), affix.roll, affix.shown, affix.pure > 100 ? " pure" : "");

	// First unmet target decides the next recipe.
	const Target* pending = nullptr;
	int recipe_type = 0;
	for (const Target& target : g_RunTargets)
	{
		const Affix* match = nullptr;
		for (const Affix& affix : g_Item)
			if (affix.tier == target.tier && affix.stat == target.stat) match = &affix;

		if (match && Met(target, *match))
			continue;
		std::string problem;
		if (TargetState(target, problem) == 2)
			return Stop((StatName(target.stat) + ": " + problem).c_str());
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
			return Stop("tier has a pure stat (allow rerolling pure stats to continue)");
	}

	int recipe = FindRecipe(Self, recipe_type, g_AllScores ? "" : pending->tier);
	if (recipe < 0)
		return Stop("recipe not offered for this item");

	std::string why = "could not read the recipe cost";
	if (!CanAfford(Self, Other, recipe, why))
		return Stop(why.c_str());

	g_Yytk->CallBuiltin("variable_instance_set", { RValue(Self), RValue("blacksmith_selected_recipe"), RValue(recipe) });

	RValue result;
	g_Apply(Self, Other, result, 0, nullptr);
	g_Attempts++;
	g_Cooldown = 3;

	RValue applied = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Self), RValue("blacksmith_last_applied_recipe") });
	if (!applied.IsArray())
		return Stop("the game did not apply the recipe");

	Note("reforge %d: %s %s for %s", g_Attempts, pending->tier.c_str(), recipe_type ? "stats" : "scores", StatName(pending->stat).c_str());
}

// Runs once per frame, after the UI object's Step event.
static void CodeCallback(FWCodeEvent& Event)
{
	// This runs for every event of every object, so compare by pointer once the event is known.
	static CCode* step_code = nullptr;
	auto& [self, other, code, argument_count, arguments] = Event.Arguments();
	if (code != step_code || !code)
	{
		if (step_code || !code || !code->GetName() || std::string_view(code->GetName()) != "gml_Object_obj_ui_next_gen_Step_0")
			return;
		step_code = code;
	}

	// The swap chain only exists once the game is running.
	static bool installed = false;
	if (!installed)
	{
		installed = true;
		Log("step callback active, overlay hook %s", OverlayInstall(g_Yytk) ? "installed" : "FAILED");
	}

	// Input aimed at the overlay must not reach the game's UI.
	if (OverlayWantsMouse())
	{
		g_Yytk->CallBuiltin("mouse_clear", { RValue(1.0) });
		g_Yytk->CallBuiltin("mouse_clear", { RValue(2.0) });
	}
	if (OverlayWantsKeys())
		g_Yytk->CallBuiltin("io_clear", {});

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
	LoadGameData();
	OverlaySetIniPath((ModulePath.parent_path() / "SlormReforger.layout.ini").string());

	g_StatFromScore = FindScript("gml_Script_scr_loot_stat_from_score");
	g_Apply = FindScript("gml_Script_scr_blacksmith_recipe_apply");
	g_CurrencySpend = FindScript("gml_Script_scr_currency_spend");
	if (!g_StatFromScore || !g_Apply || !g_CurrencySpend)
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


	Log("loaded, %d stats, F6 shows/hides the overlay, config %s", static_cast<int>(g_Stats.size()), g_ConfigPath.string().c_str());
	return AURIE_SUCCESS;
}
