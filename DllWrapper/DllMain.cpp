#include <Windows.h>
#include "UniversalProxyDLL.h"

#include "DirectXHook.h"
#include "Logger.h"
#include "MemoryUtils.h"

#include "ModConfiguration.h"

#include "ModManager.h"
#include "InputTracker.h"
#include "MusicPlayer.h"
#include "UIManager.h"
#include "GameStateManager.h"

#include "MinHook.h"

static Logger logger{ "DllMain" };

HANDLE gHookThread = NULL;

void OpenDebugTerminal()
{
	std::fstream terminalEnableFile;
	terminalEnableFile.open(ModConfiguration::enableTerminalFilename, std::fstream::in);
	if (terminalEnableFile.is_open())
	{
		if (AllocConsole())
		{
			freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
			SetWindowText(GetConsoleWindow(), ModConfiguration::modPublicName.c_str());
		}
		terminalEnableFile.close();
	}
}

DWORD WINAPI HookThread(LPVOID lpParam)
{
	static Renderer renderer;
	static DirectXHook dxHook(&renderer);

	static ModManager modManager;
	ModManager::SetInstance(&modManager);
	dxHook.AddRenderCallback(&modManager);

	static InputTracker inputTracker;
	static MusicPlayer musicPlayer;
	static UIManager uiManager;
	static GameStateManager gameStateManager;

	modManager.RegisterListener(&inputTracker);
	modManager.RegisterListener(&musicPlayer);
	modManager.RegisterListener(&uiManager);
	modManager.RegisterListener(&gameStateManager);

	dxHook.Hook();
	return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		OpenDebugTerminal();
		UPD::MuteLogging();
		UPD::CreateProxy(module);
		MH_Initialize();
		gHookThread = CreateThread(0, 0, &HookThread, 0, 0, NULL);
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		MH_RemoveHook(MH_ALL_HOOKS);
		MH_Uninitialize();
	}

	return 1;
}