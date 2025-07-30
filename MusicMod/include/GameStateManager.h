#pragma once

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
	DETECTED = 3
};

enum class ChiralNetworkFlag
{
	UNKNOWN = -1,
	OFF = 0,
	ON = 1
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
};

class GameStateManager : public IEventListener, public FunctionHook
{
public:
	GameStateManager();
	void OnEvent(const ModEvent&) override;

private:
	void OnScanDone();
	void OnRender();
	void OnPreExit();

	static void InGameFlagUpdateHook(void*, void*, void*, void*);

	template<typename T>
	void OnFlagStateChanged(FlagState<T>&);

	template<typename T>
	static void UpdateEnemyFlagState(FlagState<T>&, uint8_t);

private:
	inline static Logger logger{ "Game State Manager" };

	inline static uintptr_t inGameFlagPoolAddress = 0;

	inline static uintptr_t musicPlayingFlagOffset = 0x5D1; // Should switch to this later instead of the complex breakpoint watcher (Standard)

	inline static FlagState<EnemyTerritoryFlag> btTerritoryState = FlagState<EnemyTerritoryFlag>(
		0xCC, 0xCC, ModEventType::BTTerritoryStateChanged, EnemyTerritoryFlag::UNKNOWN
	);
	inline static FlagState<EnemyTerritoryFlag> muleTerritoryState = FlagState<EnemyTerritoryFlag>(
		0xB90, 0xCC8, ModEventType::MuleTerritoryStateChanged, EnemyTerritoryFlag::UNKNOWN // Pretty much no way around having 2 offsets here
	);
	inline static FlagState<ChiralNetworkFlag> chiralNetworkState = FlagState<ChiralNetworkFlag>(
		0x174, 0x174, ModEventType::ChiralNetworkStateChanged, ChiralNetworkFlag::UNKNOWN
	);
};