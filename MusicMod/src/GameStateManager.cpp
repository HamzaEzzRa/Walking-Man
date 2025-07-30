#include "GameStateManager.h"

#include "ModConfiguration.h"
#include "ModManager.h"
#include "PatternScanner.h"

#include "MemoryWatcher.h"

#include "MinHook.h"

GameStateManager::GameStateManager()
{
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
	if (!ModConfiguration::showMusicPlayerUI)
	{
		logger.Log("Music player UI is disabled, skipping game state hooks");
		return;
	}

	bool result = ModManager::TryHookFunction(
		"InGameFlagUpdate",
		reinterpret_cast<void*>(&GameStateManager::InGameFlagUpdateHook)
	);
	logger.Log(
		"InGameFlagUpdate function hook %s",
		result ? "installed successfully" : "failed"
	);
}

void GameStateManager::OnRender()
{
	OnFlagStateChanged(btTerritoryState);
	OnFlagStateChanged(muleTerritoryState);
	OnFlagStateChanged(chiralNetworkState);
}

void GameStateManager::OnPreExit()
{
	if (btTerritoryState.flagWatcher)
	{
		btTerritoryState.flagWatcher->Uninstall();
		btTerritoryState.flagWatcher.reset();
	}
	if (muleTerritoryState.flagWatcher)
	{
		muleTerritoryState.flagWatcher->Uninstall();
		muleTerritoryState.flagWatcher.reset();
	}
	if (chiralNetworkState.flagWatcher)
	{
		chiralNetworkState.flagWatcher->Uninstall();
		chiralNetworkState.flagWatcher.reset();
	}
}

void GameStateManager::InGameFlagUpdateHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Game State Manager");

	// This function param signature is VERY complex, we avoid calling it
	// It's alright since we only skip it one time to get the flag pool address and install memory watchers
	//originalInGameFlagUpdateFunc(...);

	uintptr_t flagPoolAddress = reinterpret_cast<uintptr_t>(arg4);
	if (flagPoolAddress)
	{
		logger.Log("Flag pool address: %p", (void*)flagPoolAddress);
		inGameFlagPoolAddress = flagPoolAddress;

		// **Dispatch events on render, safer that way, especially on game exit**

		bool useDCOffset = ModConfiguration::gameVersion == GameVersion::DC;

		uintptr_t btFlagAddress = flagPoolAddress
			+ (useDCOffset ? btTerritoryState.dcFlagPoolOffset : btTerritoryState.standardFlagPoolOffset);
		auto btFlag = *reinterpret_cast<const uint8_t*>(btFlagAddress);
		UpdateEnemyFlagState(btTerritoryState, btFlag);

		auto& btStateRef = btTerritoryState;
		btTerritoryState.flagWatcher = std::make_unique<MemoryWatcher>(
			[&btStateRef](const void* newValue) {
				auto btFlag = *reinterpret_cast<const uint8_t*>(newValue);
				UpdateEnemyFlagState(btStateRef, btFlag);
			},
			btTerritoryState.pollingInterval
		);
		btTerritoryState.flagWatcher->Install((void*)(btFlagAddress), sizeof(uint8_t));

		uintptr_t muleFlagAddress = flagPoolAddress
			+ (useDCOffset ? muleTerritoryState.dcFlagPoolOffset : muleTerritoryState.standardFlagPoolOffset);
		auto muleFlag = *reinterpret_cast<const uint8_t*>(muleFlagAddress);
		UpdateEnemyFlagState(muleTerritoryState, muleFlag);
		
		auto& muleStateRef = muleTerritoryState;
		muleTerritoryState.flagWatcher = std::make_unique<MemoryWatcher>(
			[&muleStateRef](const void* newValue) {
				auto muleFlag = *reinterpret_cast<const uint8_t*>(newValue);
				UpdateEnemyFlagState(muleStateRef, muleFlag);
			},
			muleTerritoryState.pollingInterval
		);
		muleTerritoryState.flagWatcher->Install((void*)(muleFlagAddress), sizeof(uint8_t));

		uintptr_t chiralNetworkFlagAddress = flagPoolAddress
			+ (useDCOffset ? chiralNetworkState.dcFlagPoolOffset : chiralNetworkState.standardFlagPoolOffset);
		auto chiralFlag = *reinterpret_cast<const uint8_t*>(chiralNetworkFlagAddress);
		chiralNetworkState.current = (chiralFlag != 0) ? ChiralNetworkFlag::ON : ChiralNetworkFlag::OFF;
		
		auto& chiralStateRef = chiralNetworkState;
		chiralNetworkState.flagWatcher = std::make_unique<MemoryWatcher>(
			[&chiralStateRef](const void* newValue) {
				auto chiralFlag = *reinterpret_cast<const uint8_t*>(newValue);
				chiralStateRef.current = (chiralFlag != 0) ? ChiralNetworkFlag::ON : ChiralNetworkFlag::OFF;
			},
			chiralNetworkState.pollingInterval
		);
		chiralNetworkState.flagWatcher->Install((void*)(chiralNetworkFlagAddress), sizeof(uint8_t));

		const FunctionData* functionData = ModManager::GetFunctionData("InGameFlagUpdate");
		if (!functionData || !functionData->originalFunction)
		{
			logger.Log("InGameFlagUpdate function not hooked, cannot unhook it");
			return;
		}
		bool result = ModManager::TryUnhookFunction(*functionData);
		logger.Log(
			"InGameFlagUpdate function unhook %s",
			result ? "successful" : "failed"
		);
	}
}

template<typename T>
void GameStateManager::OnFlagStateChanged(FlagState<T>& flagState)
{
	if (flagState.flagWatcher)
	{
		if (flagState.current != flagState.previous)
		{
			ModManager* instance = ModManager::GetInstance();
			if (instance)
			{
				instance->DispatchEvent(ModEvent{
					flagState.dispatchEventType,
					this,
					&flagState
				});
			}
			flagState.previous = flagState.current;
		}
	}
}

template<typename T>
void GameStateManager::UpdateEnemyFlagState(FlagState<T>& flagState, uint8_t flagValue)
{
	switch (flagValue)
	{
		case 0:
			flagState.current = EnemyTerritoryFlag::SAFE;
			break;
		case 1:
			flagState.current = EnemyTerritoryFlag::THREATENED;
			break;
		case 2:
			flagState.current = EnemyTerritoryFlag::DETECTING;
			break;
		case 3:
			flagState.current = EnemyTerritoryFlag::DETECTED;
			break;
		default:
			flagState.current = EnemyTerritoryFlag::UNKNOWN;
			break;
	}
}
