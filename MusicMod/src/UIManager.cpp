#include "UIManager.h"

#include <locale>
#include <codecvt>

#include "GameStateManager.h"
#include "ModConfiguration.h"
#include "ModManager.h"
#include "MemoryUtils.h"
#include "PatternScanner.h"

#include "InputCode.h"
#include "InputTracker.h"

#include "MinHook.h"

#include "Utils.h"

UIManager::UIManager()
{
	logger.Log("Initialized...");
}

void UIManager::OnEvent(const ModEvent& event)
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
		case ModEventType::InputPressResolved:
		{
			const InputCode& inputCode = std::any_cast<InputCode>(event.data);
			OnInputPress(inputCode);
			break;
		}
		case ModEventType::ChiralNetworkStateChanged:
		{
			if (ModConfiguration::connectToChiralNetwork)
			{
				ChiralNetworkState* chiralNetworkState = std::any_cast<ChiralNetworkState*>(event.data);
				for (auto it = musicPlayerActionButtonMap.begin(); it != musicPlayerActionButtonMap.end(); it++)
				{
					UIButton& button = it.value();
					button.Toggle(*chiralNetworkState == ChiralNetworkState::ON);
				}

				const char* notificationText = (*chiralNetworkState == ChiralNetworkState::ON)
					? "Chiral network on: music player can be activated."
					: "Chiral network off: music player deactivated.";
				ShowNotificationText(notificationText);
			}
			break;
		}
		case ModEventType::MusicPlayerShuffled:
		{
			auto& toggleMusicButton = musicPlayerActionButtonMap.at(UIButtonAction::TOGGLE_MUSIC);
			if (toggleMusicButton.GetCurrentState().id == 0)
			{
				toggleMusicButton.OnPress(false); // Update button text, don't refresh icon (blink)
			}
			break;
		}
		case ModEventType::MusicPlayerInterrupted:
		{
			if (ModConfiguration::showNotificationMessage)
			{
				const char* notificationMessage = "Music player interrupted.";
				if (event.data.has_value() && event.data.type() == typeid(const char**))
				{
					const char** interruptorNamePtr = std::any_cast<const char**>(event.data);
					if (interruptorNamePtr && *interruptorNamePtr)
					{
						notificationMessage = *interruptorNamePtr;
					}
				}
				ShowNotificationText(notificationMessage);
			}
		} // We fall through here intentionally to update the button state
		case ModEventType::MusicPlayerStopped:
		{
			auto& toggleMusicButton = musicPlayerActionButtonMap.at(UIButtonAction::TOGGLE_MUSIC);
			if (toggleMusicButton.GetCurrentState().id == 1)
			{
				toggleMusicButton.OnPress(false);
			}
			break;
		}
		default:
			break;
	}
}

void UIManager::OnScanDone()
{
	if (!ModConfiguration::showMusicPlayerUI)
	{
		logger.Log("Music player UI is disabled in configuration, skipping UI hooks");
		return;
	}

	bool hookResult;
	hookResult = ModManager::TryHookFunction(
		"InGameUIUpdateStaticPoolCaller",
		reinterpret_cast<void*>(&UIManager::InGameUIUpdateStaticPoolCallerHook)
	);
	logger.Log(
		"InGameUIUpdateStaticPoolCaller function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"AccessStaticUIPool",
		reinterpret_cast<void*>(&UIManager::AccessStaticUIPoolHook)
	);
	logger.Log(
		"AccessStaticUIPool function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"UpdateRuntimeUIText",
		reinterpret_cast<void*>(&UIManager::UpdateRuntimeUITextHook)
	);
	logger.Log(
		"UpdateRuntimeUIText function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"InGameUIDrawElement",
		reinterpret_cast<void*>(&UIManager::InGameUIDrawElementHook)
	);
	logger.Log(
		"InGameUIDrawElement function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"InGameUIUpdateElement",
		reinterpret_cast<void*>(&UIManager::InGameUIUpdateElementHook)
	);
	logger.Log(
		"InGameUIUpdateElement function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"PostMenuExit",
		reinterpret_cast<void*>(&UIManager::PostMenuExitHook)
	);
	logger.Log(
		"PostMenuExit function hook %s",
		hookResult ? "installed successfully" : "failed"
	);
}

void UIManager::OnRender()
{
}

