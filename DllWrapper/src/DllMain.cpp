#include "UniversalProxyDLL.h"

#include "Logger.h"

#include "ModConfiguration.h"

#include "ModManager.h"
#include "AreaMusicManager.h"
#include "InputTracker.h"
#include "MusicPlayer.h"
#include "UIManager.h"
#include "GameStateManager.h"
#include "LanguageManager.h"

#include "MinHook.h"

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

const wchar_t* GetModuleFilename(HMODULE hModule)
{
	static wchar_t path[MAX_PATH];
	GetModuleFileNameW(hModule, path, MAX_PATH);
	wchar_t* filename = wcsrchr(path, L'\\');
	return filename ? filename + 1 : path;
}

bool IsProxyModule(HMODULE hModule)
{
	const wchar_t* filename = GetModuleFilename(hModule);
	return _wcsicmp(filename, L"dxgi.dll") == 0 || _wcsicmp(filename, L"winhttp.dll") == 0;
}

bool StartsFromDllMain(HMODULE hModule)
{
	const wchar_t* filename = GetModuleFilename(hModule);
	return _wcsicmp(filename, L"dxgi.dll") != 0;
}

bool IsValidProcess()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(NULL, path, MAX_PATH);
	wchar_t* filename = wcsrchr(path, L'\\');
	if (!filename) return false;

	static const wchar_t* validProcesses[] = {
		L"ds.exe",
		L"DeathStranding.exe",
	};
	for (auto& proc : validProcesses)
	{
		if (_wcsicmp(filename + 1, proc) == 0) return true;
	}
	return false;
}

static HMODULE gProxyModule = nullptr;

DWORD WINAPI HookThread(LPVOID lpParam)
{
	Logging::Initialize(ModConfiguration::modLogFilename.c_str());

	static ModManager modManager;
	ModManager::SetInstance(&modManager);

	static InputTracker inputTracker;
	static MusicPlayer musicPlayer;
	static AreaMusicManager areaMusicManager;
	static UIManager uiManager;
	static GameStateManager gameStateManager;
	static LanguageManager languageManager;

	modManager.RegisterListener(&inputTracker);
	modManager.RegisterListener(&musicPlayer);
	modManager.RegisterListener(&areaMusicManager);
	modManager.RegisterListener(&uiManager);
	modManager.RegisterListener(&gameStateManager);
	modManager.RegisterListener(&languageManager);

	modManager.Initialize();
	return 0;
}

static HMODULE gRealDxgi = nullptr;
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
static PFN_CreateDXGIFactory1 real_CreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 real_CreateDXGIFactory2 = nullptr;
static volatile LONG gHookThreadStarted = 0;
static HANDLE gHookThreadMutex = nullptr;

static HMODULE LoadRealDxgi()
{
	if (!gRealDxgi)
	{
		wchar_t path[MAX_PATH];
		GetSystemDirectoryW(path, MAX_PATH);
		wcscat_s(path, L"\\dxgi.dll");

		gRealDxgi = LoadLibraryW(path);
		if (!gRealDxgi)
		{
			MessageBoxW(nullptr, L"Failed to load original dxgi.dll", L"dxgi proxy", MB_OK | MB_ICONERROR);
		}
	}

	return gRealDxgi;
}

static void StartHookThreadOnce()
{
	if (!IsValidProcess())
	{
		return;
	}

	if (InterlockedCompareExchange(&gHookThreadStarted, 1, 0) == 0)
	{
		wchar_t mutexName[128] = {};
		swprintf_s(
			mutexName,
			L"Local\\WalkingManHookThreadStarted_%lu",
			GetCurrentProcessId()
		);
		gHookThreadMutex = CreateMutexW(nullptr, TRUE, mutexName);
		if (!gHookThreadMutex || GetLastError() == ERROR_ALREADY_EXISTS)
		{
			if (gHookThreadMutex)
			{
				CloseHandle(gHookThreadMutex);
				gHookThreadMutex = nullptr;
			}
			return;
		}

		CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
	}
}

DWORD WINAPI DeferredHookThread(LPVOID lpParam)
{
	HMODULE module = reinterpret_cast<HMODULE>(lpParam);

	const ULONGLONG deadline = GetTickCount64() + 60000;
	while (GetTickCount64() < deadline)
	{
		if (GetModuleHandleW(L"dxgi.dll") || GetModuleHandleW(L"d3d12.dll"))
		{
			break;
		}
		Sleep(100);
	}

	Sleep(3000);
	StartHookThreadOnce();
	return 0;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory1Hook(REFIID riid, void** ppFactory)
{
	if (!LoadRealDxgi())
	{
		return E_FAIL;
	}

	if (!real_CreateDXGIFactory1)
	{
		real_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(gRealDxgi, "CreateDXGIFactory1");
		if (!real_CreateDXGIFactory1)
		{
			MessageBoxW(nullptr, L"Failed to find CreateDXGIFactory1", L"dxgi proxy", MB_OK | MB_ICONERROR);
			return E_FAIL;
		}
	}

	HRESULT hr = real_CreateDXGIFactory1(riid, ppFactory);

	if (SUCCEEDED(hr))
	{
		StartHookThreadOnce();
	}

	return hr;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI CreateDXGIFactory2Hook(UINT flags, REFIID riid, void** ppFactory)
{
	if (!LoadRealDxgi())
	{
		return E_FAIL;
	}

	if (!real_CreateDXGIFactory2)
	{
		real_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(gRealDxgi, "CreateDXGIFactory2");
		if (!real_CreateDXGIFactory2)
		{
			MessageBoxW(nullptr, L"Failed to find CreateDXGIFactory2", L"dxgi proxy", MB_OK | MB_ICONERROR);
			return E_FAIL;
		}
	}

	HRESULT hr = real_CreateDXGIFactory2(flags, riid, ppFactory);

	if (SUCCEEDED(hr))
	{
		StartHookThreadOnce();
	}

	return hr;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		gProxyModule = module;

		const bool proxyModule = IsProxyModule(module);
		if (proxyModule)
		{
			UPD::MuteLogging();
			UPD::CreateProxy(module);
		}

		if (!IsValidProcess())
		{
			return TRUE;
		}

		OpenDevTerminal();
		MH_Initialize();

		if (!proxyModule)
		{
			StartHookThreadOnce();
		}
		else if (StartsFromDllMain(module))
		{
			CreateThread(nullptr, 0, DeferredHookThread, module, 0, nullptr);
		}
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		MH_RemoveHook(MH_ALL_HOOKS);
		MH_Uninitialize();
		if (gHookThreadMutex)
		{
			CloseHandle(gHookThreadMutex);
			gHookThreadMutex = nullptr;
		}
	}

	return 1;
}
