#include "GameStateManager.h"

#include "ModManager.h"
#include "PatternScanner.h"

#include "MemoryWatcher.h"

#include "MinHook.h"

GameStateManager::GameStateManager()
{
	logger.Log("Initialized...");
}

void GameStateManager::OnEvent(const ModEvent& event)
{
	switch (event.type)
	{
		case ModEventType::ScanCompleted:
		{
			OnScanDone();
			break;
		}
		case ModEventType::FrameRendered:
		{
			OnRender();
			break;
		}
		case ModEventType::PreExitTriggered:
		{
			OnPreExit();
			break;
		}
		default:
			break;
	}
}

void GameStateManager::OnScanDone()
{
	bool result = ModManager::TryHookFunction(
		"InGameFlagUpdate",
		reinterpret_cast<void*>(&GameStateManager::InGameFlagUpdateHook)
	);
	logger.Log(
		"InGameFlagUpdate function hook %s.",
		result ? "installed successfully" : "failed"
	);
}

void GameStateManager::OnRender()
{
	if (chiralNetworkFlagWatcher)
	{
		if (previousChiralNetworkEnabled != chiralNetworkEnabled)
		{
			previousChiralNetworkEnabled = chiralNetworkEnabled;
			ModManager* instance = ModManager::GetInstance();
			if (instance)
			{
				instance->DispatchEvent(ModEvent{
					ModEventType::ChiralNetworkStateChanged,
					this,
					&chiralNetworkEnabled
				});
			}
		}
	}
}

void GameStateManager::OnPreExit()
{
	if (chiralNetworkFlagWatcher)
	{
		chiralNetworkFlagWatcher->Uninstall();
		chiralNetworkFlagWatcher.reset();
	}
}

void GameStateManager::InGameFlagUpdateHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Game State Manager");

	// This function param signature is VERY complex, we avoid calling it
	// It's alright since we only skip it one time to get the flag pool address and install a watcher
	//originalInGameFlagUpdateFunc(arg1, arg2, arg3, arg4);

	uintptr_t flagPoolAddress = reinterpret_cast<uintptr_t>(arg4);
	if (flagPoolAddress)
	{
		logger.Log("Flag pool address: %p", (void*)flagPoolAddress);
		inGameFlagPoolAddress = flagPoolAddress;

		auto state = *reinterpret_cast<const uint8_t*>(flagPoolAddress + chiralNetworkFlagOffset);
		chiralNetworkEnabled = (state != 0) ? ChiralNetworkState::ON : ChiralNetworkState::OFF;

		chiralNetworkFlagWatcher = std::make_unique<MemoryWatcher>(
			[](const void* newValue) {
				auto state = *reinterpret_cast<const uint8_t*>(newValue);
				chiralNetworkEnabled = (state != 0) ? ChiralNetworkState::ON : ChiralNetworkState::OFF;

				// Makes the mod crash when exiting the game, we dispatch on render instead
				/*ModManager* instance = ModManager::GetInstance();
				if (instance)
				{
					instance->DispatchEvent(ModEvent{
						ModEventType::ChiralNetworkStateChanged,
						nullptr,
						&chiralNetworkEnabled
					});
				}*/
			},
			chiralNetworkWatcherPollingInterval
		);
		chiralNetworkFlagWatcher->Install(
			(void*)(flagPoolAddress + chiralNetworkFlagOffset), sizeof(uint8_t)
		);

		const FunctionData* functionData = ModManager::GetFunctionData("InGameFlagUpdate");
		if (!functionData || !functionData->originalFunction)
		{
			logger.Log("InGameFlagUpdate function not hooked, cannot unhook it");
			return;
		}
		bool result = ModManager::TryUnhookFunction(*functionData);
		logger.Log(
			"InGameFlagUpdate function unhook %s.",
			result ? "successful" : "failed"
		);
	}
}