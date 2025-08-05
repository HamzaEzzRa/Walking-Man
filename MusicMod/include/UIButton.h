#pragma once

#include <vector>

#include "InputCode.h"
#include "LanguageManager.h"

#include "Utils.h"

enum class UIButtonAction
{
	TOGGLE_MUSIC,
	SHUFFLE_PLAYLIST,
	TOGGLE_LOOP_MODE
};

struct UIButtonState
{
	uint8_t id;
	const char* text;

	// Cached wide string + view (after ACPWide conversion)
	std::wstring cachedWideString;
	std::wstring_view cachedWideView;
	std::string cachedACPText;

	const std::string& GetLocalizedText() const
	{
		return LanguageManager::GetLocalizedText(text);
	}

	void UpdateCachedText()
	{
		const std::string& localizedText = GetLocalizedText();
		cachedWideString = Utils::DecodeGameText(localizedText);
		if (!cachedWideString.empty() && cachedWideString.back() == L'\0')
		{
			cachedWideString.pop_back();
		}
		cachedWideView = std::wstring_view(cachedWideString);
		cachedACPText = localizedText;
	}
};

struct UIButton
{
	const char* name; // Name of the button, used as key in the iconSlotIndexMap
	const UIButtonAction action; // Action associated with the button
	std::vector<UIButtonState> states;
	const std::vector<InputCode> inputCodes;

	bool enabled = true;
	bool requiresUpdate = false;
	size_t currentStateIndex = 0;

	const UIButtonState& GetCurrentState() const
	{
		return states[currentStateIndex];
	}

	void OnPress(bool blink)
	{
		currentStateIndex = (currentStateIndex + 1) % states.size();
		requiresUpdate = blink; // will probably use this later to manage button interactions (press, hold, etc.)
	}

	void Toggle(bool value)
	{
		enabled = value;
	}
};