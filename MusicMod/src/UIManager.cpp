#include "UIManager.h"

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
		case ModEventType::TextLanguageChanged:
		{
			OnTextLanguageChange();
			break;
		}
		case ModEventType::BTTerritoryStateChanged:
		{
			auto* territoryFlagState = std::any_cast<FlagState<EnemyTerritoryFlag>*>(event.data);
			UpdateMusicPlayerUIBlockers(
				MusicPlayerUIBlocker::BT_BLOCK, territoryFlagState->current != EnemyTerritoryFlag::SAFE
			);
			break;
		}
		case ModEventType::MuleTerritoryStateChanged:
		{
			auto* territoryFlagState = std::any_cast<FlagState<EnemyTerritoryFlag>*>(event.data);
			UpdateMusicPlayerUIBlockers(
				MusicPlayerUIBlocker::MULE_BLOCK, territoryFlagState->current != EnemyTerritoryFlag::SAFE
			);
			break;
		}
		case ModEventType::FacilityBlockStateChanged:
		{
		    if (ModConfiguration::stopInFacility)
		    {
			    auto* areaFlagState = std::any_cast<FlagState<AreaFlag>*>(event.data);
			    UpdateMusicPlayerUIBlockers(
					MusicPlayerUIBlocker::FACILITY_BLOCK, areaFlagState->current == AreaFlag::INSIDE
				);
		    }
		    break;
	    }
		case ModEventType::ChiralNetworkStateChanged:
		{
			if (ModConfiguration::connectToChiralNetwork)
			{
				auto* chiralNetworkFlag = std::any_cast<FlagState<ChiralNetworkFlag>*>(event.data);
				UpdateMusicPlayerUIBlockers(
					MusicPlayerUIBlocker::CHIRAL_BLOCK, chiralNetworkFlag->current == ChiralNetworkFlag::OFF
				);
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
				ShowNotificationText(
					LanguageManager::GetLocalizedText(std::string(notificationMessage)).c_str()
				);
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
		default: break;
	}
}

