#include "LanguageManager.h"

#include "ModManager.h"

#include "Utils.h"

void LanguageManager::OnEvent(const ModEvent& event)
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
		case ModEventType::PreExitTriggered:
		{
			OnPreExit();
			break;
		}
		default:
			break;
	}
}

void LanguageManager::OnScanDone()
{
	bool hookResult = ModManager::TryHookFunction(
		"AccessLanguagePool",
		reinterpret_cast<void*>(&LanguageManager::AccessLanguagePoolHook)
	);
	logger.Log(
		"AccessLanguagePool function hook %s",
		hookResult ? "installed successfully" : "failed"
	);
}

void LanguageManager::OnRender()
{
	if (currentTextLanguageId != previousTextLanguageId)
	{
		logger.Log("Current language ID changed to: %u", currentTextLanguageId);
		ReloadCache();

		ModManager* instance = ModManager::GetInstance();
		if (instance)
		{
			instance->DispatchEvent(ModEvent{
				ModEventType::TextLanguageChanged,
				this,
				&currentTextLanguageId
			});
		}

		previousTextLanguageId = currentTextLanguageId;
	}
}

void LanguageManager::OnPreExit()
{
	if (textLanguageIdWatcher)
	{
		textLanguageIdWatcher->Uninstall();
		textLanguageIdWatcher.reset();
	}
}

const std::string& LanguageManager::GetLocalizedText(const std::string& key)
{
	auto it = localizedTextCache.find(key);
	if (it != localizedTextCache.end())
	{
		return it->second;
	}
	return key; // Return the key itself if not found (default english text)
}

void LanguageManager::ReloadCache()
{
	localizedTextCache.clear();
	for (const auto& [key, localized] : ModConfiguration::Databases::localizedTextDatabase)
	{ 
		const auto& localizedText = localized.GetText(currentTextLanguageId);
		if (localizedText[0] == '\0')
		{
			localizedTextCache[key] = key;
		}
		else
		{
			localizedTextCache[key] = Utils::EncodeGameText(localizedText);
		}
	}
}

void LanguageManager::AccessLanguagePoolHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	Logger logger("Access Language Pool Hook");
	const FunctionData* funcData = ModManager::GetFunctionData("AccessLanguagePool");
	if (!funcData || !funcData->originalFunction)
	{
		logger.Log("Original AccessLanguagePool function was not hooked, cannot call it");
		return;
	}
	reinterpret_cast<GenericFunction_t>(funcData->originalFunction)(arg1, arg2, arg3, arg4);

	uintptr_t textLanguageIdAddress = reinterpret_cast<uintptr_t>(arg1) + 
		((ModConfiguration::gameVersion == GameVersion::DC) ? textLanguageIdDCOffset : textLanguageIdStandardOffset);
	currentTextLanguageId = *reinterpret_cast<const TextLanguage*>(textLanguageIdAddress);

	auto& currentTextLanguageIdRef = currentTextLanguageId;
	textLanguageIdWatcher = std::make_unique<MemoryWatcher>(
		[&currentTextLanguageIdRef](const void* newValue)
		{
			currentTextLanguageIdRef = *reinterpret_cast<const TextLanguage*>(newValue);
		},
		textLanguageIdPollingInterval
	);
	textLanguageIdWatcher->Install(
		(void*)textLanguageIdAddress,
		sizeof(uint8_t)
	);

	bool unhookResult = ModManager::TryUnhookFunction(*funcData);
	logger.Log(
		"AccessLanguagePool function unhook %s",
		unhookResult ? "successful" : "failed"
	);
}