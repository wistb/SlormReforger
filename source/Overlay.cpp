#include "Reforger.hpp"
#include "Version.hpp"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <algorithm>
using namespace YYTK;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool g_Ready = false;
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static std::string g_IniPath;
static HWND g_Window = nullptr;
static WNDPROC g_OriginalProc = nullptr;
// Latched once per frame so the game sees a steady answer.
static bool g_HideMouse = false;
using CursorFn = BOOL(WINAPI*)(LPPOINT);
static CursorFn g_OriginalCursor = nullptr;

static BOOL RealCursor(LPPOINT Point)
{
	return g_OriginalCursor ? g_OriginalCursor(Point) : GetCursorPos(Point);
}

// While the cursor is over the overlay, the game is told it is off screen, so nothing behind it hovers.
static BOOL WINAPI HookedCursor(LPPOINT Point)
{
	BOOL ok = g_OriginalCursor(Point);
	if (ok && Point && g_HideMouse)
		Point->x = Point->y = -20000;
	return ok;
}
static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);

// Target being composed in the "add" row.
static int g_NewTier = 1;
static std::string g_NewStat;
static int g_NewGoal = 0;
static double g_NewAmount = 0;
static bool g_AllStats = false;

static const char* TIER_CODES[] = { "N", "D", "M", "R", "E" };
static const char* TIER_NAMES[] = { "Normal", "Defense", "Magic", "Rare", "Epic" };
static const char* GOAL_NAMES[] = { "Stat only", "Max roll", "Value at least", "Roll at least" };

void OverlaySetIniPath(const std::string& Path)
{
	g_IniPath = Path;
}

const char* TierName(const std::string& Code)
{
	for (int i = 0; i < 5; i++)
		if (Code == TIER_CODES[i]) return TIER_NAMES[i];
	return Code == "L" ? "Legendary" : Code.c_str();
}

// Rarity colours as the game shows them.
static ImVec4 TierColor(const std::string& Code)
{
	if (Code == "M") return { 46 / 255.0f, 135 / 255.0f, 38 / 255.0f, 1.0f };
	if (Code == "R") return { 23 / 255.0f, 106 / 255.0f, 177 / 255.0f, 1.0f };
	if (Code == "E") return { 198 / 255.0f, 141 / 255.0f, 32 / 255.0f, 1.0f };
	if (Code == "L") return { 206 / 255.0f, 70 / 255.0f, 7 / 255.0f, 1.0f };
	return { 137 / 255.0f, 137 / 255.0f, 137 / 255.0f, 1.0f };
}

static const ImVec4 PURE_COLOR = { 70 / 255.0f, 231 / 255.0f, 176 / 255.0f, 1.0f };

// Which stats a tier can roll on the item in the slot, from the game's own list.
static bool InPool(const StatInfo& Stat, const std::string& Tier)
{
	if (g_AllStats || !g_HasItem)
		return true;
	auto pool = g_Pools.find(Tier);
	if (pool == g_Pools.end() || pool->second.empty())
		return true;
	return FindInPool(Tier, Stat.ref) != nullptr;
}

static std::string Describe(const Target& Target)
{
	std::string text = std::string(TierName(Target.tier)) + ": " + StatName(Target.stat);
	char amount[32];
	snprintf(amount, sizeof(amount), "%g", Target.amount);
	switch (Target.goal)
	{
	case Goal::Any: return text + ", any roll";
	case Goal::MaxRoll: return text + ", max roll";
	case Goal::Roll: return text + ", roll at least " + amount;
	default: return text + ", value at least " + amount;
	}
}

// One target per tier and stat; a new one replaces the old.
static void SetTarget(const Target& New)
{
	for (Target& target : g_Targets)
	{
		if (target.tier == New.tier && target.stat == New.stat)
		{
			target = New;
			return;
		}
	}
	g_Targets.push_back(New);
}