void UIManager::OnScanDone()
{
	if (!ModConfiguration::showMusicPlayerUI)
	{
		Logging::Write(logPrefix, "Music player UI is disabled in configuration, skipping UI hooks");
		return;
	}

	bool hookResult;
	hookResult = ModManager::TryHookFunction(
		"InGameUIUpdateStaticPoolCaller",
		reinterpret_cast<void*>(&UIManager::InGameUIUpdateStaticPoolCallerHook)
	);
	Logging::Write(logPrefix, 
		"InGameUIUpdateStaticPoolCaller function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"AccessStaticUIPool",
		reinterpret_cast<void*>(&UIManager::AccessStaticUIPoolHook)
	);
	Logging::Write(logPrefix, 
		"AccessStaticUIPool function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"InGameUIDrawElement",
		reinterpret_cast<void*>(&UIManager::InGameUIDrawElementHook)
	);
	Logging::Write(logPrefix, 
		"InGameUIDrawElement function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"InGameUIUpdateElement",
		reinterpret_cast<void*>(&UIManager::InGameUIUpdateElementHook)
	);
	Logging::Write(logPrefix, 
		"InGameUIUpdateElement function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"PostMenuExit",
		reinterpret_cast<void*>(&UIManager::PostMenuExitHook)
	);
	Logging::Write(logPrefix, 
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
					if (ModManager* instance = ModManager::GetInstance())
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

void UIManager::OnTextLanguageChange()
{
	for (auto it = musicPlayerActionButtonMap.begin(); it != musicPlayerActionButtonMap.end(); it++)
	{
		UIButton& button = it.value();
		for (auto& buttonState : button.states)
		{
			buttonState.UpdateCachedText();
		}
	}
}

void UIManager::UpdateMusicPlayerUIBlockers(MusicPlayerUIBlocker blocker, bool enable)
{
	uint8_t previousBlockers = musicPlayerUIBlockers;
	if (enable)
	{
		musicPlayerUIBlockers |= blocker;
	}
	else
	{
		musicPlayerUIBlockers &= ~blocker;
	}
	/*Logging::Write(logPrefix, 
		"Music player UI blockers updated: 0x%02X -> 0x%02X (blocker: 0x%02X, enable: %s)",
		previousBlockers, musicPlayerUIBlockers, blocker, enable ? "true" : "false"
	);*/

	for (auto it = musicPlayerActionButtonMap.begin(); it != musicPlayerActionButtonMap.end(); it++)
	{
		UIButton& button = it.value();
		button.Toggle(musicPlayerUIBlockers == MusicPlayerUIBlocker::NONE);
	}

	switch (blocker)
	{
		case MusicPlayerUIBlocker::BT_BLOCK:
		case MusicPlayerUIBlocker::MULE_BLOCK:
		{
			if (musicPlayerUIBlockers > 0 && previousBlockers == 0)
			{
				ShowNotificationText(
					LanguageManager::GetLocalizedText("Threat nearby: music player deactivated.").c_str()
				);
			}
			else if (musicPlayerUIBlockers == 0 && previousBlockers > 0)
			{
				ShowNotificationText(
					LanguageManager::GetLocalizedText("Threat cleared: music player activated.").c_str()
				);
			}
			break;
		}
		case MusicPlayerUIBlocker::CHIRAL_BLOCK:
		{
			if (musicPlayerUIBlockers > 0 && previousBlockers == 0)
			{
				ShowNotificationText(
					LanguageManager::GetLocalizedText("Chiral network off: music player deactivated.").c_str()
				);
			}
			else if (musicPlayerUIBlockers == 0 && previousBlockers > 0)
			{
				ShowNotificationText(
					LanguageManager::GetLocalizedText("Chiral network on: music player activated.").c_str()
				);
			}
			break;
		}
		case MusicPlayerUIBlocker::FACILITY_BLOCK:
		{
			if (musicPlayerUIBlockers > 0 && previousBlockers == 0)
			{
				ShowNotificationText(
					LanguageManager::GetLocalizedText("Entering facility: music player deactivated.").c_str()
				);
			}
			else if (musicPlayerUIBlockers == 0 && previousBlockers > 0)
			{
				ShowNotificationText(
					LanguageManager::GetLocalizedText("Exiting facility: music player activated.").c_str()
				);
			}
			break;
		}
	}
}

void UIManager::InGameUIUpdateStaticPoolCallerHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "UI Manager";
	const FunctionData* InGameUIUpdateStaticPoolCallerFuncData =
		ModManager::GetFunctionData("InGameUIUpdateStaticPoolCaller");
	if (!InGameUIUpdateStaticPoolCallerFuncData
		|| !InGameUIUpdateStaticPoolCallerFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original InGameUIUpdateStaticPoolCaller function is not found, cannot call it.");
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
			//Logging::Write(logPrefix, "Runtime slot index for button %s: %u", button.name, runtimeSlotIndex);

			if (currentRuntimeUIPoolStart && runtimeSlotSize)
			{
				uintptr_t runtimeUITextPoolOffset = GetRuntimeUITextPoolOffset();
				void** destinationRuntimeTextPtr = (void**)(
					currentRuntimeUIPoolStart + runtimeUITextPoolOffset + runtimeSlotIndex * runtimeSlotSize
				);
				RuntimeUIText* currentRuntimeText = *(RuntimeUIText**)destinationRuntimeTextPtr;

				bool textNeedsUpdate = true;
				if (currentRuntimeText && currentRuntimeText->data)
				{
					std::wstring_view currentView(currentRuntimeText->data);
					const std::wstring_view& expectedView = buttonState.cachedWideView;
					textNeedsUpdate = currentView != expectedView;

					/*Logging::Write(logPrefix, "====================================");
					Logging::Write(logPrefix, "Text: %s", buttonState.GetLocalizedText());
					Logging::Write(logPrefix, "Current View:");
					for (wchar_t wc : currentView)
					{
						Logging::Write(logPrefix, "0x%04X", wc);
					}

					Logging::Write(logPrefix, "Expected View:");
					for (wchar_t wc : expectedView)
					{
						Logging::Write(logPrefix, "0x%04X", wc);
					}

					Logging::Write(logPrefix, "Text needs update: %s", textNeedsUpdate ? "true" : "false");*/
				}

				if (textNeedsUpdate)
				{
					const FunctionData* createRuntimeUITextFromStringFuncData =
						ModManager::GetFunctionData("CreateRuntimeUITextFromString");
					if (!createRuntimeUITextFromStringFuncData
						|| !createRuntimeUITextFromStringFuncData->address)
					{
						Logging::Write(logPrefix, 
							"CreateRuntimeUITextFromString function was not found, cannot update text."
						);
						continue;
					}

					const FunctionData* assignRuntimeUITextFuncData =
						ModManager::GetFunctionData("AssignRuntimeUIText");
					if (!assignRuntimeUITextFuncData || !assignRuntimeUITextFuncData->address)
					{
						Logging::Write(logPrefix, 
							"AssignRuntimeUIText function was not found, cannot assign new text."
						);
						continue;
					}

					const FunctionData* updateRuntimeUITextFuncData =
						ModManager::GetFunctionData("UpdateRuntimeUIText");
					if (!updateRuntimeUITextFuncData || !updateRuntimeUITextFuncData->address)
					{
						Logging::Write(logPrefix, 
							"UpdateRuntimeUIText function was not found, cannot update runtime text."
						);
						continue;
					}

					const std::string& localizedText = buttonState.cachedACPText;
					const std::string currentText = RuntimeUITextToUtf8(currentRuntimeText);
					const std::string newText = Utils::GameTextToUtf8(localizedText);
					Logging::Write(logPrefix, 
						"Updating runtime text for button %s, slot index: %zu, current text: %s, new text: %s",
						button.name, runtimeSlotIndex, currentText.c_str(), newText.c_str()
					);

					void* newRuntimeText = nullptr;
					GenericFunction_t createRuntimeUITextFromStringFunc =
						reinterpret_cast<GenericFunction_t>(createRuntimeUITextFromStringFuncData->address);
					createRuntimeUITextFromStringFunc(
						&newRuntimeText,
						(void*)localizedText.c_str(),
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
						reinterpret_cast<GenericFunction_t>(updateRuntimeUITextFuncData->address);
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
						Logging::Write(logPrefix, 
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
	constexpr const char* logPrefix = "Access Static UI Pool Hook";
	uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(arg1);
	uintptr_t poolRootAddress = *(uintptr_t*)(ownerAddress + staticUIPoolOwnerOffset);
	staticUIPoolAddress = poolRootAddress + GetStaticUIPoolOffset();

	const FunctionData* functionData = ModManager::GetFunctionData("AccessStaticUIPool");
	if (!functionData || !functionData->originalFunction)
	{
		Logging::Write(logPrefix, "Original AccessStaticUIPool function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(functionData->originalFunction)(arg1, arg2, arg3, arg4);
}

void UIManager::InGameUIUpdateElementHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "UI Manager";

	// arg1 is runtime pool start address + 0xC8 + slotSize * slotIndex
	if (currentRuntimeUIPoolStart)
	{
		if (!runtimeSlotSize && (uintptr_t)arg1 - currentRuntimeUIPoolStart > 0xC8)
		{
			runtimeSlotSize = (uintptr_t)arg1 - currentRuntimeUIPoolStart - 0xC8;
			Logging::Write(logPrefix, "Runtime UI slot size: %zu", runtimeSlotSize);
		}
	}

	const FunctionData* inGameUIUpdateElementFuncData =
		ModManager::GetFunctionData("InGameUIUpdateElement");
	if (!inGameUIUpdateElementFuncData || !inGameUIUpdateElementFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original InGameUIUpdateElement function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(inGameUIUpdateElementFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
}

void UIManager::InGameUIDrawElementHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "UI Manager";
	currentRuntimeUIPoolStart = reinterpret_cast<uintptr_t>(arg1);

	const FunctionData* inGameUIDrawElementFuncData =
		ModManager::GetFunctionData("InGameUIDrawElement");
	if (!inGameUIDrawElementFuncData || !inGameUIDrawElementFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original InGameUIDrawElement function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(inGameUIDrawElementFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);

	//size_t runtimeSlotIndex = reinterpret_cast<size_t>(arg2);
	//if (runtimeSlotIndex == 0) // New UI group so we reset the compass state
	//{
	//	ResetModCompassState();
	//}

	if (currentRuntimeUIPoolStart) {
		uintptr_t compassFlagOffset = GetRuntimeCompassFlagOffset();
		uint8_t* runtimeCompassOpenFlagAddress1 = (uint8_t*)(
			currentRuntimeUIPoolStart + compassFlagOffset
		);
		uint8_t* runtimeCompassOpenFlagAddress2 = (uint8_t*)(
			currentRuntimeUIPoolStart + compassFlagOffset + 1
		);
		if (*runtimeCompassOpenFlagAddress1 == 1 && *runtimeCompassOpenFlagAddress2 == 1)
		{
			Logging::Write(logPrefix,
				"Compass UI detected as OPEN based on runtime flags %p and %p",
				(void*)runtimeCompassOpenFlagAddress1, (void*)runtimeCompassOpenFlagAddress2
			);
			currentCompassState = OPEN;
		}
		/*else
		{
			currentCompassState = CLOSED;
		}*/
	}
}

void UIManager::PostMenuExitHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "UI Manager";

	if (currentCompassState == OPEN)
	{
		Logging::Write(logPrefix, "Resetting compass state to CLOSED on menu exit.");
	}
	ResetModCompassState();

	const FunctionData* postMenuExitFuncData = ModManager::GetFunctionData("PostMenuExit");
	if (!postMenuExitFuncData || !postMenuExitFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original PostMenuExit function was not hooked, cannot call it.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(postMenuExitFuncData->originalFunction)(
		arg1, arg2, arg3, arg4
	);
}

void UIManager::ShowNotificationText(const char* text)
{
	constexpr const char* logPrefix = "Show Notification Text";

	const FunctionData* createRuntimeUITextFromStringFuncData =
		ModManager::GetFunctionData("CreateRuntimeUITextFromString");
	if (!createRuntimeUITextFromStringFuncData || !createRuntimeUITextFromStringFuncData->address)
	{
		Logging::Write(logPrefix, "CreateRuntimeUITextFromString function was not found, cannot show notification text.");
		return;
	}

	const FunctionData* getNotificationPoolFuncData =
		ModManager::GetFunctionData("GetNotificationPool");
	if (!getNotificationPoolFuncData || !getNotificationPoolFuncData->address)
	{
		Logging::Write(logPrefix, "GetNotificationPool function was not found, cannot show notification text.");
		return;
	}

	const FunctionData* drawNotificationTextFuncData =
		ModManager::GetFunctionData("DrawNotificationText");
	if (!drawNotificationTextFuncData || !drawNotificationTextFuncData->address)
	{
		Logging::Write(logPrefix, "DrawNotificationText function was not found, cannot show notification text.");
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
		Logging::Write(logPrefix, 
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

void UIManager::ResetModCompassState()
{
	currentCompassState = CLOSED; // reset compass state on menu exit
}

std::string UIManager::RuntimeUITextToUtf8(const RuntimeUIText* text)
{
	if (!text)
	{
		return "<null>";
	}
	if (!MemoryUtils::IsReadablePointer((void*)text, 0x10))
	{
		return "<unreadable>";
	}
	if (text->length <= 0 || text->length > 4096)
	{
		return "<invalid length>";
	}

	size_t length = static_cast<size_t>(text->length);
	if (!MemoryUtils::IsReadablePointer((void*)text->data, length * sizeof(wchar_t)))
	{
		return "<unreadable data>";
	}

	if (length > 0 && text->data[length - 1] == L'\0')
	{
		length--;
	}

	return Utils::WstringViewToUtf8(std::wstring_view(text->data, length));
}
