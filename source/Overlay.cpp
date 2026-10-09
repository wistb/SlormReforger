#include "Reforger.hpp"
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
			if (affix.pure > 100) { ImGui::SameLine(); ImGui::TextColored(PURE_COLOR, "pure"); }
			if (affix.locked) { ImGui::SameLine(); ImGui::TextDisabled("locked"); }
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

static bool DrawSettings()
{
	bool changed = false;
	ImGui::SeparatorText("Limits");
	ImGui::SetNextItemWidth(110);
	changed |= ImGui::InputInt("Max reforges per run", &g_MaxAttempts);
	ImGui::SetNextItemWidth(110);
	changed |= ImGui::InputInt("Keep at least this many of each material", &g_MinStock);
	changed |= ImGui::Checkbox("Allow rerolling pure stats", &g_AllowPureLoss);
	g_MaxAttempts = (std::max)(g_MaxAttempts, 1);
	g_MinStock = (std::max)(g_MinStock, 0);
	return changed;
}

static void DrawCosts()
{
	ImGui::SeparatorText("Recipes and stock");
	bool used[MATERIAL_COUNT] = {};
	if (ImGui::BeginTable("recipes", 3, ImGuiTableFlags_SizingFixedFit))
	{
		for (const Recipe& recipe : g_Recipes)
		{
			if (recipe.type != 0 && recipe.type != 1)
				continue;
			std::string cost;
			size_t start = 0;
			while (start <= recipe.materials.size())
			{
				size_t end = recipe.materials.find('|', start);
				std::string id_text = recipe.materials.substr(start, end == std::string::npos ? std::string::npos : end - start);
				int id = atoi(id_text.c_str());
				if (!id_text.empty() && id_text.find_first_not_of("0123456789") == std::string::npos && id < MATERIAL_COUNT)
				{
					used[id] = true;
					cost += std::string(cost.empty() ? "" : " + ") + MaterialName(id);
				}
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(recipe.label.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(cost.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%g goldus", recipe.gold);
		}
		ImGui::EndTable();
	}

	for (int id = 0; id < MATERIAL_COUNT; id++)
		if (used[id]) ImGui::Text("%s: %.0f", MaterialName(id), g_Stock[id]);
	ImGui::Text("Goldus: %.0f", g_Gold);
}

static void DrawWindow()
{
	std::lock_guard guard(g_Lock);
	ImGui::SetNextWindowPos({ 40, 40 }, ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("SlormReforger (F6 hides)", &g_Visible, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::End();
		return;
	}

	bool changed = false;
	ImGui::BeginDisabled(g_Running);
	changed |= DrawItem();
	changed |= DrawTargets();
	changed |= DrawSettings();
	ImGui::EndDisabled();
	if (g_HasItem)
		DrawCosts();

	ImGui::Separator();
	if (g_Running)
	{
		if (ImGui::Button("Stop", { 120, 0 }))
			g_WantStop = true;
		ImGui::SameLine();
		ImGui::Text("running, %d / %d reforges", g_Attempts, g_MaxAttempts);
	}
	else
	{
		ImGui::BeginDisabled(!g_HasItem);
		if (ImGui::Button("Start", { 120, 0 }))
			g_WantStart = true;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextUnformatted(g_Status.c_str());
	}

	if (!g_Notes.empty() && ImGui::BeginChild("notes", { 0, 110 }, ImGuiChildFlags_Borders))
	{
		for (const std::string& note : g_Notes)
			ImGui::TextUnformatted(note.c_str());
		if (g_Running)
			ImGui::SetScrollHereY(1.0f);
	}
	if (!g_Notes.empty())
		ImGui::EndChild();

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
		ImGui::StyleColorsDark();
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
