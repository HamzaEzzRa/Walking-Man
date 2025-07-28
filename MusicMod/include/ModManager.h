#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "IRenderCallback.h"

#include "FunctionHook.h"
#include "IEventListener.h"
#include "PatternScanner.h"

#include "GameData.h"

class ModManager : public IRenderCallback, public FunctionHook
{
public:
	ModManager();

	static void SetInstance(ModManager*);
	static ModManager* GetInstance();
	static bool TryHookFunction(const std::string&, void*);
	static bool TryUnhookFunction(const FunctionData&);
	static const FunctionData* GetFunctionData(const std::string&);

	void Setup();
	void Render();

	void RegisterListener(IEventListener*);
	void UnregisterListener(IEventListener*);
	void DispatchEvent(const ModEvent&);

private:
	static void GamePreExitHook(void*, void*, void*, void*);

	static void AccessMusicPoolHook(void*, void*, void*, void*);
	static void GamePreLoadHook(void*, void*, void*, void*);

private:
	inline static ModManager* instance = nullptr;

	inline static std::vector<IEventListener*> listeners{};
	inline static Logger logger = Logger("Mod Manager");

	inline static uintptr_t musicPoolScanStartAddress = 0;
	inline static bool gamePreLoadCalled = false;
	inline static bool musicScanStarted = false;

	inline static Microsoft::WRL::ComPtr<ID3D11DeviceContext> ofContext;
	inline static PatternScanner::ScanProgress scanProgress{};
	inline static std::atomic<bool> scanInProgress{ false };
};