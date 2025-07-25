#pragma once

#include "IEventListener.h"
#include "FunctionHook.h"

#include "MemoryWatcher.h"

#include "Logger.h"

enum class ChiralNetworkState
{
	UNKNOWN = -1,
	OFF = 0,
	ON = 1
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

private:
	inline static Logger logger{ "Game State Manager" };

	inline static uintptr_t inGameFlagPoolAddress = 0;

	inline static uintptr_t chiralNetworkFlagOffset = 0x174;
	inline static ChiralNetworkState previousChiralNetworkEnabled = ChiralNetworkState::UNKNOWN;
	inline static ChiralNetworkState chiralNetworkEnabled = ChiralNetworkState::UNKNOWN;
	inline static std::unique_ptr<MemoryWatcher> chiralNetworkFlagWatcher = nullptr;
	inline static uint32_t chiralNetworkWatcherPollingInterval = 100; // ms
};