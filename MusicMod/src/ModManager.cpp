#include "ModManager.h"

#include <cstring>

#include "MemoryUtils.h"
#include "MinHook.h"
#include "PatternScanner.h"

#include "Utils.h"

#include "CustomMediaLoader.h"
#include "ModConfiguration.h"

#include "GameData.h"

ModManager::ModManager()
{
}

void ModManager::SetInstance(ModManager* instance)
{
	ModManager::instance = instance;
}

ModManager* ModManager::GetInstance()
{
	return ModManager::instance;
}

bool ModManager::TryHookFunction(const std::string& name, void* hookFunction)
{
	auto it = ModConfiguration::Databases::functionDatabase.find(name);
	if (it == ModConfiguration::Databases::functionDatabase.end())
	{
		return false;
	}

	auto& funcData = it->second;
	if (!funcData.address)
	{
		return false;
	}

	if (funcData.usesAVX)
	{
		MemoryUtils::PlaceHook(
			funcData.address,
			reinterpret_cast<uintptr_t>(hookFunction),
			reinterpret_cast<uintptr_t*>(&funcData.originalFunction)
		);
	}
	else
	{
		auto created = MH_CreateHook(
			reinterpret_cast<LPVOID>(funcData.address),
			hookFunction, reinterpret_cast<LPVOID*>(&funcData.originalFunction)
		);
		if (created != MH_OK)
		{
			Logging::Write(logPrefix,
				"Failed to create hook for function \"%s\" at address %p",
				name.c_str(), funcData.address
			);
			return false;
		}

		auto enabled = MH_EnableHook(reinterpret_cast<LPVOID>(funcData.address));
		if (enabled != MH_OK)
		{
			Logging::Write(logPrefix,
				"Failed to enable hook for function \"%s\" at address %p",
				name.c_str(), funcData.address
			);
			return false;
		}
	}

	return true;
}

bool ModManager::TryUnhookFunction(const FunctionData& func)
{
	if (!func.address)
	{
		return false;
	}

	if (func.usesAVX)
	{
		MemoryUtils::Unhook(func.address);
	}
	else
	{
		auto disabled = MH_DisableHook(reinterpret_cast<LPVOID>(func.address));
		if (disabled != MH_OK)
		{
			Logging::Write(logPrefix,
				"Failed to disable hook for function \"%s\" at address %p",
				func.name, func.address
			);
			return false;
		}
	}

	return true;
}

const FunctionData* ModManager::GetFunctionData(const std::string& name)
{
	auto it = ModConfiguration::Databases::functionDatabase.find(name);
	if (it == ModConfiguration::Databases::functionDatabase.end())
	{
		return nullptr;
	}
	return &(it->second);
}

void ModManager::Initialize()
{
	Logging::Initialize(ModConfiguration::modLogFilename.c_str());
	Logging::Write(logPrefix, "Initializing...");

	bool configLoaded = ModConfiguration::LoadConfigFromFile();
	Logging::Write(logPrefix,
		configLoaded ? "Ini configuration loaded successfully"
		: "Failed to load ini configuration, using default settings"
	);
	bool customSongsLoaded = CustomMediaLoader::LoadCustomSongsFromFolder();
	if (!ModConfiguration::customSongsEnabled)
	{
		Logging::Write(logPrefix, "Custom songs disabled; skipped custom folder scan");
	}
	else
	{
		Logging::Write(logPrefix,
			customSongsLoaded ? "Custom songs loaded successfully"
			: "Custom songs failed to load"
		);
	}
	if (ModConfiguration::activePlaylist.empty())
	{
		Logging::Write(logPrefix, "Playlist is empty; music player queue disabled");
	}

	ModConfiguration::gameProvider = DetectProvider();
	Logging::Write(logPrefix,
		"Detected game provider: %s",
		ModConfiguration::gameProvider == GameProvider::XBOX_GAMEPASS ? "Xbox Gamepass" : "Steam/Epic"
	);

	ModConfiguration::gameVersion = DetectVersion();
	Logging::Write(logPrefix,
		"Detected game version: %s",
		ModConfiguration::gameVersion == GameVersion::DC ? "DC" : "Standard"
	);

	Logging::Write(logPrefix, "Scanning for function signatures...");
	scanProgress.message = "Searching for game functions";
	scanInProgress.store(true);
	PatternScanner::ScanAsync<FunctionData>(
		ModConfiguration::Databases::functionDatabase,
		[]() {
			Logging::Write(logPrefix, "Function signature scanning complete.");
			scanInProgress.store(false);

			bool hookResult = TryHookFunction("GamePreExit", &GamePreExitHook);
			Logging::Write(logPrefix,
				"GamePreExit function hook %s",
				hookResult ? "installed successfully" : "failed"
			);

			hookResult = TryHookFunction("RenderTask", &RenderTaskHook);
			Logging::Write(logPrefix,
				"RenderTask function hook %s",
				hookResult ? "installed successfully" : "failed"
			);

			hookResult = TryHookFunction("GamePreLoad", &GamePreLoadHook);
			Logging::Write(logPrefix,
				"GamePreLoad function hook %s",
				hookResult ? "installed successfully" : "failed"
			);

			hookResult = TryHookFunction("AccessMusicPool", &AccessMusicPoolHook);
			Logging::Write(logPrefix,
				"AccessMusicPool function hook %s",
				hookResult ? "installed successfully" : "failed"
			);
		}, &scanProgress
	);
}

