#include <YYToolkit/YYTK_Shared.hpp>
#include <array>
#include <string>
#include <vector>
#include <utility>
using namespace Aurie;
using namespace YYTK;

static YYTKInterface* g_Yytk = nullptr;

// Scripts to trace. Names are from the game exe.
static constexpr std::array g_Scripts = {
	"gml_Script_scr_blacksmith_recipe_apply",
	"gml_Script_scr_blacksmith_recipe_select",
	"gml_Script_scr_blacksmith_recipe_confirm",
	"gml_Script_scr_blacksmith_recipe_confirm_true",
	"gml_Script_scr_loot_roll_stats",
	"gml_Script_scr_loot_roll_score",
	"gml_Script_scr_loot_roll_extra",
	"gml_Script_scr_loot_erase_on_reroll",
};

static std::array<PFUNC_YYGMLScript, g_Scripts.size()> g_Originals = {};
static int g_ApplyDepth = 0;

static std::string Describe(const RValue& Value, int Depth = 0)
{
	switch (Value.m_Kind)
	{
	case VALUE_REAL:
	case VALUE_INT32:
	case VALUE_INT64:
	case VALUE_BOOL:
		return Value.ToString();
	case VALUE_STRING:
		return "\"" + Value.ToString() + "\"";
	case VALUE_ARRAY:
	{
		if (Depth >= 3)
			return "[...]";
		std::vector<RValue> items = Value.ToVector();
		std::string out = "[";
		for (size_t i = 0; i < items.size(); i++)
		{
			if (i) out += ", ";
			if (i >= 64) { out += "... " + std::to_string(items.size()) + " total"; break; }
			out += Describe(items[i], Depth + 1);
		}
		return out + "]";
	}
	default:
		return "<" + Value.GetKindName() + ">";
	}
}

// Aurie's logger truncates long lines.
static void LogLong(const char* Prefix, const std::string& Text)
{
	for (size_t at = 0; at < Text.size() || at == 0; at += 900)
	{
		DbgPrintEx(LOG_SEVERITY_INFO, "[SlormReforger] %s %s", Prefix, Text.substr(at, 900).c_str());
		if (Text.empty()) break;
	}
}

static std::vector<PVOID> g_SeenItems;

static void NoteItem(const RValue& Value)
{
	if (Value.m_Kind != VALUE_ARRAY || !Value.m_Pointer)
		return;
	for (PVOID seen : g_SeenItems)
		if (seen == Value.m_Pointer) return;
	g_SeenItems.push_back(Value.m_Pointer);
}

// Logs every path under Value that holds one of the item arrays seen during the apply.
static void FindItems(const std::string& Path, RValue& Value, int Depth)
{
	if (Value.m_Kind != VALUE_ARRAY || !Value.m_Pointer)
		return;
	for (PVOID seen : g_SeenItems)
	{
		if (seen == Value.m_Pointer)
		{
			LogLong(("item at " + Path).c_str(), Describe(Value));
			return;
		}
	}
	if (Depth >= 5)
		return;
	std::vector<RValue*> items = Value.ToRefVector();
	for (size_t i = 0; i < items.size(); i++)
		if (items[i]) FindItems(Path + "[" + std::to_string(i) + "]", *items[i], Depth + 1);
}

static void SearchInstance(const char* Label, CInstance* Instance)
{
	if (!Instance)
		return;
	RValue object_index = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Instance), RValue("object_index") });
	RValue object_name;
	if (object_index.m_Kind == VALUE_REAL || object_index.m_Kind == VALUE_INT32 || object_index.m_Kind == VALUE_INT64)
		object_name = g_Yytk->CallBuiltin("object_get_name", { object_index });
	int count = 0;
	g_Yytk->EnumInstanceMembers(RValue(Instance), [&](const char* Name, RValue* Value) -> bool
	{
		count++;
		if (Value) FindItems(std::string(Label) + "." + Name, *Value, 0);
		return false;
	});
	DbgPrintEx(LOG_SEVERITY_INFO, "[SlormReforger] searched %s (%s), %d members", Label, Describe(object_name).c_str(), count);
}

static void LogMember(CInstance* Instance, const char* Name)
{
	RValue value = g_Yytk->CallBuiltin("variable_instance_get", { RValue(Instance), RValue(Name) });
	LogLong(Name, Describe(value));
}

template <size_t Index>
static RValue& TraceHook(CInstance* Self, CInstance* Other, RValue& Result, int ArgumentCount, RValue** Arguments)
{
	const char* name = g_Scripts[Index] + 11;
	bool is_apply = Index == 0;

	// Outside an apply these run every frame for the outcome preview.
	if (!is_apply && Index >= 4 && g_ApplyDepth == 0)
		return g_Originals[Index](Self, Other, Result, ArgumentCount, Arguments);

	std::string args;
	for (int i = 0; i < ArgumentCount; i++)
	{
		if (i) args += ", ";
		args += Arguments[i] ? Describe(*Arguments[i]) : "null";
		if (g_ApplyDepth > 0 && Arguments[i]) NoteItem(*Arguments[i]);
	}
	LogLong((std::string("> ") + name).c_str(), "(" + args + ")");

	if (is_apply)
	{
		g_SeenItems.clear();
		LogMember(Self, "blacksmith_selected_recipe");
		LogMember(Self, "blacksmith_recipes");
		LogMember(Self, "craft_slot");
		g_ApplyDepth++;
	}

	RValue& ret = g_Originals[Index](Self, Other, Result, ArgumentCount, Arguments);

	if (g_ApplyDepth > 0 && !is_apply)
		NoteItem(Result);

	if (is_apply)
	{
		g_ApplyDepth--;
		LogMember(Self, "blacksmith_last_applied_recipe");
		SearchInstance("self", Self);
		if (Other != Self) SearchInstance("other", Other);
		CInstance* global_instance = nullptr;
		if (AurieSuccess(g_Yytk->GetGlobalInstance(&global_instance)))
			SearchInstance("global", global_instance);
	}

	LogLong((std::string("< ") + name).c_str(), Describe(Result));
	return ret;
}

template <size_t Index>
static void InstallHook(AurieModule* Module)
{
	CScript* script = nullptr;
	AurieStatus status = g_Yytk->GetNamedRoutinePointer(g_Scripts[Index], reinterpret_cast<PVOID*>(&script));
	if (!AurieSuccess(status) || !script || !script->m_Functions)
	{
		DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormReforger] script not found: %s", g_Scripts[Index]);
		return;
	}

	status = MmCreateHook(
		Module,
		g_Scripts[Index],
		script->m_Functions->m_ScriptFunction,
		TraceHook<Index>,
		reinterpret_cast<PVOID*>(&g_Originals[Index])
	);
	if (!AurieSuccess(status))
		DbgPrintEx(LOG_SEVERITY_WARNING, "[SlormReforger] hook failed: %s", g_Scripts[Index]);
}

template <size_t... Indices>
static void InstallHooks(AurieModule* Module, std::index_sequence<Indices...>)
{
	(InstallHook<Indices>(Module), ...);
}

EXPORTED AurieStatus ModuleInitialize(
	IN AurieModule* Module,
	IN const fs::path& ModulePath
)
{
	UNREFERENCED_PARAMETER(ModulePath);

	g_Yytk = YYTK::GetInterface();
	if (!g_Yytk)
		return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;

	InstallHooks(Module, std::make_index_sequence<g_Scripts.size()>{});
	DbgPrintEx(LOG_SEVERITY_INFO, "[SlormReforger] loaded");
	return AURIE_SUCCESS;
}
