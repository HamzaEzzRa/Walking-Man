#include "GameStateManager.h"

#include "ModConfiguration.h"
#include "ModManager.h"

#include "MemoryWatcher.h"
#include "MemoryUtils.h"

#include "MinHook.h"

GameStateManager::GameStateManager()
{
}

void GameStateManager::OnEvent(const ModEvent& event)
{
	switch (event.type)
	{
		case ModEventType::FunctionScanCompleted:
		{
			OnFunctionScanDone();
			break;
		}
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
		"InGameAreaUpdate",
		reinterpret_cast<void*>(&GameStateManager::InGameAreaUpdateHook)
	);
	Logging::Write(logPrefix,
		"InGameAreaUpdate function hook %s",
		result ? "installed successfully" : "failed"
	);
}

void GameStateManager::OnFunctionScanDone()
{
	if (!ModConfiguration::showMusicPlayerUI)
	{
		return;
	}

	bool result = ModManager::TryHookFunction(
		"WwiseRTPCStateActivate",
		reinterpret_cast<void*>(&GameStateManager::WwiseRTPCStateActivateHook)
	);
	Logging::Write(logPrefix,
		"WwiseRTPCStateActivate function hook %s",
		result ? "installed successfully" : "failed"
	);
}

void GameStateManager::OnRender()
{
	OnFlagStateChanged(btTerritoryState);
	OnFlagStateChanged(muleTerritoryState);

	OnFlagStateChanged(facilityAreaState);
	OnFlagStateChanged(privateRoomAreaState);
	
	OnFlagStateChanged(chiralNetworkState);

	OnFlagStateChanged(cutsceneState);
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

	if (privateRoomAreaState.flagWatcher)
	{
		privateRoomAreaState.flagWatcher->Uninstall();
		privateRoomAreaState.flagWatcher.reset();
	}
	if (facilityAreaState.flagWatcher)
	{
		facilityAreaState.flagWatcher->Uninstall();
		facilityAreaState.flagWatcher.reset();
	}

	if (chiralNetworkState.flagWatcher)
	{
		chiralNetworkState.flagWatcher->Uninstall();
		chiralNetworkState.flagWatcher.reset();
	}

	if (cutsceneState.flagWatcher)
	{
		cutsceneState.flagWatcher->Uninstall();
		cutsceneState.flagWatcher.reset();
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

		uintptr_t privateRoomFlagAddress = flagPoolAddress + privateRoomAreaState.GetFlagOffset(useDCOffset);
		auto privateRoomFlag = *reinterpret_cast<const uint8_t*>(privateRoomFlagAddress);
		privateRoomAreaState.current = (privateRoomFlag != 0) ? AreaFlag::INSIDE : AreaFlag::OUTSIDE;

		auto& privateRoomStateRef = privateRoomAreaState;
		privateRoomAreaState.flagWatcher = std::make_unique<MemoryWatcher>(
		    [&privateRoomStateRef](const void* newValue)
		    {
			    auto privateRoomFlag = *reinterpret_cast<const uint8_t*>(newValue);
			    privateRoomStateRef.current = (privateRoomFlag != 0) ? AreaFlag::INSIDE : AreaFlag::OUTSIDE;
		    },
		    privateRoomAreaState.pollingInterval);
		privateRoomAreaState.flagWatcher->Install((void*)privateRoomFlagAddress, sizeof(uint8_t));

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

void GameStateManager::InGameAreaUpdateHook(void* manager)
{
	const FunctionData* functionData = ModManager::GetFunctionData("InGameAreaUpdate");
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

	size_t facilityFlagOffset = facilityAreaState.GetFlagOffset(ModConfiguration::gameVersion == GameVersion::DC);
	uintptr_t facilityFlagAddress = managerAddress + facilityFlagOffset;
	auto facilityFlag = *reinterpret_cast<const uint8_t*>(facilityFlagAddress);
	facilityAreaState.current = (facilityFlag != 0) ? AreaFlag::INSIDE : AreaFlag::OUTSIDE;

	auto& facilityStateRef = facilityAreaState;
	facilityAreaState.flagWatcher = std::make_unique<MemoryWatcher>(
		[&facilityStateRef](const void* newValue) {
			auto facilityFlag = *reinterpret_cast<const uint8_t*>(newValue);
			facilityStateRef.current = (facilityFlag != 0) ? AreaFlag::INSIDE : AreaFlag::OUTSIDE;
		},
		facilityAreaState.pollingInterval
	);
	facilityAreaState.flagWatcher->Install((void*)facilityFlagAddress, sizeof(uint8_t));

	if (!functionData->originalFunction)
	{
		Logging::Write(logPrefix, "InGameAreaUpdate function not hooked, cannot unhook it");
		return;
	}
	bool result = ModManager::TryUnhookFunction(*functionData);
	Logging::Write(logPrefix,
		"InGameAreaUpdate function unhook %s",
		result ? "successful" : "failed"
	);
}

void GameStateManager::WwiseRTPCStateActivateHook(void* manager, void* state, void* mask)
{
	const FunctionData* functionData = ModManager::GetFunctionData("WwiseRTPCStateActivate");
	if (functionData && functionData->originalFunction)
	{
		reinterpret_cast<void(*)(void*, void*, void*)>(functionData->originalFunction)(
			manager, state, mask
		);
	}

	const uintptr_t managerAddress = reinterpret_cast<uintptr_t>(manager);
	if (
		!managerAddress
		|| !MemoryUtils::IsReadableAddress(
			managerAddress + rtpcManagerBucketCountOffset,
			sizeof(uint32_t)
		)
		|| !MemoryUtils::IsReadableAddress(
			managerAddress + rtpcManagerBucketsOffset,
			sizeof(uintptr_t)
		)
	)
	{
		return;
	}

	const uintptr_t buckets =
		*reinterpret_cast<const uintptr_t*>(managerAddress + rtpcManagerBucketsOffset);
	const uint32_t bucketCount =
		*reinterpret_cast<const uint32_t*>(managerAddress + rtpcManagerBucketCountOffset);
	if (!buckets || !bucketCount)
	{
		return;
	}

	const uintptr_t bucketAddress =
		buckets + static_cast<uintptr_t>(cutsceneMusicPauseRtpcId % bucketCount) * sizeof(uintptr_t);
	if (!MemoryUtils::IsReadableAddress(bucketAddress, sizeof(uintptr_t)))
	{
		return;
	}

	uintptr_t nodeAddress = *reinterpret_cast<const uintptr_t*>(bucketAddress);
	while (nodeAddress)
	{
		if (
			!MemoryUtils::IsReadableAddress(nodeAddress, sizeof(uint32_t))
			|| !MemoryUtils::IsReadableAddress(nodeAddress + rtpcNodeNextOffset, sizeof(uintptr_t))
		)
		{
			return;
		}

		const uint32_t nodeKey =
			*reinterpret_cast<const uint32_t*>(nodeAddress);
		if (nodeKey == cutsceneMusicPauseRtpcId)
		{
			break;
		}

		nodeAddress = *reinterpret_cast<const uintptr_t*>(nodeAddress + rtpcNodeNextOffset);
	}

	if (!nodeAddress)
	{
		return;
	}

	const uintptr_t countAddress = nodeAddress + rtpcNodeActiveCountOffset;
	if (!MemoryUtils::IsReadableAddress(countAddress, sizeof(uint32_t)))
	{
		return;
	}

	const uint32_t count = *reinterpret_cast<const uint32_t*>(countAddress);
	const CutsceneFlag current = count != 0 ? CutsceneFlag::ACTIVE : CutsceneFlag::INACTIVE;
	const bool firstInstall =
		cutsceneState.current == CutsceneFlag::UNKNOWN && cutsceneState.previous == CutsceneFlag::UNKNOWN;
	cutsceneState.current = current;
	if (firstInstall && current == CutsceneFlag::INACTIVE)
	{
		cutsceneState.previous = current;
	}

	auto& cutsceneStateRef = cutsceneState;
	cutsceneState.flagWatcher = std::make_unique<MemoryWatcher>(
		[&cutsceneStateRef](const void* newValue)
		{
			const uint32_t count = *reinterpret_cast<const uint32_t*>(newValue);
			const CutsceneFlag next = count != 0 ? CutsceneFlag::ACTIVE : CutsceneFlag::INACTIVE;
			if (cutsceneStateRef.current != next)
			{
				cutsceneStateRef.current = next;
			}
		},
		cutsceneState.pollingInterval
	);
	cutsceneState.flagWatcher->Install(reinterpret_cast<const void*>(countAddress), sizeof(uint32_t));

	if (functionData)
	{
		const bool result = ModManager::TryUnhookFunction(*functionData);
		Logging::Write(logPrefix,
			"WwiseRTPCStateActivate function unhook %s",
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