void ModManager::OnRender()
{
	if (!musicScanStarted && musicPoolScanStartAddress && gamePreLoadCalled)
	{
		musicScanStarted = true;

		std::unordered_map<std::string, MusicData*> allMusicTargets;
		for (auto& [name, data] : ModConfiguration::Databases::interruptorDatabase)
		{
			allMusicTargets[name] = &data;
		}
		for (auto& [name, data] : ModConfiguration::Databases::songDatabase)
		{
			allMusicTargets[name] = &data;
		}

		Logging::Write(logPrefix, "Scanning for music signatures...");
		scanProgress.message = "Searching for game music";
		scanInProgress.store(true);
		PatternScanner::ScanAsyncPtr<MusicData>(
			allMusicTargets,
			[]() {
				scanInProgress.store(false);

				for (auto& [name, data] : ModConfiguration::Databases::interruptorDatabase)
				{
					if (data.address)
					{
						Logging::Write(logPrefix, "Interruptor %s found at address: %p", name.c_str(), data.address);
						data.active = true;
					}
				}

				std::vector<std::string> unexistingSongs{};
				for (auto name : ModConfiguration::activePlaylist)
				{
					auto it = ModConfiguration::Databases::songDatabase.find(name);
					if (it != ModConfiguration::Databases::songDatabase.end())
					{
						auto& songData = it->second;
						if (songData.address)
						{
							Logging::Write(logPrefix, "Song %s found at address: %p", name.c_str(), songData.address);
							songData.active = true;
						}
					}
					else if (ModConfiguration::Databases::customSongDatabase.find(name)
						!= ModConfiguration::Databases::customSongDatabase.end())
					{
						continue;
					}
					else
					{
						unexistingSongs.push_back(name);
					}
				}
				if (unexistingSongs.size() > 0)
				{
					std::string errorMessage = "The following songs are unknown:\n";
					for (const auto& song : unexistingSongs)
					{
						errorMessage += "\"" + song + "\"\n";
					}
					errorMessage += "Please check your playlist inside " + ModConfiguration::configFilePath;
					MemoryUtils::ShowErrorPopup(errorMessage, ModConfiguration::modPublicName);
				}

				if (instance)
				{
					instance->DispatchEvent(ModEvent{ ModEventType::ScanCompleted, nullptr, nullptr });
				}
				Logging::Write(logPrefix, "Setup complete");
			},
			&scanProgress,
			PAGE_READWRITE,
			std::chrono::milliseconds(0),
			false,  // reverse = false
			musicPoolScanStartAddress,
			musicPoolScanStartAddress + 0x1000000,
			false  // mainModuleOnly = false
		);
	}

	ModEvent renderEvent{ ModEventType::FrameRendered, nullptr, nullptr };
	if (ModManager* instance = ModManager::GetInstance())
	{
		instance->DispatchEvent(renderEvent);
	}
}

void ModManager::RegisterListener(IEventListener* listener)
{
	if (listener == nullptr) {
		Logging::Write(logPrefix, "Attempted to register a null listener");
		return;
	}
	listeners.push_back(listener);
}

