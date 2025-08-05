#include "ModManager.h"

#include "MemoryUtils.h"
#include "MinHook.h"
#include "PatternScanner.h"

#include "Utils.h"

#include "ModConfiguration.h"

#include "GameData.h"

ModManager::ModManager()
{
	logger.Log("Initializing...");
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
			return false;
		}

		auto enabled = MH_EnableHook(reinterpret_cast<LPVOID>(funcData.address));
		if (enabled != MH_OK)
		{
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
	bool configLoaded = ModConfiguration::LoadConfigFromFile();
	logger.Log(
		configLoaded ? "Ini configuration loaded successfully"
		: "Failed to load ini configuration, using default settings"
	);

	logger.Log("Scanning for function signatures...");
	scanProgress.message = "Searching for game functions";
	scanInProgress.store(true);
	PatternScanner::ScanAsync<FunctionData>(
		ModConfiguration::Databases::functionDatabase,
		[]() {
			logger.Log("Function signature scanning complete.");
			scanInProgress.store(false);

			bool hookResult = TryHookFunction("GamePreExit", &GamePreExitHook);
			logger.Log(
				"GamePreExit function hook %s",
				hookResult ? "installed successfully" : "failed"
			);

			hookResult = TryHookFunction("RenderTask", &RenderTaskHook);
			logger.Log(
				"RenderTask function hook %s",
				hookResult ? "installed successfully" : "failed"
			);

			hookResult = TryHookFunction("GamePreLoad", &GamePreLoadHook);
			logger.Log(
				"GamePreLoad function hook %s",
				hookResult ? "installed successfully" : "failed"
			);

			hookResult = TryHookFunction("AccessMusicPool", &AccessMusicPoolHook);
			logger.Log(
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

		logger.Log("Scanning for music signatures...");
		scanProgress.message = "Searching for game music";
		scanInProgress.store(true);
		PatternScanner::ScanAsyncPtr<MusicData>(
			allMusicTargets,
			[]() {
				scanInProgress.store(false);

				// Determine game version based on exclusive DC songs
				size_t dcCount = 0;
				size_t dcNotFound = 0;
				for (auto& [name, data] : ModConfiguration::Databases::songDatabase)
				{
					if (data.exclusiveDC)
					{
						dcCount++;
						if (!data.address)
						{
							dcNotFound++;
						}
					}
				}
				ModConfiguration::gameVersion = dcCount == dcNotFound ? GameVersion::STANDARD : GameVersion::DC;
				logger.Log("Game version is set to %s",
					ModConfiguration::gameVersion == GameVersion::DC ? "DC" : "STANDARD"
				);

				for (auto& [name, data] : ModConfiguration::Databases::interruptorDatabase)
				{
					if (data.address)
					{
						logger.Log("Interruptor %s found at address: %p", name.c_str(), data.address);
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
							logger.Log("Song %s found at address: %p", name.c_str(), songData.address);
							songData.active = true;
						}
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
				logger.Log("Setup complete");
			},
			&scanProgress,
			PAGE_READWRITE,
			std::chrono::milliseconds(0),
			false,
			musicPoolScanStartAddress,
			musicPoolScanStartAddress + 0x1000000
		);
	}

	ModEvent renderEvent{ ModEventType::FrameRendered, nullptr, nullptr };
	ModManager* instance = ModManager::GetInstance();
	if (instance)
	{
		instance->DispatchEvent(renderEvent);
	}
}

void ModManager::RegisterListener(IEventListener* listener)
{
	if (listener == nullptr) {
		logger.Log("Attempted to register a null listener");
		return;
	}
	listeners.push_back(listener);
}

void ModManager::UnregisterListener(IEventListener* listener)
{
	if (listener == nullptr) {
		logger.Log("Attempted to unregister a null listener");
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

void ModManager::RenderTaskHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	const FunctionData* renderTaskFuncData = ModManager::GetFunctionData("RenderTask");
	if (!renderTaskFuncData || !renderTaskFuncData->originalFunction)
	{
		logger.Log("Original RenderTask function was not hooked, cannot call it");
		return;
	}
	reinterpret_cast<GenericFunction_t>(renderTaskFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);

	OnRender();
}

void ModManager::GamePreExitHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	ModManager* instance = ModManager::GetInstance();
	if (instance)
	{
		instance->DispatchEvent(ModEvent{ ModEventType::PreExitTriggered, nullptr, nullptr });
	}
	instance = nullptr; // Clear instance to prevent further calls

	const FunctionData* gamePreExitFuncData = ModManager::GetFunctionData("GamePreExit");
	if (!gamePreExitFuncData || !gamePreExitFuncData->originalFunction)
	{
		logger.Log("Original GamePreExit function was not hooked, cannot call it");
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
		logger.Log("Original AccessMusicPool function was not hooked, cannot call it");
		return;
	}
	reinterpret_cast<GenericFunction_t>(accessMusicPoolFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);

	// take the first 4 non-zero hex digits of arg1 as music pool start address
	if (!musicPoolScanStartAddress)
	{
		musicPoolScanStartAddress = Utils::KeepTopHex(reinterpret_cast<uintptr_t>(arg1), 4);
		logger.Log("Music pool start address set to: %p (from %p)", (void*)musicPoolScanStartAddress, arg1);

		bool unhookResult = TryUnhookFunction(*accessMusicPoolFuncData);
		logger.Log("AccessMusicPool function unhooking %s",
			unhookResult ? "successful" : "failed"
		);
	}
}

void ModManager::GamePreLoadHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	const FunctionData* gamePreLoadFuncData = ModManager::GetFunctionData("GamePreLoad");
	if (!gamePreLoadFuncData || !gamePreLoadFuncData->originalFunction)
	{
		logger.Log("Original GamePreLoad function was not hooked, cannot call it");
		return;
	}

	reinterpret_cast<GenericFunction_t>(gamePreLoadFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
	gamePreLoadCalled = true;

	bool unhookResult = TryUnhookFunction(*gamePreLoadFuncData);
	logger.Log(
		"GamePreLoad function unhooking %s",
		unhookResult ? "successful" : "failed"
	);
}