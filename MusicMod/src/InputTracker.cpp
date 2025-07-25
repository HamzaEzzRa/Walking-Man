#include "InputTracker.h"

#include <Windows.h>
#include <XInput.h>

#include "MemoryUtils.h"
#include "ModManager.h"

void InputTracker::OnEvent(const ModEvent& event)
{
	switch (event.type)
	{
		case ModEventType::ScanCompleted:
		{
			ZeroMemory(&lastGamepadState, sizeof(XINPUT_STATE));
			logger.Log("Setup complete");
			break;
		}
		case ModEventType::FrameRendered:
		{
			PollKeyboard();
			PollGamepad();
			break;
		}
		default:
			break;
	}
}

bool InputTracker::IsCombinationActive(const std::vector<InputCode>& combination)
{
	for (const auto& code : combination)
	{
		if (code.source == InputSource::KBM && !keyStates[code.code])
		{
			return false;
		}
		if (code.source == InputSource::GAMEPAD && activeGamepadButtons.find(code.code) == activeGamepadButtons.end())
		{
			return false;
		}
	}
	return true;
}

bool InputTracker::IsCombinationPressed(const std::vector<InputCode>& combination)
{
	std::string hash = HashCombination(combination);

	bool isActive = IsCombinationActive(combination);
	bool wasActive = lastActiveCombinations.count(hash) > 0;

	if (isActive && !wasActive)
	{
		lastActiveCombinations.insert(hash); // remember it's now active
		return true;
	}
	else if (!isActive && wasActive)
	{
		lastActiveCombinations.erase(hash); // reset once released
	}
	return false;
}

void InputTracker::PollKeyboard()
{
	for (int vk = 1; vk < 256; ++vk)
	{
		SHORT state = GetAsyncKeyState(vk);
		InputCode code{ vk, InputSource::KBM };
		if (state & 0x8000)
		{
			if (!keyStates[vk])
			{
				SendInputPress(code);
			}
			SendInputDown(code);
			keyStates[vk] = true;
		}
		else if (keyStates[vk])
		{
			SendInputUp(code);
			keyStates[vk] = false;
		}
	}
}

void InputTracker::PollGamepad()
{
	XINPUT_STATE currentState;
	ZeroMemory(&currentState, sizeof(XINPUT_STATE));
	DWORD result;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
	{
		result = XInputGetState(i, &currentState);
		if (result == ERROR_SUCCESS) // Controller found
		{
			break;
		}
	}

	if (result != ERROR_SUCCESS)
	{
		if (gamepadConnected)
		{
			logger.Log("Gamepad disconnected.");
			gamepadConnected = false;
		}
		return;
	}

	if (!gamepadConnected)
	{
		logger.Log("Gamepad connected.");
		gamepadConnected = true;
	}

	// Handle triggers
	HandleGamepadTrigger(
		currentState.Gamepad.bLeftTrigger,
		lastLeftTrigger,
		{ GAMEPAD_LEFT_TRIGGER_CODE, InputSource::GAMEPAD }
	);
	HandleGamepadTrigger(
		currentState.Gamepad.bRightTrigger,
		lastRightTrigger,
		{ GAMEPAD_RIGHT_TRIGGER_CODE, InputSource::GAMEPAD }
	);

	// Handle buttons
	WORD oldButtons = lastGamepadState.Gamepad.wButtons;
	WORD newButtons = currentState.Gamepad.wButtons;

	activeGamepadButtons.clear();
	for (WORD mask = 1; mask != 0; mask <<= 1)
	{
		bool wasDown = (oldButtons & mask) != 0;
		bool isDown = (newButtons & mask) != 0;
		if (isDown)
		{
			activeGamepadButtons.insert(mask);
		}

		InputCode code{ static_cast<int>(mask), InputSource::GAMEPAD };
		if (isDown)
		{
			if (!wasDown)
			{
				SendInputPress(code);
			}
			SendInputDown(code);
		}
		else if (wasDown)
		{
			SendInputUp(code);
		}
	}

	lastGamepadState = currentState;
}

void InputTracker::HandleGamepadTrigger(BYTE currentValue,
	BYTE& lastValue, const InputCode& code)
{
	if (currentValue > GAMEPAD_TRIGGER_THRESHOLD)
	{
		if (lastValue <= GAMEPAD_TRIGGER_THRESHOLD)
		{
			SendInputPress(code);
		}
		SendInputDown(code);
	}
	else if (lastValue > GAMEPAD_TRIGGER_THRESHOLD)
	{
		SendInputUp(code);
	}
	lastValue = currentValue;
}

void InputTracker::SendInputPress(const InputCode& code)
{
	ModManager* instance = ModManager::GetInstance();
	if (instance)
	{
		instance->DispatchEvent(ModEvent{ ModEventType::InputPressResolved, this, code });
	}
}

void InputTracker::SendInputDown(const InputCode& code)
{
	ModManager* instance = ModManager::GetInstance();
	if (instance)
	{
		instance->DispatchEvent(ModEvent{ ModEventType::InputDownResolved, this, code });
	}
}

void InputTracker::SendInputUp(const InputCode& code)
{
	ModManager* instance = ModManager::GetInstance();
	if (instance)
	{
		instance->DispatchEvent(ModEvent{ ModEventType::InputUpResolved, this, code });
	}
}

std::string InputTracker::HashCombination(const std::vector<InputCode>& combination)
{
	std::string hash;
	for (const auto& code : combination)
	{
		hash += std::to_string(static_cast<int>(code.source)) + ":" + std::to_string(code.code) + "|";
	}
	return hash;
}