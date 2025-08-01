#include "InputTracker.h"

#include <Windows.h>

#include "MemoryUtils.h"
#include "ModManager.h"

void InputTracker::OnEvent(const ModEvent& event)
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
		default:
			break;
	}
}

void InputTracker::OnScanDone()
{
	bool hookResult = ModManager::TryHookFunction(
		"ProcessControllerInput",
		reinterpret_cast<void*>(&InputTracker::ProcessControllerInputHook)
	);
	logger.Log(
		"ProcessControllerInput function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	logger.Log("Setup complete");
}

void InputTracker::OnRender()
{
	PollKeyboard();
	PollGamepad();
}

void InputTracker::ProcessControllerInputHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Process Controller Input Hook");
	const FunctionData* funcData = ModManager::GetFunctionData("ProcessControllerInput");
	if (!funcData || !funcData->originalFunction)
	{
		logger.Log("Original ProcessControllerInput function was not hooked, cannot call it");
		return;
	}
	reinterpret_cast<GenericFunction_t>(funcData->originalFunction)(arg1, arg2, arg3, arg4);

	uintptr_t newAddress = reinterpret_cast<uintptr_t>(arg1) + controllerInputMaskOffset;
	if ((newAddress & 0xFF) == 0x40)
	{
		if (!controllerInputMaskAddress)
		{
			controllerInputMaskAddress = newAddress;
			logger.Log(
				"Controller input mask address: %p",
				(void*)newAddress
			);
		}

		// Update here to avoid debounce issues
		currentControllerInputMask = *reinterpret_cast<uint64_t*>(controllerInputMaskAddress);
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
	if (controllerInputMaskAddress == 0)
	{
		return;
	}

	if (lastControllerInputMask == 0 && currentControllerInputMask == 0)
	{
		return; // No gamepad input detected and no change
	}

	activeGamepadButtons.clear();
	for (const auto& [bit, name] : inGameBindings)
	{
		bool wasDown = (lastControllerInputMask & bit) != 0;
		bool isDown = (currentControllerInputMask & bit) != 0;

		InputCode code{ bit, InputSource::GAMEPAD };

		if (isDown)
		{
			activeGamepadButtons.insert(bit);
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

	lastControllerInputMask = currentControllerInputMask;
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

const char* InputTracker::LookupBindingName(uint64_t bit)
{
	for (const auto& [b, name] : inGameBindings)
	{
		if (b == bit)
		{
			return name;
		}
	}
	return nullptr;
}

uint64_t InputTracker::LookupBindingBit(const std::string& name)
{
	for (const auto& [bit, n] : inGameBindings)
	{
		if (name == n)
		{
			return bit;
		}
	}
	return 0;
}