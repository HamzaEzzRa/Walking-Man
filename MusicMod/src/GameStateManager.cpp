#include "GameStateManager.h"

#include "ModConfiguration.h"
#include "ModManager.h"

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
		default: break;
	}
}

void GameStateManager::OnScanDone()
{
	if (!ModConfiguration::showMusicPlayerUI)
	{
		Logging::Write(logPrefix, "Music player UI is disabled, skipping game state hooks");
		return;
	}

	bool result = ModManager::TryHookFunction(
		"InGameFlagUpdate",
		reinterpret_cast<void*>(&GameStateManager::InGameFlagUpdateHook)
	);
	Logging::Write(logPrefix,
		"InGameFlagUpdate function hook %s",
		result ? "installed successfully" : "failed"
	);

	result = ModManager::TryHookFunction(
		"FacilityManagerUpdate",
		reinterpret_cast<void*>(&GameStateManager::FacilityManagerUpdateHook)
	);
	Logging::Write(logPrefix,
		"FacilityManagerUpdate function hook %s",
		result ? "installed successfully" : "failed"
	);
}

void GameStateManager::OnRender()
{
	OnFlagStateChanged(btTerritoryState);
	OnFlagStateChanged(muleTerritoryState);
	OnFlagStateChanged(facilityTerritoryState);
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
	if (facilityTerritoryState.flagWatcher)
	{
		facilityTerritoryState.flagWatcher->Uninstall();
		facilityTerritoryState.flagWatcher.reset();
	}
	if (chiralNetworkState.flagWatcher)
	{
		chiralNetworkState.flagWatcher->Uninstall();
		chiralNetworkState.flagWatcher.reset();
	}
}

void GameStateManager::InGameFlagUpdateHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	// This function param signature is VERY complex, we avoid calling it
	// It's alright since we only skip it one time to get the flag pool address and install memory watchers
	//originalInGameFlagUpdateFunc(...);

	uintptr_t flagPoolAddress = reinterpret_cast<uintptr_t>(arg4);
	if (flagPoolAddress)
	{
		Logging::Write(logPrefix, "Flag pool address: %p", (void*)flagPoolAddress);
		inGameFlagPoolAddress = flagPoolAddress;

		// **Dispatch events on render, safer that way, especially on game exit**

		bool useDCOffset = ModConfiguration::gameVersion == GameVersion::DC;

		uintptr_t btFlagAddress = flagPoolAddress + btTerritoryState.GetFlagOffset(useDCOffset);
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

		uintptr_t muleFlagAddress = flagPoolAddress + muleTerritoryState.GetFlagOffset(useDCOffset);
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

		uintptr_t chiralNetworkFlagAddress = flagPoolAddress + chiralNetworkState.GetFlagOffset(useDCOffset);
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
			Logging::Write(logPrefix, "InGameFlagUpdate function not hooked, cannot unhook it");
			return;
		}
		bool result = ModManager::TryUnhookFunction(*functionData);
		Logging::Write(logPrefix,
			"InGameFlagUpdate function unhook %s",
			result ? "successful" : "failed"
		);
	}
}

void GameStateManager::FacilityManagerUpdateHook(void* manager)
{
	const FunctionData* functionData = ModManager::GetFunctionData("FacilityManagerUpdate");
	if (!functionData || !functionData->address)
	{
		return;
	}

	uintptr_t managerAddress = reinterpret_cast<uintptr_t>(manager);
	auto* code = reinterpret_cast<const uint8_t*>(functionData->address);

	if (ModConfiguration::gameProvider == GameProvider::XBOX_GAMEPASS)
	{
		managerAddress = 0;
		for (size_t i = 0; i < 0x80; ++i)
		{
			if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x3D)
			{
				auto rel = *reinterpret_cast<const int32_t*>(code + i + 3);
				managerAddress = *reinterpret_cast<const uintptr_t*>(functionData->address + i + 7 + rel);
				break;
			}
		}
	}
	if (!managerAddress)
	{
		return;
	}

	size_t facilityFlagOffset = facilityTerritoryState.GetFlagOffset(ModConfiguration::gameVersion == GameVersion::DC);
	uintptr_t facilityFlagAddress = managerAddress + facilityFlagOffset;
	auto facilityFlag = *reinterpret_cast<const uint8_t*>(facilityFlagAddress);
	facilityTerritoryState.current = (facilityFlag != 0) ? FacilityFlag::INSIDE : FacilityFlag::OUTSIDE;

	auto& facilityStateRef = facilityTerritoryState;
	facilityTerritoryState.flagWatcher = std::make_unique<MemoryWatcher>(
		[&facilityStateRef](const void* newValue) {
			auto facilityFlag = *reinterpret_cast<const uint8_t*>(newValue);
			facilityStateRef.current = (facilityFlag != 0) ? FacilityFlag::INSIDE : FacilityFlag::OUTSIDE;
		},
		facilityTerritoryState.pollingInterval
	);
	facilityTerritoryState.flagWatcher->Install((void*)facilityFlagAddress, sizeof(uint8_t));
	Logging::Write(logPrefix,
		"Installed watcher on facility flag address: %p",
		(void*)facilityFlagAddress
	);

	if (!functionData->originalFunction)
	{
		Logging::Write(logPrefix, "FacilityManagerUpdate function not hooked, cannot unhook it");
		return;
	}
	bool result = ModManager::TryUnhookFunction(*functionData);
	Logging::Write(logPrefix,
		"FacilityManagerUpdate function unhook %s",
		result ? "successful" : "failed"
	);
}

template<typename T>
void GameStateManager::OnFlagStateChanged(FlagState<T>& flagState)
{
	if (flagState.flagWatcher)
	{
		if (flagState.current != flagState.previous)
		{
			if (ModManager* instance = ModManager::GetInstance())
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
		{
			flagState.current = EnemyTerritoryFlag::SAFE;
			break;
		}
		case 1:
		{
			flagState.current = EnemyTerritoryFlag::THREATENED;
			break;
		}
		case 2:
		{
			flagState.current = EnemyTerritoryFlag::DETECTING;
			break;
		}
		case 3:
		{
			flagState.current = EnemyTerritoryFlag::DETECTED;
			break;
		}
		default:
		{
			flagState.current = EnemyTerritoryFlag::UNKNOWN;
			break;
		}
	}
}
