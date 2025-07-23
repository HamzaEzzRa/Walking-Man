#pragma once

#include <Windows.h>
#include <XInput.h>

#include <set>
#include <unordered_set>
#include <vector>

#include "IEventListener.h"
#include "InputCode.h"
#include "Logger.h"

class InputTracker : public IEventListener
{
public:
	void OnEvent(const ModEvent&) override;

	static bool IsCombinationActive(const std::vector<InputCode>&);
	static bool IsCombinationPressed(const std::vector<InputCode>&);

public:
	inline static constexpr int GAMEPAD_LEFT_TRIGGER_CODE = 0x10000;
	inline static constexpr int GAMEPAD_RIGHT_TRIGGER_CODE = 0x10001;
	inline static constexpr BYTE GAMEPAD_TRIGGER_THRESHOLD = 30; // Threshold for trigger activation

private:
	void SendInputPress(const InputCode&);
	void SendInputDown(const InputCode&);
	void SendInputUp(const InputCode&);

	void PollKeyboard();
	void PollGamepad();
	void HandleGamepadTrigger(BYTE, BYTE&, const InputCode&);

	static std::string HashCombination(const std::vector<InputCode>&);

private:
	Logger logger = Logger("Input Tracker");

	XINPUT_STATE lastGamepadState{};
	bool gamepadConnected = false;
	
	inline static bool keyStates[256] = { false };
	inline static std::set<int> activeGamepadButtons = {};
	inline static BYTE lastLeftTrigger = 0;
	inline static BYTE lastRightTrigger = 0;

	inline static std::unordered_set<std::string> lastActiveCombinations = {};
};