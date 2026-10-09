// version.dll proxy: Windows loads it from the game folder in place of the system one.
// It forwards the real version.dll and starts Aurie at the game's entry point, which is
// what Aurie's exe patcher does, without changing any file of the game.
#include <Windows.h>
#include <cstdint>
#include <cstring>

static const char* const EXPORTS[] = {
	"GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
	"GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
	"GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
	"VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW",
};
constexpr int EXPORT_COUNT = sizeof(EXPORTS) / sizeof(EXPORTS[0]);

// No export takes more than eight arguments, all integers or pointers, so one shape forwards them all.
using Forwarded = uintptr_t(*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static Forwarded g_Real[EXPORT_COUNT] = {};

#define FORWARD(Index, Name) \
	extern "C" uintptr_t Proxy_##Name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) \
	{ return g_Real[Index] ? g_Real[Index](a, b, c, d, e, f, g, h) : 0; }

FORWARD(0, GetFileVersionInfoA)
FORWARD(1, GetFileVersionInfoByHandle)
FORWARD(2, GetFileVersionInfoExA)
FORWARD(3, GetFileVersionInfoExW)
FORWARD(4, GetFileVersionInfoSizeA)
FORWARD(5, GetFileVersionInfoSizeExA)
FORWARD(6, GetFileVersionInfoSizeExW)
FORWARD(7, GetFileVersionInfoSizeW)
FORWARD(8, GetFileVersionInfoW)
FORWARD(9, VerFindFileA)
FORWARD(10, VerFindFileW)
FORWARD(11, VerInstallFileA)
FORWARD(12, VerInstallFileW)
FORWARD(13, VerLanguageNameA)
FORWARD(14, VerLanguageNameW)
FORWARD(15, VerQueryValueA)
FORWARD(16, VerQueryValueW)

// mov rax, address; jmp rax
constexpr size_t PATCH_SIZE = 12;
static uint8_t* g_Entry = nullptr;
static uint8_t g_EntryBytes[PATCH_SIZE] = {};
static wchar_t g_AuriePath[MAX_PATH] = {};

static void WriteEntry(const uint8_t* Bytes)
{
	DWORD old = 0;
	if (!VirtualProtect(g_Entry, PATCH_SIZE, PAGE_EXECUTE_READWRITE, &old))
		return;
	memcpy(g_Entry, Bytes, PATCH_SIZE);
	VirtualProtect(g_Entry, PATCH_SIZE, old, &old);
	FlushInstructionCache(GetCurrentProcess(), g_Entry, PATCH_SIZE);
}

// If Aurie fails before resuming the game, let the game start without mods instead of hanging.
static DWORD WINAPI Watchdog(LPVOID Thread)
{
	Sleep(30000);
	ResumeThread(static_cast<HANDLE>(Thread));
	CloseHandle(static_cast<HANDLE>(Thread));
	return 0;
}

// Runs in place of the game's entry point, after the loader has finished.
static DWORD WINAPI EntryStub(LPVOID Parameter)
{
	WriteEntry(g_EntryBytes);

	// Aurie starts on its own thread, loads the mods that must hook early,
	// then resumes this thread; it expects to find it suspended.
	if (LoadLibraryW(g_AuriePath))
	{
		HANDLE self = nullptr;
		if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS))
		{
			HANDLE watchdog = CreateThread(nullptr, 0, Watchdog, self, 0, nullptr);
			if (watchdog)
				CloseHandle(watchdog);
			else
				CloseHandle(self);
		}
		SuspendThread(GetCurrentThread());
	}
	return reinterpret_cast<LPTHREAD_START_ROUTINE>(g_Entry)(Parameter);
}

static bool HasSection(HMODULE Image, const char* Name)
{
	auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(Image);
	auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uint8_t*>(Image) + dos->e_lfanew);
	PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
		if (!strncmp(reinterpret_cast<const char*>(section->Name), Name, IMAGE_SIZEOF_SHORT_NAME)) return true;
	return false;
}

static void LoadRealVersion()
{
	wchar_t path[MAX_PATH] = {};
	UINT length = GetSystemDirectoryW(path, MAX_PATH);
	if (!length || length > MAX_PATH - 16)
		return;
	wcscat_s(path, L"\\version.dll");
	HMODULE real = LoadLibraryW(path);
	if (!real)
		return;
	for (int i = 0; i < EXPORT_COUNT; i++)
		g_Real[i] = reinterpret_cast<Forwarded>(GetProcAddress(real, EXPORTS[i]));
}

static void HookEntry()
{
	HMODULE exe = GetModuleHandleW(nullptr);
	// An exe already patched by Aurie loads the framework itself.
	if (HasSection(exe, ".aurie"))
		return;

	DWORD length = GetModuleFileNameW(exe, g_AuriePath, MAX_PATH);
	wchar_t* slash = length && length < MAX_PATH ? wcsrchr(g_AuriePath, L'\\') : nullptr;
	if (!slash)
		return;
	*slash = 0;
	if (wcscat_s(g_AuriePath, L"\\mods\\Native\\AurieCore.dll") != 0)
		return;
	if (GetFileAttributesW(g_AuriePath) == INVALID_FILE_ATTRIBUTES)
		return;

	auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(exe);
	auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
	if (!nt->OptionalHeader.AddressOfEntryPoint)
		return;
	g_Entry = reinterpret_cast<uint8_t*>(exe) + nt->OptionalHeader.AddressOfEntryPoint;
	memcpy(g_EntryBytes, g_Entry, PATCH_SIZE);

	uint8_t jump[PATCH_SIZE] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
	uintptr_t target = reinterpret_cast<uintptr_t>(&EntryStub);
	memcpy(jump + 2, &target, sizeof(target));
	WriteEntry(jump);
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID)
{
	if (Reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(Instance);
		LoadRealVersion();
		HookEntry();
	}
	return TRUE;
}
