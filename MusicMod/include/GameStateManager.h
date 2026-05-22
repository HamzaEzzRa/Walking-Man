#pragma once

#include <cstdint>
#include <cstddef>

#include "IEventListener.h"
#include "FunctionHook.h"

#include "MemoryWatcher.h"

#include "Logger.h"

enum class EnemyTerritoryFlag
{
	UNKNOWN = -1,
	SAFE = 0,
	THREATENED = 1,
	DETECTING = 2,
	DETECTED = 3,
};

enum class AreaFlag
{
	UNKNOWN = -1,
	OUTSIDE = 0,
	INSIDE = 1,
};

enum class ChiralNetworkFlag
{
	UNKNOWN = -1,
	OFF = 0,
	ON = 1,
};

enum class CutsceneFlag
{
	UNKNOWN = -1,
	INACTIVE = 0,
	ACTIVE = 1,
};

template<typename T>
struct FlagState
{
	size_t standardFlagPoolOffset; // offset from the flag pool to where the flag is (Standard version)
	size_t dcFlagPoolOffset; // offset from the flag pool to where the flag is (DC version)
	ModEventType dispatchEventType; // event type to dispatch when the flag state changes
	T previous;
	T current;
	std::unique_ptr<MemoryWatcher> flagWatcher = nullptr;
	uint32_t pollingInterval = 100; // watcher polling interval in ms

	FlagState(size_t standardOffset, size_t dcOffset, ModEventType eventType, T initialState)
		: standardFlagPoolOffset(standardOffset), dcFlagPoolOffset(dcOffset),
		dispatchEventType(eventType), previous(initialState), current(initialState) {}

	size_t GetFlagOffset(bool useDCVersion) const
	{
		return useDCVersion ? dcFlagPoolOffset : standardFlagPoolOffset;
	}
};

class GameStateManager : public IEventListener, public FunctionHook
{
public:
	GameStateManager();
	void OnEvent(const ModEvent&) override;

private:
	void OnScanDone();
	void OnFunctionScanDone();
	void OnRender();
	void OnPreExit();

	static void InGameFlagUpdateHook(void*, void*, void*, void*);
	static void InGameAreaUpdateHook(void*);
	static void WwiseRTPCStateActivateHook(void*, void*, void*);

	template<typename T>
	void OnFlagStateChanged(FlagState<T>&);

	template<typename T>
	static void UpdateEnemyFlagState(FlagState<T>&, uint8_t);

private:
	inline static constexpr const char* logPrefix = "Game State Manager";
	inline static constexpr uint32_t cutsceneMusicPauseRtpcId = 0x28D65BDB;
	inline static constexpr size_t rtpcManagerBucketsOffset = 0x0;
	inline static constexpr size_t rtpcManagerBucketCountOffset = 0x8;
	inline static constexpr size_t rtpcNodeNextOffset = 0x8;
	inline static constexpr size_t rtpcNodeActiveCountOffset = 0x58;

	inline static uintptr_t inGameFlagPoolAddress = 0;

	inline static uintptr_t musicPlayingFlagOffset = 0x5D1; // Should switch to this later instead of the complex breakpoint watcher (Standard)

	inline static FlagState<EnemyTerritoryFlag> btTerritoryState = FlagState<EnemyTerritoryFlag>(
		0xCC, 0xCC, ModEventType::BTTerritoryStateChanged, EnemyTerritoryFlag::UNKNOWN
	);
	inline static FlagState<EnemyTerritoryFlag> muleTerritoryState = FlagState<EnemyTerritoryFlag>(
		0xB90, 0xCC8, ModEventType::MuleTerritoryStateChanged, EnemyTerritoryFlag::UNKNOWN // Pretty much no way around having 2 offsets here
	);
	
	inline static FlagState<AreaFlag> facilityAreaState = FlagState<AreaFlag>(
		0x228, 0x290, ModEventType::FacilityAreaStateChanged, AreaFlag::UNKNOWN
	);
	inline static FlagState<AreaFlag> privateRoomAreaState = FlagState<AreaFlag>(
		0x1E8, 0x1E8, ModEventType::PrivateRoomAreaStateChanged, AreaFlag::UNKNOWN
	);

	inline static FlagState<ChiralNetworkFlag> chiralNetworkState = FlagState<ChiralNetworkFlag>(
		0x174, 0x174, ModEventType::ChiralNetworkStateChanged, ChiralNetworkFlag::UNKNOWN
	);

	inline static FlagState<CutsceneFlag> cutsceneState = FlagState<CutsceneFlag>(
		0x0, 0x0, ModEventType::CutsceneStateChanged, CutsceneFlag::UNKNOWN
	);
};
