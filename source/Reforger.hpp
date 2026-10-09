#pragma once
#include <YYToolkit/YYTK_Shared.hpp>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct Affix
{
	std::string tier;
	std::string stat;
	double roll = 0;
	bool locked = false;
	double pure = 0;
	// Position in the item array; lock recipes refer to it.
	int index = 0;
	// Displayed value, as the game computes it.
	double shown = 0;
	bool has_shown = false;
};

enum class Goal { Any, MaxRoll, Value, Roll };

struct Target
{
	std::string tier;
	std::string stat;
	Goal goal = Goal::MaxRoll;
	double amount = 0;
};

struct Recipe
{
	std::string label;
	std::string detail;
	int type = 0;
	std::string materials;
	double gold = 0;
	std::string tier;
};

// One row of dat_sta.json; columns kept as text.
struct StatInfo
{
	std::string ref;
	// name is label plus " %" for percent stats; label is the game's own text.
	std::string name;
	std::string label;
	std::map<std::string, std::string> columns;
};

// One stat a tier can roll on the slotted item, as the game lists it.
struct PoolEntry
{
	std::string stat;
	double min = 0;
	double max = 0;
	bool on_item = false;
};

// Overlay appearance, saved with the rest of the config.
struct Appearance
{
	int theme = 0;
	bool custom_accent = false;
	float accent[3] = { 0.78f, 0.62f, 0.25f };
	// Title bar, tabs, input fields and table headers.
	bool custom_primary = false;
	float primary[3] = { 0.16f, 0.29f, 0.48f };
	float opacity = 0.95f;
	float scale = 1.0f;
	bool show_costs = true;
	// Virtual-key code that shows or hides the overlay.
	int hotkey = 0x75;
};

constexpr int MATERIAL_COUNT = 11;

// Guards everything below; the game step writes it, the overlay reads and edits it.
extern std::recursive_mutex g_Lock;

extern std::vector<Target> g_Targets;
extern int g_MaxAttempts;
extern bool g_AllowPureLoss;
// Reserve kept of each material, by material id.
extern int g_MinStock[];
extern bool g_AutoLock;
extern Appearance g_Appearance;
// Set while the hotkey is being rebound, so the key press does not also toggle the overlay.
extern bool g_SuppressToggle;

extern bool g_Visible;
extern bool g_Running;
extern bool g_WantStart;
extern bool g_WantStop;
extern int g_Attempts;
extern std::string g_Status;
extern std::deque<std::string> g_Notes;

extern bool g_HasItem;
extern std::vector<Affix> g_Item;
extern std::string g_ItemSlot;
extern int g_ItemLevel;
extern std::vector<Recipe> g_Recipes;
extern std::map<std::string, std::vector<PoolEntry>> g_Pools;
extern double g_Stock[MATERIAL_COUNT];
extern double g_Gold;

extern std::vector<StatInfo> g_Stats;

const PoolEntry* FindInPool(const std::string& Tier, const std::string& Stat);
double MaxRoll(const std::string& Tier, const std::string& Stat);
const char* MaterialName(int Id);
void SaveConfig();
std::vector<std::pair<int, int>> ParseCost(const std::string& Spec);
int TargetState(const Target& Target, std::string& Text);
const char* TierName(const std::string& Code);

void LoadGameData();
const StatInfo* FindStat(const std::string& Ref);
std::string StatName(const std::string& Ref);

bool OverlayInstall(YYTK::YYTKInterface* Yytk);
bool OverlayWantsMouse();
bool OverlayWantsKeys();
void OverlaySetIniPath(const std::string& Path);
