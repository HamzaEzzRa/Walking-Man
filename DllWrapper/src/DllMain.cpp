#include "UniversalProxyDLL.h"

#include "Logger.h"

#include "ModConfiguration.h"

#include "ModManager.h"
#include "InputTracker.h"
#include "MusicPlayer.h"
#include "UIManager.h"
#include "GameStateManager.h"
#include "LanguageManager.h"

#include "MinHook.h"

static Logger logger{ "DllMain" };

void OpenDevTerminal()
{
	std::fstream terminalEnableFile;
	terminalEnableFile.open(ModConfiguration::enableDevFilename, std::fstream::in);
	if (terminalEnableFile.is_open())
	{
		ModConfiguration::devMode = true;
		if (AllocConsole())
		{
			freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
			SetWindowText(GetConsoleWindow(), ModConfiguration::modPublicName.c_str());
		}
		terminalEnableFile.close();
	}
}

bool IsStandalone(HMODULE hModule)
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(hModule, path, MAX_PATH);
	wchar_t* filename = wcsrchr(path, L'\\');
	return filename && _wcsicmp(filename + 1, L"dxgi.dll") == 0;
}

DWORD WINAPI HookThread(LPVOID lpParam)
{
	static ModManager modManager;
	ModManager::SetInstance(&modManager);

	static InputTracker inputTracker;
	static MusicPlayer musicPlayer;
	static UIManager uiManager;
	static GameStateManager gameStateManager;
	static LanguageManager languageManager;

	modManager.RegisterListener(&inputTracker);
	modManager.RegisterListener(&musicPlayer);
	modManager.RegisterListener(&uiManager);
	modManager.RegisterListener(&gameStateManager);
	modManager.RegisterListener(&languageManager);

	modManager.Initialize();
	return 0;
}

static HMODULE gRealDxgi = nullptr;
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
static PFN_CreateDXGIFactory1 real_CreateDXGIFactory1 = nullptr;

extern "C" __declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory1Hook(REFIID riid, void** ppFactory)
{
	if (!gRealDxgi) {
		wchar_t path[MAX_PATH];
		GetSystemDirectoryW(path, MAX_PATH);
		wcscat_s(path, L"\\dxgi.dll");

		gRealDxgi = LoadLibraryW(path);
		if (!gRealDxgi) {
			MessageBoxW(nullptr, L"Failed to load original dxgi.dll", L"dxgi proxy", MB_OK | MB_ICONERROR);
			return E_FAIL;
		}
	}

	if (!real_CreateDXGIFactory1) {
		real_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(gRealDxgi, "CreateDXGIFactory1");
		if (!real_CreateDXGIFactory1) {
			MessageBoxW(nullptr, L"Failed to find CreateDXGIFactory1", L"dxgi proxy", MB_OK | MB_ICONERROR);
			return E_FAIL;
		}
	}

	HRESULT hr = real_CreateDXGIFactory1(riid, ppFactory);

	if (SUCCEEDED(hr))
	{
		static bool initialized = false;
		if (!initialized)
		{
			initialized = true;
			CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
		}
	}

	return hr;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		OpenDevTerminal();

		if (IsStandalone(module))
		{
			UPD::MuteLogging();
			UPD::CreateProxy(module);
		}
		else
		{
			CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
		}

		MH_Initialize();
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		MH_RemoveHook(MH_ALL_HOOKS);
		MH_Uninitialize();
	}

	return 1;
}