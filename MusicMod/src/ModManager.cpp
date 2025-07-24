#include "ModManager.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "OverlayFramework.h"

#include "MemoryUtils.h"
#include "MinHook.h"
#include "PatternScanner.h"

#include "Utils.h"

#include "ModConfiguration.h"

#include "GameData.h"

using namespace OF;

ModManager::ModManager()
{
	logger.Log("Initialized...");
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

	auto& func = it->second;
	if (!func.address)
	{
		return false;
	}

	if (func.usesAVX)
	{
		MemoryUtils::PlaceHook(
			func.address,
			reinterpret_cast<uintptr_t>(hookFunction),
			reinterpret_cast<uintptr_t*>(&func.originalFunction)
		);
	}
	else
	{
		auto created = MH_CreateHook(
			reinterpret_cast<LPVOID>(func.address),
			hookFunction, reinterpret_cast<LPVOID*>(&func.originalFunction)
		);
		if (created != MH_OK)
		{
			return false;
		}

		auto enabled = MH_EnableHook(reinterpret_cast<LPVOID>(func.address));
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

void ModManager::Setup()
{
	InitFramework(device, spriteBatch, window);
	
	if (!ofContext) {
		ofDevice->GetImmediateContext(&ofContext);
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	ImGui::StyleColorsDark();

	// Hook to DX + Win32
	ImGui_ImplWin32_Init(ofWindow);
	ImGui_ImplDX11_Init(ofDevice.Get(), ofContext.Get());

	bool configLoaded = ModConfiguration::LoadConfigFromFile();
	if (configLoaded)
	{
		logger.Log("Ini configuration loaded successfully.");
	}
	else
	{
		logger.Log("Failed to load ini configuration, using default settings.");
	}

	logger.Log("Scanning for function signatures...");
	scanProgress.message = "Searching for game functions";
	scanInProgress.store(true);
	PatternScanner::ScanAsync<FunctionData>(
		ModConfiguration::Databases::functionDatabase,
		[]() {
			logger.Log("Function signature scanning complete.");
			scanInProgress.store(false);

			bool result = TryHookFunction("GamePreExit", &GamePreExitHook);
			if (!result)
			{
				logger.Log("Failed to hook GamePreExit function.");
			}
			else
			{
				logger.Log("GamePreExit function hooked successfully.");
			}

			result = TryHookFunction("GamePreLoad", &GamePreLoadHook);
			if (!result)
			{
				logger.Log("Failed to hook GamePreLoad function.");
			}
			else
			{
				logger.Log("GamePreLoad function hooked successfully.");
			}

			result = TryHookFunction("AccessMusicPool", &AccessMusicPoolHook);
			if (!result)
			{
				logger.Log("Failed to hook AccessMusicPool function.");
			}
			else
			{
				logger.Log("AccessMusicPool function hooked successfully.");
			}
		}, &scanProgress
	);
}

void ModManager::Render()
{
	if (scanInProgress.load())
	{
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// Scanning overlay
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.35f);

		ImGui::Begin("Mod##ScanOverlay", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings);

		ImGui::TextColored(
			ImVec4(1, 0.7f, 0.2f, 1),
			"%s (%s)",
			ModConfiguration::modPublicName.c_str(),
			ModConfiguration::modInternalVersion.c_str()
		);
		ImGui::Separator();

		ImGui::Text(
			"%s: %zu / %zu",
			scanProgress.message,
			scanProgress.matchedPatterns.load(),
			scanProgress.totalPatterns
		);
		ImGui::ProgressBar(scanProgress.GetProgress(), ImVec2(300, 0));
		ImGui::End();

		ImGui::Render();
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	ModEvent renderEvent{ ModEventType::FrameRendered, this, nullptr };
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
		logger.Log("Original GamePreExit function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(gamePreExitFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
}

void ModManager::GamePreLoadHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	logger.Log("GamePreLoadHook called with args: %p, %p, %p, %p",
		arg1, arg2, arg3, arg4
	);

	const FunctionData* gamePreLoadFuncData = ModManager::GetFunctionData("GamePreLoad");
	if (!gamePreLoadFuncData || !gamePreLoadFuncData->originalFunction)
	{
		logger.Log("Original GamePreLoad function was not hooked, cannot call it.");
	}
	else
	{
		reinterpret_cast<GenericFunction_t>(gamePreLoadFuncData->originalFunction)(
			arg1, arg2, arg3, arg4
		);

		if (musicPoolStartAddress)
		{
			std::unordered_map<std::string, MusicData*> allMusicTargets;
			for (auto& [name, data] : ModConfiguration::Databases::sfxDatabase)
			{
				allMusicTargets[name] = &data;
			}
			for (auto& [name, data] : ModConfiguration::Databases::unknownDatabase)
			{
				allMusicTargets[name] = &data;
			}
			for (auto& [name, data] : ModConfiguration::Databases::ambientDatabase)
			{
				allMusicTargets[name] = &data;
			}

			std::vector<std::string> unexistingSongs{};
			for (const std::string& songName : ModConfiguration::activePlaylist)
			{
				auto it = ModConfiguration::Databases::songDatabase.find(songName);
				if (it == ModConfiguration::Databases::songDatabase.end())
				{
					logger.Log("Song %s not found in database, skipping...", songName.c_str());
					unexistingSongs.push_back(songName);
					continue;
				}

				auto& songMusicData = it->second;
				allMusicTargets[songName] = &songMusicData;
			}
			if (unexistingSongs.size() > 0)
			{
				std::string errorMessage = "The following songs were not found:\n";
				for (const auto& song : unexistingSongs)
				{
					errorMessage += "\"" + song + "\"\n";
				}
				errorMessage += "Please check your playlist inside " + ModConfiguration::configFilePath;
				MemoryUtils::ShowErrorPopup(errorMessage, ModConfiguration::modPublicName);
			}

			logger.Log("Scanning for music signatures...");
			scanProgress.message = "Searching for game music";
			scanInProgress.store(true);
			PatternScanner::ScanAsyncPtr<MusicData>(
				allMusicTargets,
				[]() {
					scanInProgress.store(false);
					if (instance)
					{
						instance->DispatchEvent(ModEvent{ ModEventType::ScanCompleted, nullptr, nullptr });
					}
					logger.Log("Setup complete");

					for (auto& [name, data] : ModConfiguration::Databases::sfxDatabase)
					{
						if (data.address)
						{
							logger.Log("SFX %s found at address: %p", name.c_str(), data.address);
						}
					}
					for (auto& [name, data] : ModConfiguration::Databases::unknownDatabase)
					{
						if (data.address)
						{
							logger.Log("Unknown music %s found at address: %p", name.c_str(), data.address);
						}
					}
					for (auto& [name, data] : ModConfiguration::Databases::ambientDatabase)
					{
						if (data.address)
						{
							logger.Log("Ambient music %s found at address: %p", name.c_str(), data.address);
						}
					}
					for (auto& [name, data] : ModConfiguration::Databases::songDatabase)
					{
						if (data.address)
						{
							logger.Log("Song %s found at address: %p", name.c_str(), data.address);
						}
					}
				},
				&scanProgress,
				PAGE_READWRITE,
				std::chrono::milliseconds(0),
				false,
				musicPoolStartAddress,
				musicPoolStartAddress + 0x1000000
			);

			bool unhookResult = TryUnhookFunction(*gamePreLoadFuncData);
			logger.Log("GamePreLoad function unhooking %s",
				unhookResult ? "successful" : "failed"
			);
		}
	}
}

void ModManager::AccessMusicPoolHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	const FunctionData* accessMusicPoolFuncData = ModManager::GetFunctionData("AccessMusicPool");
	if (!accessMusicPoolFuncData || !accessMusicPoolFuncData->originalFunction)
	{
		logger.Log("Original AccessMusicPool function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(accessMusicPoolFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);

	// take the first 4 non-zero hex digits of arg1 as music pool start address
	if (!musicPoolStartAddress)
	{
		musicPoolStartAddress = Utils::KeepTopHex(reinterpret_cast<uintptr_t>(arg1), 4);
		logger.Log("Music pool start address set to: %p", (void*)musicPoolStartAddress);

		bool unhookResult = TryUnhookFunction(*accessMusicPoolFuncData);
		logger.Log("AccessMusicPool function unhooking %s",
			unhookResult ? "successful" : "failed"
		);
	}
}