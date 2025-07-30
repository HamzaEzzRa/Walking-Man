#pragma once

#include <Windows.h>

#include <set>
#include <unordered_set>
#include <vector>

#include "FunctionHook.h"
#include "IEventListener.h"
#include "MemoryWatcher.h"

#include "InputCode.h"
#include "Logger.h"

#define GAMEPAD_DPAD_RIGHT	0x0000000011000000
#define GAMEPAD_DPAD_LEFT	0x0000000024000000
#define GAMEPAD_DPAD_UP		0x0000000048000000
#define GAMEPAD_DPAD_DOWN	0x0000000082000000

#define GAMEPAD_A			0x0000004000000000
#define GAMEPAD_B			0x0000008000000000
#define GAMEPAD_Y			0x0000010000000000
#define GAMEPAD_X			0x0000020000000000

#define GAMEPAD_LB			0x0001040000000000
#define GAMEPAD_RB			0x0002080000000000
#define GAMEPAD_LT			0x0004100000000000
#define GAMEPAD_RT			0x0008200000000000

#define GAMEPAD_LS			0x0010000000000000
#define GAMEPAD_RS			0x0020000000000000

class InputTracker : public IEventListener, public FunctionHook
{
public:
	void OnEvent(const ModEvent&) override;

	static bool IsCombinationActive(const std::vector<InputCode>&);
	static bool IsCombinationPressed(const std::vector<InputCode>&);

private:
	void OnScanDone();
	void OnRender();
	void OnPreExit();

	void SendInputPress(const InputCode&);
	void SendInputDown(const InputCode&);
	void SendInputUp(const InputCode&);

	void PollKeyboard();
	void PollGamepad();

	static std::string HashCombination(const std::vector<InputCode>&);
	static const char* LookupBindingName(uint64_t);
	static uint64_t LookupBindingBit(const std::string&);

	static void ProcessControllerInputHook(void*, void*, void*, void*);

private:
	Logger logger = Logger("Input Tracker");

	inline static uintptr_t controllerInputMaskAddress = 0; // Address to watch for controller input mask
	inline static uintptr_t controllerInputMaskOffset = 0x10; // Offset from the base address to the mask
	inline static std::unique_ptr<MemoryWatcher> controllerInputMaskWatcher = nullptr;
	inline static uint32_t controllerInputMaskPollingInterval = 100; // Watcher polling interval in ms
	inline static uint64_t currentControllerInputMask = 0;
	inline static uint64_t lastControllerInputMask = 0;

	inline static std::vector<std::pair<uint64_t, const char*>> inGameBindings = {
		{0x0000000011000000, "DPAD_RIGHT"},
		{0x0000000024000000, "DPAD_LEFT"},
		{0x0000000048000000, "DPAD_UP"},
		{0x0000000082000000, "DPAD_DOWN"},

		{0x0000004000000000, "A"},
		{0x0000008000000000, "B"},
		{0x0000010000000000, "Y"},
		{0x0000020000000000, "X"},

		{0x0001040000000000, "LB"},
		{0x0002080000000000, "RB"},
		{0x0004100000000000, "LT"},
		{0x0008200000000000, "RT"},
		
		{0x0010000000000000, "LS"},
		{0x0020000000000000, "RS"}
	};
	
	inline static bool keyStates[256] = { false };
	inline static std::set<int> activeGamepadButtons = {};

	inline static std::unordered_set<std::string> lastActiveCombinations = {};
};