void ModManager::UnregisterListener(IEventListener* listener)
{
	if (listener == nullptr) {
		Logging::Write(logPrefix, "Attempted to unregister a null listener");
		return;
	}

	auto it = std::remove(listeners.begin(), listeners.end(), listener);
	if (it != listeners.end()) {
		listeners.erase(it, listeners.end());
	}
}

void ModManager::DispatchEvent(const ModEvent& event)
{
	for (auto* listener : listeners)
	{
		listener->OnEvent(event);
	}
}

GameProvider ModManager::DetectProvider()
{
	const std::vector<const char*> gamePassModules = {
		"xgameruntime.dll",
		"Microsoft.Xbox.Services.dll",
		"MicrosoftGame.Config.dll",
		"xg.dll"
	};
	for (const auto* moduleName : gamePassModules)
	{
		if (GetModuleHandleA(moduleName))
		{
			return GameProvider::XBOX_GAMEPASS;
		}
	}

	return GameProvider::STEAM;
}

GameVersion ModManager::DetectVersion()
{
	const std::vector<const wchar_t*> dcFiles = {
		L"XeFX.dll",
		L"XeFX_Loader.dll"
	};
	for (const auto* filename : dcFiles)
	{
		if (MemoryUtils::IsFileNextToExecutable(filename))
		{
			return GameVersion::DC;
		}
	}

	return GameVersion::STANDARD;
}

void ModManager::RenderTaskHook(void* arg1, void* arg2, void* arg3, void* arg4, void* arg5)
{
	const FunctionData* renderTaskFuncData = ModManager::GetFunctionData("RenderTask");
	if (!renderTaskFuncData || !renderTaskFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original RenderTask function was not hooked, cannot call it");
		return;
	}
	using Function_t = void* (*)(void*, void*, void*, void*, void*);
	reinterpret_cast<Function_t>(renderTaskFuncData->originalFunction)(
		arg1, arg2, arg3, arg4, arg5
	);

	OnRender();
}

void ModManager::GamePreExitHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	if (ModManager* instance = ModManager::GetInstance())
	{
		instance->DispatchEvent(ModEvent{ ModEventType::PreExitTriggered, nullptr, nullptr });
	}
	instance = nullptr; // Clear instance to prevent further calls

	const FunctionData* gamePreExitFuncData = ModManager::GetFunctionData("GamePreExit");
	if (!gamePreExitFuncData || !gamePreExitFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original GamePreExit function was not hooked, cannot call it");
		return;
	}
	reinterpret_cast<GenericFunction_t>(gamePreExitFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
}

void ModManager::AccessMusicPoolHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	const FunctionData* accessMusicPoolFuncData = ModManager::GetFunctionData("AccessMusicPool");
	if (!accessMusicPoolFuncData || !accessMusicPoolFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original AccessMusicPool function was not hooked, cannot call it");
		return;
	}
	reinterpret_cast<GenericFunction_t>(accessMusicPoolFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);

	uintptr_t entryAddress = reinterpret_cast<uintptr_t>(arg1);
	Logging::Write(logPrefix,
		"AccessMusicPool function called with entry pointer: %p",
		(void*)entryAddress
	);

	if (!musicPoolScanStartAddress && entryAddress)
	{
		musicPoolScanStartAddress = Utils::KeepTopHex(entryAddress, 4);
		Logging::Write(logPrefix,
			"Music pool start address set to: %p (from %p)",
			(void*)musicPoolScanStartAddress,
			(void*)entryAddress
		);

		bool unhookResult = TryUnhookFunction(*accessMusicPoolFuncData);
		Logging::Write(logPrefix, "AccessMusicPool function unhooking %s",
			unhookResult ? "successful" : "failed"
		);
	}
}

void ModManager::GamePreLoadHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	const FunctionData* gamePreLoadFuncData = ModManager::GetFunctionData("GamePreLoad");
	if (!gamePreLoadFuncData || !gamePreLoadFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original GamePreLoad function was not hooked, cannot call it");
		return;
	}

	reinterpret_cast<GenericFunction_t>(gamePreLoadFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
	gamePreLoadCalled = true;
	Logging::Write(logPrefix, "GamePreLoad function called, gamePreLoadCalled set to true");

	bool unhookResult = TryUnhookFunction(*gamePreLoadFuncData);
	Logging::Write(logPrefix,
		"GamePreLoad function unhooking %s",
		unhookResult ? "successful" : "failed"
	);
}