void UIManager::OnInputPress(const InputCode& inputCode)
{
	if (currentCompassState == OPEN)
	{
		for (auto it = musicPlayerActionButtonMap.begin(); it != musicPlayerActionButtonMap.end(); it++)
		{
			UIButton& button = it.value();
			if (!button.enabled)
			{
				continue;
			}

			for (const auto& buttonInputCode : button.inputCodes)
			{
				if (inputCode == buttonInputCode)
				{
					button.OnPress(true); // Update state and refresh icon (blink)
					ModManager* instance = ModManager::GetInstance();
					if (instance)
					{
						instance->DispatchEvent(
							ModEvent{ ModEventType::UIButtonPressed, this, button.action }
						);
					}
				}
			}
		}
	}
}

void UIManager::InGameUIUpdateStaticPoolCallerHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("UI Manager");
	const FunctionData* InGameUIUpdateStaticPoolCallerFuncData =
		ModManager::GetFunctionData("InGameUIUpdateStaticPoolCaller");
	if (!InGameUIUpdateStaticPoolCallerFuncData
		|| !InGameUIUpdateStaticPoolCallerFuncData->originalFunction)
	{
		logger.Log("Original InGameUIUpdateStaticPoolCaller function is not found, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(InGameUIUpdateStaticPoolCallerFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);

	// Persist slots in the pool for display in compass UI
	if (currentCompassState == OPEN && staticUIPoolAddress)
	{
		std::lock_guard<std::mutex> lock(updateRuntimeUITextMutex);

		size_t buttonIndex = 0;
		for (auto it = musicPlayerActionButtonMap.begin(); it != musicPlayerActionButtonMap.end(); it++)
		{
			auto& button = it.value();
			const UIButtonState& buttonState = button.GetCurrentState();
			size_t staticSlotIndex = iconSlotIndexMap[button.name];

			uintptr_t orderFlagAddress =
				staticUIPoolAddress + staticSlotOrderOffset + staticSlotIndex * staticSlotSize;
			*(uint8_t*)(orderFlagAddress) = (uint8_t)(++buttonIndex); // force order flag

			uintptr_t activeFlagAddress =
				staticUIPoolAddress + staticSlotFlagOffset + staticSlotIndex * staticSlotSize;
			*(uint16_t*)(activeFlagAddress) = 0x0001; // enable active flag and disable activated flag after it

			uintptr_t grayedFlagAddress =
				staticUIPoolAddress + staticSlotGrayedFlagOffset + staticSlotIndex * staticSlotSize;
			*(uint8_t*)(grayedFlagAddress) = button.enabled ? 0x00 : 0x01; // grayed out flag

			uintptr_t runtimeOrderAddress =
				staticUIPoolAddress + staticSlotRuntimeOrderOffset + staticSlotIndex * staticSlotSize;
			size_t runtimeSlotIndex = *(uint8_t*)(runtimeOrderAddress); // forced order flag dictates this, final runtime order
			if (runtimeSlotIndex >= maxRuntimeSlots)
			{
				continue;
			}
			//logger.Log("Runtime slot index for button %s: %u", button.name, runtimeSlotIndex);

			if (currentRuntimeUIPoolStart && runtimeUITextPoolOffset && runtimeSlotSize)
			{
				void** destinationRuntimeTextPtr = (void**)(
					currentRuntimeUIPoolStart + runtimeUITextPoolOffset + runtimeSlotIndex * runtimeSlotSize
				);
				RuntimeUIText* currentRuntimeText = *(RuntimeUIText**)destinationRuntimeTextPtr;

				bool textNeedsUpdate = true;
				if (currentRuntimeText && currentRuntimeText->data)
				{
					std::wstring_view currentView(currentRuntimeText->data);
					std::wstring expectedW = Utils::Utf8ToWstring(buttonState.text);
					std::wstring_view expectedView(expectedW);
					textNeedsUpdate = currentView != expectedView;

					/*logger.Log("====================================");
					logger.Log("Text: %s", buttonState.text);
					logger.Log("Current View:");
					for (wchar_t wc : currentView)
					{
						logger.Log("0x%04X", wc);
					}

					logger.Log("Expected View:");
					for (wchar_t wc : expectedView)
					{
						logger.Log("0x%04X", wc);
					}

					logger.Log("Text needs update: %s", textNeedsUpdate ? "true" : "false");*/
				}

				if (textNeedsUpdate)
				{
					const FunctionData* createRuntimeUITextFromStringFuncData =
						ModManager::GetFunctionData("CreateRuntimeUITextFromString");
					if (!createRuntimeUITextFromStringFuncData
						|| !createRuntimeUITextFromStringFuncData->address)
					{
						logger.Log(
							"CreateRuntimeUITextFromString function was not found, cannot update text."
						);
						continue;
					}

					const FunctionData* assignRuntimeUITextFuncData =
						ModManager::GetFunctionData("AssignRuntimeUIText");
					if (!assignRuntimeUITextFuncData || !assignRuntimeUITextFuncData->address)
					{
						logger.Log(
							"AssignRuntimeUIText function was not found, cannot assign new text."
						);
						continue;
					}

					const FunctionData* updateRuntimeUITextFuncData =
						ModManager::GetFunctionData("UpdateRuntimeUIText");
					if (!updateRuntimeUITextFuncData || !updateRuntimeUITextFuncData->originalFunction)
					{
						logger.Log(
							"UpdateRuntimeUIText function was not hooked, cannot update runtime text."
						);
						continue;
					}

					logger.Log(
						"Updating runtime text for button %s, slot index: %zu, current text: %s",
						button.name, runtimeSlotIndex, buttonState.text
					);

					void* newRuntimeText = nullptr;
					GenericFunction_t createRuntimeUITextFromStringFunc =
						reinterpret_cast<GenericFunction_t>(createRuntimeUITextFromStringFuncData->address);
					createRuntimeUITextFromStringFunc(
						&newRuntimeText,
						(void*)buttonState.text,
						nullptr,
						nullptr
					);

					GenericFunction_t assignRuntimeUITextFunc =
						reinterpret_cast<GenericFunction_t>(assignRuntimeUITextFuncData->address);
					assignRuntimeUITextFunc(
						destinationRuntimeTextPtr,
						&newRuntimeText,
						nullptr,
						nullptr
					);

					void* richTextInstance = *(void**)(
						currentRuntimeUIPoolStart + updateRuntimeUITextRCXOffset
						+ runtimeSlotIndex * runtimeSlotSize
					);
					GenericFunction_t updateRuntimeUITextFunc =
						reinterpret_cast<GenericFunction_t>(updateRuntimeUITextFuncData->originalFunction);
					updateRuntimeUITextFunc(
						richTextInstance,
						destinationRuntimeTextPtr,
						nullptr,
						nullptr
					);

					const FunctionData* tryFreeRuntimeUITextFuncData =
						ModManager::GetFunctionData("TryFreeRuntimeUIText");
					if (!tryFreeRuntimeUITextFuncData || !tryFreeRuntimeUITextFuncData->address)
					{
						logger.Log(
							"TryFreeRuntimeUIText function was not found, cannot free old text. "
							"Potential memory leak."
						);
					}
					else
					{
						GenericFunction_t tryFreeRuntimeUITextFunc =
							reinterpret_cast<GenericFunction_t>(tryFreeRuntimeUITextFuncData->address);
						tryFreeRuntimeUITextFunc(
							&newRuntimeText,
							nullptr,
							nullptr,
							nullptr
						);
					}
				}

				if (button.requiresUpdate)
				{
					button.requiresUpdate = false;
					uint8_t* runtimeActiveFlagAddress = (uint8_t*)(
						reinterpret_cast<uintptr_t>(destinationRuntimeTextPtr) + runtimeUITextOffsetToActiveFlag
					);
					*runtimeActiveFlagAddress = 0x00; // disable active flag to refresh the UI element (blink)
				}
			}
		}
	}
}

void UIManager::AccessStaticUIPoolHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Access Static UI Pool Hook");
	staticUIPoolAddress = reinterpret_cast<uintptr_t>(arg4) + offsetToStaticUIPool;

	const FunctionData* functionData = ModManager::GetFunctionData("AccessStaticUIPool");
	if (!functionData || !functionData->originalFunction)
	{
		logger.Log("Original AccessStaticUIPool function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(functionData->originalFunction)(arg1, arg2, arg3, arg4);
}

void UIManager::UpdateRuntimeUITextHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Update Runtime UI Text Hook");
	if (currentRuntimeUIPoolStart && !runtimeUITextPoolOffset)
	{
		// First time this is called, calculate offset of runtime UI text pool from the runtime pool start
		// Arg 2 is the runtime pool start + offset + slotSize * slotIndex. First call, slotIndex is 0.
		// this changes between game versions (0x248 for standard, and 0x258 for director's cut)
		if (arg2 > (void*)currentRuntimeUIPoolStart)
		{
			uintptr_t runtimeTextPoolStart = reinterpret_cast<uintptr_t>(arg2);
			runtimeUITextPoolOffset = runtimeTextPoolStart - currentRuntimeUIPoolStart;
			logger.Log("Runtime UI pool start address: %p, runtime text pool address: %p, offset: %p",
				(void*)currentRuntimeUIPoolStart, (void*)(runtimeTextPoolStart),
				(void*)runtimeUITextPoolOffset
			);
		}
	}

	const FunctionData* updateRuntimeUITextFuncData =
		ModManager::GetFunctionData("UpdateRuntimeUIText");
	if (!updateRuntimeUITextFuncData || !updateRuntimeUITextFuncData->originalFunction)
	{
		logger.Log("Original UpdateRuntimeUIText function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(updateRuntimeUITextFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
}

void UIManager::InGameUIUpdateElementHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("UI Manager");

	// arg1 is runtime pool start address + 0xC8 + slotSize * slotIndex
	if (currentRuntimeUIPoolStart)
	{
		if (!runtimeSlotSize && (uintptr_t)arg1 - currentRuntimeUIPoolStart > 0xC8)
		{
			runtimeSlotSize = (uintptr_t)arg1 - currentRuntimeUIPoolStart - 0xC8;
			logger.Log("Runtime UI slot size: %zu", runtimeSlotSize);
		}
	}

	const FunctionData* inGameUIUpdateElementFuncData =
		ModManager::GetFunctionData("InGameUIUpdateElement");
	if (!inGameUIUpdateElementFuncData || !inGameUIUpdateElementFuncData->originalFunction)
	{
		logger.Log("Original InGameUIUpdateElement function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(inGameUIUpdateElementFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
}

void UIManager::InGameUIDrawElementHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("UI Manager");
	currentRuntimeUIPoolStart = reinterpret_cast<uintptr_t>(arg1);

	const FunctionData* inGameUIDrawElementFuncData =
		ModManager::GetFunctionData("InGameUIDrawElement");
	if (!inGameUIDrawElementFuncData || !inGameUIDrawElementFuncData->originalFunction)
	{
		logger.Log("Original InGameUIDrawElement function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(inGameUIDrawElementFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);

	size_t runtimeSlotIndex = reinterpret_cast<size_t>(arg2);
	if (runtimeSlotIndex == 0) // New UI group so we reset the compass state
	{
		ResetModCompassState();
	}
	if (currentCompassState == INCOMPATIBLE)
	{
		return; // Don't update compass state if it's incompatible, wait until reset
	}

	// Update compass state
	void** destinationRuntimeTextPtr = (void**)(
		currentRuntimeUIPoolStart + runtimeUITextPoolOffset + runtimeSlotIndex * runtimeSlotSize
	);
	RuntimeUIText* currentRuntimeText = *(RuntimeUIText**)destinationRuntimeTextPtr;
	std::wstring_view currentView(currentRuntimeText->data);

	uint8_t* runtimeActiveFlagAddress = (uint8_t*)(
		reinterpret_cast<uintptr_t>(destinationRuntimeTextPtr) + runtimeUITextOffsetToActiveFlag
	);
	uint8_t* runtimeIconIdAddress = (uint8_t*)(
		reinterpret_cast<uintptr_t>(destinationRuntimeTextPtr) + runtimeUITextOffsetToIconId
	);
	
	// We check for INCOMPATIBLE state first, then OPEN state
	bool incompatibleMatched = CheckForCompassState(INCOMPATIBLE, *runtimeIconIdAddress, currentView);
	if (*runtimeActiveFlagAddress && incompatibleMatched)
	{
		logger.Log(
			"Found text \"%s\" at runtime slot index %zu. Compass state set to INCOMPATIBLE",
			Utils::WstringToUtf8(std::wstring(currentView)),
			runtimeSlotIndex
		);
		currentCompassState = INCOMPATIBLE;
		return; // No need to check for OPEN state if INCOMPATIBLE matched
	}

	bool openMatched = CheckForCompassState(OPEN, *runtimeIconIdAddress, currentView);
	if (*runtimeActiveFlagAddress && openMatched)
	{
		logger.Log(
			"Found text \"%s\" at runtime slot index %zu. Compass state set to OPEN",
			Utils::WstringToUtf8(std::wstring(currentView)),
			runtimeSlotIndex
		);
		currentCompassState = OPEN;
	}
}

void UIManager::PostMenuExitHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("UI Manager");

	ResetModCompassState();
	logger.Log("Compass state reset to CLOSED on menu exit.");

	const FunctionData* postMenuExitFuncData = ModManager::GetFunctionData("PostMenuExit");
	if (!postMenuExitFuncData || !postMenuExitFuncData->originalFunction)
	{
		logger.Log("Original PostMenuExit function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(postMenuExitFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
}

void UIManager::ShowNotificationText(const char* text)
{
	Logger logger("Show Notification Text");

	const FunctionData* createRuntimeUITextFromStringFuncData =
		ModManager::GetFunctionData("CreateRuntimeUITextFromString");
	if (!createRuntimeUITextFromStringFuncData || !createRuntimeUITextFromStringFuncData->address)
	{
		logger.Log("CreateRuntimeUITextFromString function was not found, cannot show notification text.");
		return;
	}

	const FunctionData* getNotificationPoolFuncData =
		ModManager::GetFunctionData("GetNotificationPool");
	if (!getNotificationPoolFuncData || !getNotificationPoolFuncData->address)
	{
		logger.Log("GetNotificationPool function was not found, cannot show notification text.");
		return;
	}

	const FunctionData* drawNotificationTextFuncData =
		ModManager::GetFunctionData("DrawNotificationText");
	if (!drawNotificationTextFuncData || !drawNotificationTextFuncData->address)
	{
		logger.Log("DrawNotificationText function was not found, cannot show notification text.");
		return;
	}

	RuntimeUIText* notificationText = nullptr;
	GenericFunction_t createRuntimeUITextFromStringFunc =
		reinterpret_cast<GenericFunction_t>(createRuntimeUITextFromStringFuncData->address);
	createRuntimeUITextFromStringFunc(
		&notificationText,
		(void*)text,
		nullptr,
		nullptr
	);

	using Func_t = void* (*)();
	Func_t getNotificationPoolFunc = reinterpret_cast<Func_t>(getNotificationPoolFuncData->address);
	void* notificationPoolStart = getNotificationPoolFunc();
	uintptr_t notificationTextAddress = reinterpret_cast<uintptr_t>(notificationPoolStart)
		+ notificationTextOffset;

	// Acquire lock, same as drawNotificationText caller does
	PSRWLOCK lock = (PSRWLOCK)(reinterpret_cast<uintptr_t>(notificationPoolStart) + notificationLockOffset);
	if (!TryAcquireSRWLockExclusive(lock))
	{
		AcquireSRWLockExclusive(lock);
	}

	using DrawNotificationText_t = void (*)(
		void* poolBase,
		void* text,
		uint32_t param3,	// param_3 (type or style?)
		uint8_t param4,		// 4-6 flags (not sure what they do?)
		uint8_t param5,
		uint8_t param6,
		float param7,		// some sort of priority, timestamp?
		uint32_t param8,	// gets OR’d with 1
		uint32_t* param9	// pointer to a uint32_t (?)
	);

	DrawNotificationText_t drawNotificationTextFunc =
		reinterpret_cast<DrawNotificationText_t>(drawNotificationTextFuncData->address);

	uint32_t metadata = 0;
	drawNotificationTextFunc(
		(void*)notificationTextAddress,
		&notificationText,
		0,
		0, 0, 0,
		0.0f,
		0,
		&metadata
	);

	if (lock)
	{
		ReleaseSRWLockExclusive(lock); // release lock after drawing notification text
	}

	const FunctionData* tryFreeRuntimeUITextFuncData =
		ModManager::GetFunctionData("TryFreeRuntimeUIText");
	if (!tryFreeRuntimeUITextFuncData || !tryFreeRuntimeUITextFuncData->address)
	{
		logger.Log(
			"TryFreeRuntimeUIText function was not found, cannot free notification text. "
			"Potential memory leak."
		);
	}
	else
	{
		GenericFunction_t tryFreeRuntimeUITextFunc =
			reinterpret_cast<GenericFunction_t>(tryFreeRuntimeUITextFuncData->address);
		tryFreeRuntimeUITextFunc(
			&notificationText,
			nullptr,
			nullptr,
			nullptr
		);
	}
}

bool UIManager::CheckForCompassState(CompassState stateToCheck,
	uint8_t iconIdToMatch, const std::wstring_view& textViewToMatch)
{
	//bool iconMatched = false;
	bool textMatched = false;

	std::vector<CompassStateData> compassStateReference = UIManager::compassStateReferenceMap[stateToCheck];
	for (const auto& compassStateData : compassStateReference)
	{
		//if (compassStateData.expectedIconValue == iconIdToMatch)
		{
			//iconMatched = true;
			std::wstring expectedW = Utils::Utf8ToWstring(compassStateData.expectedUITextContent);
			std::wstring_view expectedView(expectedW);
			if (textViewToMatch == expectedView)
			{
				textMatched = true;
				break;
			}
		}
		//else
		//{
			//logger.Log(
				//"Icon %02X does not match expected icon %02X for state %d",
				//iconIdToMatch, compassStateData.expectedIconValue, stateToCheck
			//);
		//}

		// Reset if either didn't match
		/*if (!textMatched)
		{
			iconMatched = false;
			textMatched = false;
		}*/
	}

	return textMatched;
}

void UIManager::ResetModCompassState()
{
	currentCompassState = CLOSED; // reset compass state on menu exit
}