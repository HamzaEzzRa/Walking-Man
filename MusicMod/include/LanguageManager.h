#pragma once

#include <string>

#include "FunctionHook.h"
#include "IEventListener.h"
#include "MemoryWatcher.h"

#include "GameData.h"

#include "Logger.h"

class LanguageManager : public IEventListener, public FunctionHook
{
public:
	void OnEvent(const ModEvent&) override;

	static const std::string& GetLocalizedText(const std::string&);

private:
	void OnScanDone();
	void OnRender();
	void OnPreExit();

	static void ReloadCache();

	static void AccessLanguagePoolHook(void*, void*, void*, void*);

private:
	Logger logger{ "Language Manager" };

	inline static uintptr_t textLanguageIdStandardOffset = 0x80;
	inline static uintptr_t textLanguageIdDCOffset = 0xB0;
	inline static TextLanguage previousTextLanguageId = TextLanguage::UNKNOWN;
	inline static TextLanguage currentTextLanguageId = TextLanguage::UNKNOWN;
	inline static std::unique_ptr<MemoryWatcher> textLanguageIdWatcher = nullptr;
	inline static uint32_t textLanguageIdPollingInterval = 100; // ms

	inline static std::unordered_map<std::string, std::string> localizedTextCache;
};