static bool DrawItem()
{
	bool changed = false;
	if (!g_HasItem)
	{
		ImGui::TextDisabled("Put an item in the blacksmith's reforge slot.");
		return false;
	}

	ImGui::Text("%s, level %d", g_ItemSlot.c_str(), g_ItemLevel);
	if (ImGui::BeginTable("item", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Tier");
		ImGui::TableSetupColumn("Stat");
		ImGui::TableSetupColumn("Roll");
		ImGui::TableSetupColumn("Value");
		ImGui::TableSetupColumn("Target");
		ImGui::TableHeadersRow();

		for (size_t i = 0; i < g_Item.size(); i++)
		{
			const Affix& affix = g_Item[i];
			bool reforgeable = std::any_of(std::begin(TIER_CODES), std::end(TIER_CODES), [&](const char* code) { return affix.tier == code; });
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextColored(TierColor(affix.tier), "%s", TierName(affix.tier));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(StatName(affix.stat).c_str());
			// Same tags and colours as the stat dropdown.
			bool tagged = false;
			if (affix.pure > 100)
			{
				ImGui::SameLine();
				ImGui::TextColored(PURE_COLOR, "[pure]");
				tagged = true;
			}
			if (affix.locked)
			{
				ImGui::SameLine(0, tagged ? 0.0f : -1.0f);
				ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "[locked]");
			}
			ImGui::TableNextColumn();
			if (reforgeable)
				ImGui::Text("%g / %g", affix.roll, MaxRoll(affix.tier, affix.stat));
			else
				ImGui::Text("%g", affix.roll);
			ImGui::TableNextColumn();
			if (affix.has_shown)
				ImGui::Text("%g", affix.shown);
			ImGui::TableNextColumn();
			if (reforgeable && !affix.locked)
			{
				ImGui::PushID(static_cast<int>(i));
				if (ImGui::SmallButton("Max roll"))
				{
					Target target;
					target.tier = affix.tier;
					target.stat = affix.stat;
					target.goal = Goal::MaxRoll;
					SetTarget(target);
					changed = true;
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	return changed;
}

static bool DrawTargets()
{
	bool changed = false;
	ImGui::SeparatorText("Targets");
	if (g_Targets.empty())
		ImGui::TextDisabled("None. Start will max the Reforge Scores recipe selected in the game.");

	for (size_t i = 0; i < g_Targets.size(); i++)
	{
		ImGui::PushID(static_cast<int>(i));
		if (ImGui::SmallButton("x"))
		{
			g_Targets.erase(g_Targets.begin() + i);
			changed = true;
			ImGui::PopID();
			break;
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(Describe(g_Targets[i]).c_str());
		if (g_HasItem)
		{
			static const ImVec4 colors[] = { { 0.4f, 0.9f, 0.4f, 1.0f }, { 0.9f, 0.8f, 0.3f, 1.0f }, { 1.0f, 0.4f, 0.4f, 1.0f } };
			std::string state_text;
			int state = TargetState(g_Targets[i], state_text);
			ImGui::SameLine();
			ImGui::TextColored(colors[state], "- %s", state_text.c_str());
		}
		ImGui::PopID();
	}

	ImGui::SetNextItemWidth(90);
	if (ImGui::Combo("##tier", &g_NewTier, TIER_NAMES, 5))
		g_NewStat.clear();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(260);
	std::string tier = TIER_CODES[g_NewTier];
	if (ImGui::BeginCombo("##stat", g_NewStat.empty() ? "choose a stat" : StatName(g_NewStat).c_str()))
	{
		for (const StatInfo& stat : g_Stats)
		{
			if (!InPool(stat, tier))
				continue;
			// Stats already on the item are marked; a stat cannot appear twice.
			const Affix* on_item = nullptr;
			for (const Affix& affix : g_Item)
				if (affix.stat == stat.ref) on_item = &affix;
			ImGui::PushID(stat.ref.c_str());
			bool picked = ImGui::Selectable(stat.name.c_str(), stat.ref == g_NewStat);
			if (on_item)
			{
				std::string tier = TierName(on_item->tier);
				std::transform(tier.begin(), tier.end(), tier.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
				ImGui::SameLine();
				ImGui::TextColored({ 0.9f, 0.8f, 0.3f, 1.0f }, "[on item]");
				ImGui::SameLine(0, 0);
				ImGui::TextColored(TierColor(on_item->tier), "[%s]", tier.c_str());
				if (on_item->pure > 100)
				{
					ImGui::SameLine(0, 0);
					ImGui::TextColored(PURE_COLOR, "[pure]");
				}
				if (on_item->locked)
				{
					ImGui::SameLine(0, 0);
					ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "[locked]");
				}
			}
			if (picked)
				g_NewStat = stat.ref;
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::Checkbox("All stats", &g_AllStats);

	ImGui::SetNextItemWidth(130);
	ImGui::Combo("##goal", &g_NewGoal, GOAL_NAMES, 4);
	if (g_NewGoal >= 2)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(100);
		ImGui::InputDouble("##amount", &g_NewAmount, 0, 0, "%g");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(g_NewStat.empty());
	if (ImGui::Button("Add target"))
	{
		Target target;
		target.tier = tier;
		target.stat = g_NewStat;
		target.goal = g_NewGoal == 0 ? Goal::Any : g_NewGoal == 1 ? Goal::MaxRoll : g_NewGoal == 2 ? Goal::Value : Goal::Roll;
		target.amount = g_NewAmount;
		SetTarget(target);
		changed = true;
	}
	ImGui::EndDisabled();
	return changed;
}

static std::string KeyName(int Key)
{
	if (Key >= VK_F1 && Key <= VK_F24)
		return "F" + std::to_string(Key - VK_F1 + 1);
	UINT scan = MapVirtualKeyW(Key, MAPVK_VK_TO_VSC);
	// Navigation keys share scan codes with the numpad unless flagged as extended.
	bool extended = Key == VK_INSERT || Key == VK_DELETE || Key == VK_HOME || Key == VK_END || Key == VK_PRIOR || Key == VK_NEXT
		|| Key == VK_LEFT || Key == VK_RIGHT || Key == VK_UP || Key == VK_DOWN || Key == VK_DIVIDE || Key == VK_RCONTROL || Key == VK_RMENU;
	char name[64] = {};
	if (scan && GetKeyNameTextA(static_cast<LONG>((scan << 16) | (extended ? 1 << 24 : 0)), name, sizeof(name)) > 0)
		return name;
	return "key " + std::to_string(Key);
}

// Returns a key that is down, for rebinding. Mouse buttons are skipped.
static int PressedKey()
{
	for (int key = 8; key <= 254; key++)
	{
		if (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2)
			continue;
		// Skip the side-neutral modifier codes; the left/right ones report the same press.
		if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU)
			continue;
		if (GetAsyncKeyState(key) & 0x8000)
			return key;
	}
	return 0;
}

// About links. An empty address shows the name greyed out until there is a page to point at.
static const char* LINK_AUTHOR = "https://discord.com/users/187106829984202761";
static const char* LINK_NEXUS = "https://www.nexusmods.com/theslormancer/mods/2";
static const char* LINK_GITHUB = "https://github.com/wistb/SlormReforger";

static void Link(const char* Label, const char* Url)
{
	if (Url[0])
	{
		ImGui::TextLinkOpenURL(Label, Url);
		return;
	}
	ImGui::BeginDisabled();
	ImGui::TextLink(Label);
	ImGui::EndDisabled();
}

static const char* THEME_NAMES[] = { "Dark", "Light", "Classic", "Slormancer" };

// Colours every preset derives from one accent.
static void SetAccent(ImVec4 accent)
{
	ImVec4* colors = ImGui::GetStyle().Colors;
	auto with = [&](float alpha) { return ImVec4(accent.x, accent.y, accent.z, alpha); };
	auto dim = [&](float factor) { return ImVec4(accent.x * factor, accent.y * factor, accent.z * factor, 1.0f); };
	colors[ImGuiCol_Button] = with(0.45f);
	colors[ImGuiCol_ButtonHovered] = with(0.75f);
	colors[ImGuiCol_ButtonActive] = with(1.0f);
	colors[ImGuiCol_Header] = with(0.35f);
	colors[ImGuiCol_HeaderHovered] = with(0.65f);
	colors[ImGuiCol_HeaderActive] = with(0.9f);
	colors[ImGuiCol_FrameBgHovered] = with(0.3f);
	colors[ImGuiCol_FrameBgActive] = with(0.5f);
	colors[ImGuiCol_CheckMark] = with(1.0f);
	colors[ImGuiCol_SliderGrab] = with(0.8f);
	colors[ImGuiCol_SliderGrabActive] = with(1.0f);
	colors[ImGuiCol_Tab] = dim(0.4f);
	colors[ImGuiCol_TabHovered] = with(0.8f);
	colors[ImGuiCol_TabSelected] = dim(0.7f);
	colors[ImGuiCol_TitleBgActive] = dim(0.45f);
	colors[ImGuiCol_SeparatorHovered] = with(0.7f);
	colors[ImGuiCol_SeparatorActive] = with(1.0f);
	colors[ImGuiCol_TextSelectedBg] = with(0.4f);
}

// Bars and fields: title bar, unselected tabs, input fields, table headers.
static void SetPrimary(ImVec4 primary)
{
	ImVec4* colors = ImGui::GetStyle().Colors;
	auto shade = [&](float factor, float alpha = 1.0f)
	{
		return ImVec4((std::min)(primary.x * factor, 1.0f), (std::min)(primary.y * factor, 1.0f), (std::min)(primary.z * factor, 1.0f), alpha);
	};
	colors[ImGuiCol_TitleBg] = shade(0.7f);
	colors[ImGuiCol_TitleBgActive] = shade(1.0f);
	colors[ImGuiCol_TitleBgCollapsed] = shade(0.7f, 0.8f);
	colors[ImGuiCol_Tab] = shade(0.7f);
	colors[ImGuiCol_TableHeaderBg] = shade(0.8f);
	colors[ImGuiCol_FrameBg] = shade(0.8f);
	colors[ImGuiCol_FrameBgHovered] = shade(1.15f);
	colors[ImGuiCol_FrameBgActive] = shade(1.4f);
	colors[ImGuiCol_ScrollbarGrab] = shade(1.0f);
	colors[ImGuiCol_ScrollbarGrabHovered] = shade(1.25f);
	colors[ImGuiCol_ScrollbarGrabActive] = shade(1.5f);
}

static void ApplyAppearance()
{
	Appearance& look = g_Appearance;
	look.theme = std::clamp(look.theme, 0, 3);
	look.opacity = std::clamp(look.opacity, 0.3f, 1.0f);
	look.scale = std::clamp(look.scale, 0.75f, 2.0f);

	ImGuiStyle& style = ImGui::GetStyle();
	style = ImGuiStyle();
	style.TabRounding = 0;
	if (look.theme == 1) ImGui::StyleColorsLight();
	else if (look.theme == 2) ImGui::StyleColorsClassic();
	else ImGui::StyleColorsDark();

	ImVec4* colors = style.Colors;
	if (look.theme == 3)
	{
		// The game's panels: near-black brown with dull gold edges.
		style.WindowRounding = 0;
		style.FrameRounding = 0;
		style.WindowBorderSize = 2;
		colors[ImGuiCol_WindowBg] = { 0.07f, 0.06f, 0.05f, 1.0f };
		colors[ImGuiCol_PopupBg] = { 0.09f, 0.08f, 0.07f, 1.0f };
		colors[ImGuiCol_ChildBg] = { 0.05f, 0.045f, 0.04f, 1.0f };
		colors[ImGuiCol_Border] = { 0.36f, 0.32f, 0.24f, 1.0f };
		colors[ImGuiCol_Separator] = { 0.36f, 0.32f, 0.24f, 1.0f };
		colors[ImGuiCol_FrameBg] = { 0.15f, 0.13f, 0.11f, 1.0f };
		colors[ImGuiCol_TitleBg] = { 0.10f, 0.09f, 0.07f, 1.0f };
		colors[ImGuiCol_TitleBgCollapsed] = { 0.10f, 0.09f, 0.07f, 0.8f };
		colors[ImGuiCol_TableHeaderBg] = { 0.14f, 0.12f, 0.10f, 1.0f };
		colors[ImGuiCol_TableRowBgAlt] = { 1.0f, 0.95f, 0.8f, 0.04f };
		colors[ImGuiCol_Text] = { 0.92f, 0.89f, 0.80f, 1.0f };
		colors[ImGuiCol_TextDisabled] = { 0.54f, 0.52f, 0.46f, 1.0f };
		SetAccent({ 0.78f, 0.62f, 0.25f, 1.0f });
	}
	if (look.custom_accent)
		SetAccent({ look.accent[0], look.accent[1], look.accent[2], 1.0f });
	if (look.custom_primary)
		SetPrimary({ look.primary[0], look.primary[1], look.primary[2], 1.0f });

	colors[ImGuiCol_WindowBg].w = look.opacity;
	colors[ImGuiCol_PopupBg].w = (std::max)(look.opacity, 0.9f);
	ImGui::GetIO().FontGlobalScale = look.scale;
}

static bool DrawRunSettings()
{
	bool changed = false;
	ImGui::SeparatorText("Runs");
	ImGui::SetNextItemWidth(110);
	changed |= ImGui::InputInt("Max steps per run", &g_MaxAttempts);

	// One reserve per material the current targets can spend.
	bool needed[MATERIAL_COUNT] = {};
	for (const Recipe& recipe : g_Recipes)
	{
		bool reroll = recipe.type == 0 || recipe.type == 1;
		if (!reroll && !(recipe.type == 3 && g_AutoLock))
			continue;
		bool relevant = g_Targets.empty() && reroll;
		for (const Target& target : g_Targets)
			if (!recipe.tier.empty() && std::tolower(static_cast<unsigned char>(target.tier[0])) == recipe.tier[0]) relevant = true;
		if (!relevant)
			continue;
		for (const auto& [id, count] : ParseCost(recipe.materials))
			needed[id] = true;
	}
	bool any = false;
	for (int id = 0; id < MATERIAL_COUNT; id++)
	{
		if (!needed[id])
			continue;
		if (!any)
			ImGui::TextUnformatted("Keep at least:");
		any = true;
		ImGui::PushID(id);
		ImGui::SetNextItemWidth(110);
		changed |= ImGui::InputInt(MaterialName(id), &g_MinStock[id]);
		ImGui::SameLine();
		ImGui::TextDisabled("have %.0f", g_Stock[id]);
		ImGui::PopID();
		g_MinStock[id] = (std::max)(g_MinStock[id], 0);
	}
	changed |= ImGui::Checkbox("Allow rerolling pure stats", &g_AllowPureLoss);
	changed |= ImGui::Checkbox("Lock stats as they reach their target", &g_AutoLock);
	g_MaxAttempts = (std::max)(g_MaxAttempts, 1);
	return changed;
}

static bool DrawOptions()
{
	bool changed = false;
	ImGui::SeparatorText("Appearance");
	bool restyle = false;
	ImGui::SetNextItemWidth(160);
	restyle |= ImGui::Combo("Theme", &g_Appearance.theme, THEME_NAMES, 4);
	restyle |= ImGui::Checkbox("Custom accent", &g_Appearance.custom_accent);
	if (g_Appearance.custom_accent)
	{
		ImGui::SameLine();
		restyle |= ImGui::ColorEdit3("##accent", g_Appearance.accent, ImGuiColorEditFlags_NoInputs);
	}
	restyle |= ImGui::Checkbox("Custom primary", &g_Appearance.custom_primary);
	if (g_Appearance.custom_primary)
	{
		ImGui::SameLine();
		restyle |= ImGui::ColorEdit3("##primary", g_Appearance.primary, ImGuiColorEditFlags_NoInputs);
	}
	ImGui::SetNextItemWidth(160);
	restyle |= ImGui::SliderFloat("Opacity", &g_Appearance.opacity, 0.3f, 1.0f, "%.2f");
	ImGui::SetNextItemWidth(160);
	// Applied on release: resizing the text under the cursor makes the slider jump.
	static float scale = 0;
	if (!ImGui::IsAnyItemActive() || scale == 0)
		scale = g_Appearance.scale;
	ImGui::SliderFloat("Text size", &scale, 0.75f, 2.0f, "%.2f");
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		g_Appearance.scale = scale;
		restyle = true;
	}
	changed |= ImGui::Checkbox("Show recipes and stock", &g_Appearance.show_costs);
	if (ImGui::Button("Reset appearance"))
	{
		int hotkey = g_Appearance.hotkey;
		g_Appearance = Appearance();
		g_Appearance.hotkey = hotkey;
		scale = 0;
		restyle = true;
	}
	if (restyle)
		ApplyAppearance();

	ImGui::SeparatorText("Hotkey");
	static bool capturing = false;
	// A key held when capture starts must be released first, or it would be taken at once.
	static bool armed = false;
	if (capturing)
	{
		g_SuppressToggle = true;
		ImGui::Button("Press a key (Esc cancels)", { 220, 0 });
		int key = PressedKey();
		if (!armed)
			armed = key == 0;
		else if (key == VK_ESCAPE)
			capturing = false;
		else if (key)
		{
			g_Appearance.hotkey = key;
			capturing = false;
			changed = true;
		}
	}
	else
	{
		if (ImGui::Button(KeyName(g_Appearance.hotkey).c_str(), { 220, 0 }))
		{
			capturing = true;
			armed = false;
		}
		ImGui::SameLine();
		ImGui::TextUnformatted("shows or hides the overlay");
	}

	ImGui::SeparatorText("About");
	ImGui::TextUnformatted("SlormReforger v" SLORMREFORGER_VERSION " by");
	ImGui::SameLine();
	Link("Crash", LINK_AUTHOR);
	Link("Nexus Mods", LINK_NEXUS);
	ImGui::SameLine();
	Link("GitHub", LINK_GITHUB);
	return changed || restyle;
}

static void DrawCosts()
{
	ImGui::SeparatorText("Recipes and stock");
	bool used[MATERIAL_COUNT] = {};
	if (ImGui::BeginTable("recipes", 2, ImGuiTableFlags_SizingFixedFit))
	{
		for (const Recipe& recipe : g_Recipes)
		{
			// Reforge, lock and unlock; the mod does not apply the others.
			if (recipe.type != 0 && recipe.type != 1 && recipe.type != 3)
				continue;
			std::string cost;
			for (const auto& [id, count] : ParseCost(recipe.materials))
			{
				used[id] = true;
				cost += std::string(cost.empty() ? "" : " + ") + (count > 1 ? std::to_string(count) + " x " : "") + MaterialName(id);
			}
			std::string name = recipe.type == 3 ? recipe.detail : recipe.label;
			name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return c == '{' || c == '}'; }), name.end());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(cost.c_str());
		}
		ImGui::EndTable();
	}

	for (int id = 0; id < MATERIAL_COUNT; id++)
		if (used[id]) ImGui::Text("%s: %.0f", MaterialName(id), g_Stock[id]);
	ImGui::Text("Goldus: %.0f", g_Gold);
}

static void DrawRun()
{
	ImGui::Separator();
	if (g_Running)
	{
		if (ImGui::Button("Stop", { 120, 0 }))
			g_WantStop = true;
		ImGui::SameLine();
		ImGui::Text("running, %d / %d steps", g_Attempts, g_MaxAttempts);
	}
	else
	{
		ImGui::BeginDisabled(!g_HasItem);
		if (ImGui::Button("Start", { 120, 0 }))
			g_WantStart = true;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextWrapped("%s", g_Status.c_str());
	}

	// Long lines wrap; the box never scrolls sideways.
	if (!g_Notes.empty() && ImGui::BeginChild("notes", { 0, 110 * g_Appearance.scale }, ImGuiChildFlags_Borders))
	{
		for (const std::string& note : g_Notes)
			ImGui::TextWrapped("%s", note.c_str());
		if (g_Running)
			ImGui::SetScrollHereY(1.0f);
	}
	if (!g_Notes.empty())
		ImGui::EndChild();
}

static void DrawWindow()
{
	std::lock_guard guard(g_Lock);
	ImGui::SetNextWindowPos({ 40, 40 }, ImGuiCond_FirstUseEver);
	// The id after ### keeps the saved position when the title changes.
	if (!ImGui::Begin("SlormReforger v" SLORMREFORGER_VERSION "###SlormReforger", &g_Visible, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::End();
		return;
	}

	bool changed = false;
	if (ImGui::BeginTabBar("tabs"))
	{
		if (ImGui::BeginTabItem("Reforge"))
		{
			ImGui::BeginDisabled(g_Running);
			changed |= DrawItem();
			changed |= DrawTargets();
			changed |= DrawRunSettings();
			ImGui::EndDisabled();
			if (g_HasItem && g_Appearance.show_costs)
				DrawCosts();
			DrawRun();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Options"))
		{
			changed |= DrawOptions();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
	if (changed)
		SaveConfig();
}

static void Frame(IDXGISwapChain* swapchain, UINT flags)
{
	if (!swapchain || (flags & DXGI_PRESENT_TEST))
		return;

	if (!g_Ready)
	{
		DXGI_SWAP_CHAIN_DESC description = {};
		if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_Device))) || FAILED(swapchain->GetDesc(&description)))
			return;
		g_Device->GetImmediateContext(&g_Context);

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = g_IniPath.empty() ? nullptr : g_IniPath.c_str();
		ApplyAppearance();
		g_Window = description.OutputWindow;
		ImGui_ImplWin32_Init(g_Window);
		g_OriginalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_Window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
		ImGui_ImplDX11_Init(g_Device, g_Context);
		g_Ready = true;
		Aurie::DbgPrintEx(Aurie::LOG_SEVERITY_INFO, "[SlormReforger] overlay ready");
	}

	if (!g_Visible || !g_HasItem)
	{
		g_HideMouse = false;
		return;
	}

	ID3D11Texture2D* buffer = nullptr;
	ID3D11RenderTargetView* view = nullptr;
	if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
		return;
	D3D11_TEXTURE2D_DESC size = {};
	buffer->GetDesc(&size);
	HRESULT created = g_Device->CreateRenderTargetView(buffer, nullptr, &view);
	buffer->Release();
	if (FAILED(created))
		return;

	g_HideMouse = false;
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	// The mouse is polled: window messages do not reliably carry it in this game.
	// The back buffer can be a different size from the window, so scale to it.
	ImGuiIO& io = ImGui::GetIO();
	RECT client = {};
	POINT cursor = {};
	GetClientRect(g_Window, &client);
	RealCursor(&cursor);
	ScreenToClient(g_Window, &cursor);
	io.DisplaySize = { static_cast<float>(size.Width), static_cast<float>(size.Height) };
	static bool logged = false;
	if (!logged)
	{
		logged = true;
		Aurie::DbgPrintEx(Aurie::LOG_SEVERITY_INFO, "[SlormReforger] back buffer %ux%u, window %ldx%ld", size.Width, size.Height, client.right, client.bottom);
	}
	if (client.right > 0 && client.bottom > 0 && GetForegroundWindow() == g_Window)
	{
		io.AddMousePosEvent(cursor.x * static_cast<float>(size.Width) / client.right, cursor.y * static_cast<float>(size.Height) / client.bottom);
		io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
		io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
	}
	ImGui::NewFrame();
	// The game draws its own cursor under the overlay.
	ImGui::GetIO().MouseDrawCursor = ImGui::GetIO().WantCaptureMouse;
	DrawWindow();
	ImGui::Render();
	g_HideMouse = io.WantCaptureMouse;

	ID3D11RenderTargetView* old_view = nullptr;
	ID3D11DepthStencilView* old_depth = nullptr;
	g_Context->OMGetRenderTargets(1, &old_view, &old_depth);
	g_Context->OMSetRenderTargets(1, &view, nullptr);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	g_Context->OMSetRenderTargets(1, &old_view, old_depth);
	if (old_view) old_view->Release();
	if (old_depth) old_depth->Release();
	view->Release();
}

static void LogOnce(bool& Done, const char* Text)
{
	if (Done)
		return;
	Done = true;
	Aurie::DbgPrintEx(Aurie::LOG_SEVERITY_INFO, "[SlormReforger] %s", Text);
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	static bool saw_any = false, saw_mouse = false, saw_raw = false, saw_char = false;
	bool mouse = Message >= WM_MOUSEFIRST && Message <= WM_MOUSELAST;
	bool keyboard = Message == WM_KEYDOWN || Message == WM_KEYUP || Message == WM_CHAR;
	LogOnce(saw_any, "window messages seen");
	if (mouse) LogOnce(saw_mouse, "mouse messages seen");
	if (Message == WM_INPUT) LogOnce(saw_raw, "raw input seen");
	if (Message == WM_CHAR) LogOnce(saw_char, "char messages seen");

	if (g_Ready && g_Visible && g_HasItem)
	{
		// Mouse position and buttons are polled per frame; wheel and keys come from here.
		if (!mouse || Message == WM_MOUSEWHEEL)
			ImGui_ImplWin32_WndProcHandler(Window, Message, WParam, LParam);

		ImGuiIO& io = ImGui::GetIO();
		// Without WM_CHAR, type digits from key presses.
		if (Message == WM_KEYDOWN && io.WantTextInput && !saw_char)
		{
			if (WParam >= '0' && WParam <= '9') io.AddInputCharacter(static_cast<unsigned>(WParam));
			else if (WParam >= VK_NUMPAD0 && WParam <= VK_NUMPAD9) io.AddInputCharacter(static_cast<unsigned>('0' + WParam - VK_NUMPAD0));
			else if (WParam == VK_OEM_PERIOD || WParam == VK_DECIMAL) io.AddInputCharacter('.');
			else if (WParam == VK_OEM_MINUS || WParam == VK_SUBTRACT) io.AddInputCharacter('-');
		}

		// Keep clicks and typing on the overlay away from the game.
		if ((mouse && io.WantCaptureMouse) || (keyboard && io.WantTextInput))
			return 0;
	}
	return CallWindowProcW(g_OriginalProc, Window, Message, WParam, LParam);
}

bool OverlayWantsMouse()
{
	return g_Ready && g_Visible && g_HasItem && ImGui::GetIO().WantCaptureMouse;
}

bool OverlayWantsKeys()
{
	return g_Ready && g_Visible && g_HasItem && ImGui::GetIO().WantTextInput;
}

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_OriginalPresent = nullptr;

static HRESULT WINAPI HookedPresent(IDXGISwapChain* Swapchain, UINT Sync, UINT Flags)
{
	Frame(Swapchain, Flags);
	return g_OriginalPresent(Swapchain, Sync, Flags);
}

// YYToolkit 5.0.0c never installs its own Present hook, so EVENT_FRAME does not fire.
bool OverlayInstall(YYTKInterface* Yytk)
{
	RValue info = Yytk->CallBuiltin("os_get_info", {});
	RValue chain = Yytk->CallBuiltin("ds_map_find_value", { info, RValue("video_d3d11_swapchain") });
	Yytk->CallBuiltin("ds_map_destroy", { info });

	IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(chain.m_Pointer);
	if (!swapchain)
		return false;
	PVOID* table = *reinterpret_cast<PVOID**>(swapchain);
	Aurie::AurieStatus status = Aurie::MmCreateHook(Aurie::g_ArSelfModule, "SlormReforger Present", table[8], HookedPresent, reinterpret_cast<PVOID*>(&g_OriginalPresent));
	if (!Aurie::AurieSuccess(status) || !g_OriginalPresent)
		return false;

	status = Aurie::MmCreateHook(Aurie::g_ArSelfModule, "SlormReforger GetCursorPos", GetCursorPos, HookedCursor, reinterpret_cast<PVOID*>(&g_OriginalCursor));
	if (!Aurie::AurieSuccess(status))
		Aurie::DbgPrintEx(Aurie::LOG_SEVERITY_WARNING, "[SlormReforger] cursor hook failed: %s", Aurie::AurieStatusToString(status));
	return true;
}
