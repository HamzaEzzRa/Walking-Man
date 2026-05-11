#include "MusicPlayer.h"

#include "AreaMusicManager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <Windows.h>

#include "CustomMediaLoader.h"
#include "GameStateManager.h"
#include "ModConfiguration.h"
#include "ModManager.h"
#include "PlaybackQueue.h"

#include "MemoryUtils.h"
#include "MinHook.h"

#include "PatternScanner.h"

#include "UIButton.h"

#include "Utils.h"

namespace
{
	constexpr uint32_t NativeAreaMusicPauseEventId = 183800411;
	constexpr uint32_t NativeAreaMusicResumeEventId = 4253292098;
	constexpr ptrdiff_t NativeAreaMusicPauseCounterOffset = 0xf8;
	constexpr const char* PostEventExportName =
		"?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z";
	constexpr const char* NativeAreaMusicPauseBodyPattern =
		"B8 01 00 00 00 F0 0F C1 ?? F8 00 00 00 33 ?? 45 33 C9 "
		"?? ?? 24 38 45 33 C0 48 89 ?? 24 30 33 D2 ?? ?? 24 28 "
		"48 89 ?? 24 20 B9 5B 92 F4 0A";
	constexpr const char* NativeAreaMusicResumeBodyPattern =
		"B8 FF FF FF FF F0 0F C1 ?? F8 00 00 00 ?? ?? F8 00 00 "
		"00 00 7F 4A";
	constexpr const char* NativeAreaMusicPauseBodyPatternGP =
		"F0 FF 83 F8 00 00 00 33 C0 45 33 C9 89 44 24 38 45 33 "
		"C0 48 89 44 24 30 33 D2 89 44 24 28 B9 5B 92 F4 0A";
	constexpr const char* NativeAreaMusicResumeBodyPatternGP =
		"F0 FF 89 F8 00 00 00 83 B9 F8 00 00 00 00 0F 8F";
	constexpr const char* NativeAreaMusicResumeCallsitePattern =
		"80 ?? 58 00 74 ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 ?? 58 00";

	using AkPostEventFn = unsigned long(__cdecl*)(
		uint32_t,
		uint64_t,
		uint32_t,
		void*,
		void*,
		uint32_t,
		void*,
		uint32_t
	);
	using NativeAreaMusicFunc = void (*)(void*);

	struct NativeAreaMusicControls
	{
		uintptr_t pauseFunction = 0;
		uintptr_t resumeFunction = 0;
		uintptr_t contextSlot = 0;
		bool resolved = false;
	};

	AkPostEventFn ResolvePostEvent()
	{
		static AkPostEventFn postEvent = nullptr;
		static bool resolved = false;
		if (resolved)
		{
			return postEvent;
		}

		resolved = true;
		HMODULE mainModule = GetModuleHandle(nullptr);
		if (!mainModule)
		{
			return nullptr;
		}

		postEvent = reinterpret_cast<AkPostEventFn>(
			GetProcAddress(mainModule, PostEventExportName)
		);
		return postEvent;
	}

	bool GetMainModuleRange(uintptr_t* start, uintptr_t* end)
	{
		HMODULE mainModule = GetModuleHandle(nullptr);
		if (!mainModule)
		{
			return false;
		}

		auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(mainModule);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return false;
		}

		auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(
			reinterpret_cast<uint8_t*>(mainModule) + dosHeader->e_lfanew
		);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
		{
			return false;
		}

		*start = reinterpret_cast<uintptr_t>(mainModule);
		*end = *start + ntHeaders->OptionalHeader.SizeOfImage;
		return true;
	}

	std::vector<uintptr_t> ScanMainModule(const char* pattern)
	{
		uintptr_t moduleStart = 0;
		uintptr_t moduleEnd = 0;
		if (!GetMainModuleRange(&moduleStart, &moduleEnd))
		{
			return {};
		}

		return PatternScanner::ScanAll(
			pattern,
			PAGE_EXECUTE_READ,
			std::chrono::milliseconds(0),
			0,
			false,
			moduleStart,
			moduleEnd
		);
	}

	uintptr_t FindFunctionStart(uintptr_t bodyAddress)
	{
		constexpr uintptr_t maxFunctionPrefixSize = 0x140;
		uintptr_t scanStart = bodyAddress > maxFunctionPrefixSize
			? bodyAddress - maxFunctionPrefixSize
			: 0;

		for (uintptr_t address = bodyAddress; address > scanStart; --address)
		{
			auto* current = reinterpret_cast<uint8_t*>(address);
			auto* previous = reinterpret_cast<uint8_t*>(address - 1);
			if (*previous == 0xcc && *current != 0xcc)
			{
				return address;
			}
		}

		return 0;
	}

	uintptr_t ResolveFunctionFromBodyPattern(const char* pattern)
	{
		std::vector<uintptr_t> matches = ScanMainModule(pattern);
		if (matches.empty())
		{
			return 0;
		}

		uintptr_t bodyAddress = *std::min_element(matches.begin(), matches.end());
		return FindFunctionStart(bodyAddress);
	}

	uintptr_t ResolveNativeAreaMusicContextSlot(uintptr_t resumeFunction)
	{
		std::vector<uintptr_t> matches = ScanMainModule(NativeAreaMusicResumeCallsitePattern);
		for (uintptr_t callsite : matches)
		{
			int32_t callDisplacement = *reinterpret_cast<int32_t*>(callsite + 14);
			uintptr_t callTarget = callsite + 18 + callDisplacement;
			if (callTarget != resumeFunction)
			{
				continue;
			}

			int32_t loadDisplacement = *reinterpret_cast<int32_t*>(callsite + 9);
			return callsite + 13 + loadDisplacement;
		}

		return 0;
	}

	NativeAreaMusicControls& ResolveNativeAreaMusicControls()
	{
		static NativeAreaMusicControls controls{};
		if (controls.resolved)
		{
			return controls;
		}

		controls.resolved = true;
		controls.pauseFunction = ResolveFunctionFromBodyPattern(NativeAreaMusicPauseBodyPattern);
		controls.resumeFunction = ResolveFunctionFromBodyPattern(NativeAreaMusicResumeBodyPattern);
		if (!controls.pauseFunction || !controls.resumeFunction)
		{
			controls.pauseFunction = ResolveFunctionFromBodyPattern(NativeAreaMusicPauseBodyPatternGP);
			controls.resumeFunction = ResolveFunctionFromBodyPattern(NativeAreaMusicResumeBodyPatternGP);
		}

		if (controls.resumeFunction)
		{
			controls.contextSlot = ResolveNativeAreaMusicContextSlot(controls.resumeFunction);
		}

		Logging::Write("Music Player",
			"Native area music controls resolved: pause=%p resume=%p context slot=%p",
			reinterpret_cast<void*>(controls.pauseFunction),
			reinterpret_cast<void*>(controls.resumeFunction),
			reinterpret_cast<void*>(controls.contextSlot)
		);
		return controls;
	}

	uintptr_t ReadNativeAreaMusicContext(uintptr_t contextSlot)
	{
		if (
			!contextSlot
			|| !MemoryUtils::IsReadablePointer(
				reinterpret_cast<void*>(contextSlot),
				sizeof(uintptr_t)
			)
		)
		{
			return 0;
		}

		uintptr_t context = *reinterpret_cast<uintptr_t*>(contextSlot);
		if (
			!context
			|| !MemoryUtils::IsReadablePointer(
				reinterpret_cast<void*>(context + NativeAreaMusicPauseCounterOffset),
				sizeof(long)
			)
		)
		{
			return 0;
		}

		return context;
	}

	bool PostNativeAreaMusicEvent(bool paused)
	{
		AkPostEventFn postEvent = ResolvePostEvent();
		if (!postEvent)
		{
			return false;
		}

		const uint32_t eventId = paused
			? NativeAreaMusicPauseEventId
			: NativeAreaMusicResumeEventId;
		postEvent(
			eventId,
			0,
			0,
			nullptr,
			nullptr,
			0,
			nullptr,
			0
		);
		return true;
	}
}

MusicPlayer::MusicPlayer()
{
}

void MusicPlayer::CancelPendingAreaMusicTransition(const char* reason)
{
	if (!pendingMusicData)
	{
		return;
	}

	constexpr const char* logPrefix = "Music Player";
	Logging::Write(logPrefix,
		"Canceling queued area music track \"%s\"%s%s",
		pendingMusicData->name ? pendingMusicData->name : "",
		reason && reason[0] ? ": " : "",
		reason && reason[0] ? reason : ""
	);
	pendingMusicData = nullptr;
	pendingMusicDisplayDescription = true;
	pendingMusicOverridePrepared = false;
	currentMusicPlayTime.store(0);
	currentMusicMaxLength.store(0);
	AreaMusicManager::Unset();
}

void MusicPlayer::OnEvent(const ModEvent& event)
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
	case ModEventType::UIButtonPressed:
	{
		UIButtonAction action = std::any_cast<UIButtonAction>(event.data);
		OnUIButtonAction(action);
		break;
	}
	case ModEventType::InputPressResolved:
	{
		const InputCode& inputCode = std::any_cast<InputCode>(event.data);
		OnInputPress(inputCode);
		break;
	}
	case ModEventType::BTTerritoryStateChanged:
	case ModEventType::MuleTerritoryStateChanged:
	{
		auto* territoryFlagState = std::any_cast<FlagState<EnemyTerritoryFlag>*>(event.data);
		const bool wasBlocked = HasActiveMusicBlocker();
		std::atomic<bool>& territoryBlocker = event.type == ModEventType::BTTerritoryStateChanged
			? btTerritoryBlocksMusic
			: muleTerritoryBlocksMusic;
		territoryBlocker.store(territoryFlagState->current != EnemyTerritoryFlag::SAFE);
		const bool isBlocked = HasActiveMusicBlocker();

		if (!wasBlocked && isBlocked && (currentMusicData || pendingMusicData))
		{
			if (!PauseCurrentMusicForBlocker("enemy territory"))
			{
				CancelPendingAreaMusicTransition("enemy territory interruption");
				if (AreaMusicManager::UsesOverride(currentMusicData))
				{
					AreaMusicManager::Unset();
				}
				currentMusicData = nullptr;
				currentMusicIsPlaying.store(false);
				currentMusicPausedByBlocker.store(false);
				currentMusicPlayTime.store(0);
				currentMusicMaxLength.store(0);

				ModManager* instance = ModManager::GetInstance();
				if (instance)
				{
					instance->DispatchEvent(
						ModEvent{ ModEventType::MusicPlayerStopped, this, nullptr }
					);
				}
			}
		}
		else if (wasBlocked && !isBlocked)
		{
			ResumeCurrentMusicForBlocker("enemy territory cleared");
		}
		break;
	}
	case ModEventType::ChiralNetworkStateChanged:
	{
		if (ModConfiguration::connectToChiralNetwork)
		{
			auto* chiralNetworkFlagState = std::any_cast<FlagState<ChiralNetworkFlag>*>(event.data);
			const bool wasBlocked = HasActiveMusicBlocker();
			chiralNetworkBlocksMusic.store(chiralNetworkFlagState->current == ChiralNetworkFlag::OFF);
			const bool isBlocked = HasActiveMusicBlocker();

			if (!wasBlocked && isBlocked && (currentMusicData || pendingMusicData))
			{
				if (!PauseCurrentMusicForBlocker("chiral network off"))
				{
					StopMusic();

					ModManager* instance = ModManager::GetInstance();
					if (instance)
					{
						instance->DispatchEvent(
							ModEvent{ ModEventType::MusicPlayerStopped, this, nullptr }
						);
					}
				}
			}
			else if (wasBlocked && !isBlocked)
			{
				ResumeCurrentMusicForBlocker("chiral network restored");
			}
		}
		break;
	}
	default:
		break;
	}
}

void MusicPlayer::OnScanDone()
{
	bool hookResult = ModManager::TryHookFunction(
		"PlayMusic",
		reinterpret_cast<void*>(&MusicPlayer::PlayMusicHook)
	);
	Logging::Write(logPrefix,
		"PlayMusic function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	const FunctionData* playingLoopData = ModManager::GetFunctionData("PlayingLoop");
	if (!playingLoopData || !playingLoopData->address)
	{
		Logging::Write(logPrefix, "Failed to find PlayingLoop function signature");
	}
	else
	{
		playingLoopAddress = playingLoopData->address;
	}
	ResolveNativeAreaMusicControls();

	hookResult = ModManager::TryHookFunction(
		"PlayUISound",
		reinterpret_cast<void*>(&MusicPlayer::PlayUISoundHook)
	);
	Logging::Write(logPrefix,
		"PlayUISound function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	hookResult = ModManager::TryHookFunction(
		"ShowMusicDescriptionCore",
		reinterpret_cast<void*>(&MusicPlayer::ShowMusicDescriptionCoreHook)
	);
	Logging::Write(logPrefix,
		"ShowMusicDescriptionCore function hook %s",
		hookResult ? "installed successfully" : "failed"
	);

	auto baseAreaTrackIt = ModConfiguration::Databases::songDatabase.find(AreaMusicManager::TargetTrackName);
	if (baseAreaTrackIt != ModConfiguration::Databases::songDatabase.end())
	{
		CustomMediaLoader::BindCustomSongsToAreaTrack(baseAreaTrackIt->second);
	}
	else
	{
		Logging::Write(logPrefix,
			"Base area track \"%s\" not found in song database; custom area tracks will not be bound",
			AreaMusicManager::TargetTrackName
		);
	}

	std::vector<const MusicData*> songMusicDataList;
	for (const std::string& name : ModConfiguration::activePlaylist)
	{
		auto it = ModConfiguration::Databases::songDatabase.find(name);
		if (it == ModConfiguration::Databases::songDatabase.end())
		{
			auto customIt = ModConfiguration::Databases::customSongDatabase.find(name);
			if (customIt == ModConfiguration::Databases::customSongDatabase.end())
			{
				Logging::Write(logPrefix, "Song \"%s\" not found in database, skipping...", name.c_str());
				continue;
			}

			auto& customMusicData = customIt->second;
			if (!customMusicData.address)
			{
				Logging::Write(logPrefix, "Custom song \"%s\" has no bound area address, skipping...", name.c_str());
				continue;
			}
			songMusicDataList.push_back(&customMusicData);
			continue;
		}

		auto& songMusicData = it->second;
		if (!songMusicData.address)
		{
			Logging::Write(logPrefix, "Song \"%s\" has no valid address, skipping...", name.c_str());
			continue;
		}
		songMusicDataList.push_back(&songMusicData);
	}
	songQueue.SetData(songMusicDataList.data(), songMusicDataList.size());
	songQueue.Reset();

	if (ModConfiguration::devMode) // Only create pool queue in dev mode
	{
		std::vector<const MusicData*> poolMusicDataList;
		auto it = ModConfiguration::Databases::songDatabase.find("Music Pool Start");
		if (it != ModConfiguration::Databases::songDatabase.end())
		{
			auto& poolMusicData = it->second;
			if (poolMusicData.address)
			{
				poolMusicDataList.push_back(&poolMusicData);
				Logging::Write(logPrefix, "Pushed pool music %s with address %p",
					poolMusicData.name, poolMusicData.address
				);

				uintptr_t currentAddress = poolMusicData.address;
				// Take the first 8 bytes and compare with each new address start to check if we're still within the pool
				uint64_t poolMusicSignature = *reinterpret_cast<uint64_t*>(poolMusicData.address);
				size_t index = 0;

				std::vector<size_t> skipIndices{};

				while (true)
				{
					currentAddress = currentAddress + poolMusicDataSize;
					uint64_t signature = *reinterpret_cast<uint64_t*>(currentAddress);
					if (signature != poolMusicSignature)
					{
						Logging::Write(logPrefix, "Pool music signature mismatch at address %p, pool ended", (void*)currentAddress);
						break;
					}

					std::string poolMusicName = "Pool Music " + std::to_string(++index);
					for (size_t skipIndex : skipIndices)
					{
						if (index == skipIndex + 1)
						{
							continue;
						}
					}

					MusicData* newData = (MusicData*)malloc(sizeof(MusicData));
					if (!newData)
					{
						Logging::Write(logPrefix, "Failed to allocate memory for new pool music data");
						continue;
					}
					newData->descriptionID = 0;
					newData->type = MusicType::UNKNOWN;
					newData->maxLength = 0;
					newData->name = _strdup(poolMusicName.c_str());
					newData->artist = "";
					newData->signature = "";
					newData->address = currentAddress;
					newData->active = true;

					poolMusicDataList.push_back(newData);

					Logging::Write(logPrefix, "Pushed pool music %s with address %p",
						newData->name, newData->address
					);
				}
			}
		}
		poolQueue.SetData(poolMusicDataList.data(), poolMusicDataList.size());
		poolQueue.Reset();
	}
}

void MusicPlayer::OnRender()
{
	if (
		pendingMusicData
		&& std::chrono::steady_clock::now() >= pendingMusicStartTime
	)
	{
		const MusicData* data = pendingMusicData;
		const bool displayDescription = pendingMusicDisplayDescription;

		if (!pendingMusicOverridePrepared && AreaMusicManager::UsesOverride(data))
		{
			if (!AreaMusicManager::Register(data))
			{
				Logging::Write(logPrefix,
					"Area music override was not prepared for queued track \"%s\"; canceling playback",
					data->name ? data->name : ""
				);
				pendingMusicData = nullptr;
				pendingMusicDisplayDescription = true;
				pendingMusicOverridePrepared = false;
				AreaMusicManager::Unset();
				currentMusicData = nullptr;
				currentMusicIsPlaying.store(false);
				currentMusicPlayTime.store(0);
				currentMusicMaxLength.store(0);
				currentMusicPausedByBlocker.store(false);
				return;
			}

			pendingMusicOverridePrepared = true;
			pendingMusicStartTime = std::chrono::steady_clock::now();
			Logging::Write(logPrefix, "Prepared queued area music override for \"%s\"", data->name ? data->name : "");
			return;
		}

		pendingMusicData = nullptr;
		pendingMusicDisplayDescription = true;
		pendingMusicOverridePrepared = false;
		Logging::Write(logPrefix, "Starting queued area music track: %s", data->name);
		PlayMusic(data, displayDescription);
	}

	// Handle Playback
	if (currentMusicData && !currentMusicPausedByBlocker.load())
	{
		if (currentMusicIsPlaying.load())
		{
			const std::chrono::milliseconds songDuration =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - currentMusicStartTime
				);
			currentMusicPlayTime.store(songDuration.count());
		}

		const long long maxLength = currentMusicMaxLength.load();
		bool maxLengthReached = false;
		if (
			maxLength > 0
			&& maxLength < currentMusicPlayTime.load()
		)
		{
			Logging::Write(logPrefix, "Current music playback time exceeded its max duration");
			currentMusicPlayTime.store(0);
			currentMusicIsPlaying.store(false);
			maxLengthReached = true;
		}

		if (!currentMusicIsPlaying.load())
		{
			if (gameCalledInterruptor || (gameCalledSong && ModConfiguration::allowScriptedSongs))
			{
				gameCalledSong = false;
				gameCalledInterruptor = false;

				if (AreaMusicManager::UsesOverride(currentMusicData))
				{
					AreaMusicManager::Unset();
				}
				currentMusicData = nullptr;
				ModManager* instance = ModManager::GetInstance();
				if (instance)
				{
					const char* interruptionMessage = "Music player interrupted.";
					instance->DispatchEvent(ModEvent{
						ModEventType::MusicPlayerInterrupted,
						this,
						&interruptionMessage
					});
				}
			}
			else
			{
				switch (loopMode)
				{
					case LoopMode::ALL:
					{
						Logging::Write(logPrefix, "Autoplaying next song...");
						PlayNextSong();
						break;
					}
					case LoopMode::ONE:
					{
						Logging::Write(logPrefix, "Autoplaying current song...");
						PlayCurrentSong();
						break;
					}
					case LoopMode::NONE:
					{
						if (maxLengthReached)
						{
							StopMusic();
						}
						else
						{
							currentMusicData = nullptr;
						}

						if (musicAddressWatcher)
						{
							musicAddressWatcher->Uninstall();
							musicAddressWatcher.reset();
						}

						ModManager* instance = ModManager::GetInstance();
						if (instance)
						{
							instance->DispatchEvent(ModEvent{
								ModEventType::MusicPlayerStopped,
								this,
								nullptr
								});
						}
						break;
					}
					default:
						break;
				}
			}
		}
	}

	if (!currentMusicData && musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	// Handle Song Description
	if (descriptionDisplayed)
	{
		auto now = std::chrono::steady_clock::now();
		if (now - lastDisplayTime >= std::chrono::milliseconds(displayDuration))
		{
			Logging::Write(logPrefix, "Hiding song description after %d ms", displayDuration);
			const FunctionData* functionData = ModManager::GetFunctionData("ShowMusicDescription");
			if (!functionData || !functionData->address)
			{
				Logging::Write(logPrefix, "Show description function not found, cannot hide description");
			}
			else
			{
				descriptionDisplayed = false;
				GenericFunction_t showMusicDescriptionFunc = reinterpret_cast<GenericFunction_t>(
					functionData->address
				);
				showMusicDescriptionFunc(nullptr, nullptr, nullptr, nullptr);
			}
		}
	}
}

void MusicPlayer::OnPreExit()
{
	if (currentMusicPausedByBlocker.exchange(false) || areaMusicPauseStateActive.load())
	{
		SetAreaMusicPauseState(false);
	}
	CancelPendingAreaMusicTransition("pre-exit");
	pendingNativeOffsetMs.store(0);
	AreaMusicManager::RestoreNativeAreaMusicOffset();
	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
	currentMusicMaxLength.store(0);
	btTerritoryBlocksMusic.store(false);
	muleTerritoryBlocksMusic.store(false);
	chiralNetworkBlocksMusic.store(false);
	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}
}

void MusicPlayer::OnUIButtonAction(const UIButtonAction& action)
{
	switch (action)
	{
		case UIButtonAction::TOGGLE_MUSIC:
		{
			if (currentMusicIsPlaying.load())
			{
				Logging::Write(logPrefix, "Toggling music off...");
				StopMusic();
			}
			else
			{
				Logging::Write(logPrefix, "Toggling music on...");
				if (loopMode == LoopMode::ONE)
				{
					Logging::Write(logPrefix, "Loop mode is ONE, playing current song...");
					PlayCurrentSong();
				}
				else
				{
					Logging::Write(logPrefix, "Loop mode is ALL, playing next song...");
					PlayNextSong();
				}
			}
			break;
		}
		case UIButtonAction::SHUFFLE_PLAYLIST:
		{
			if (songQueue.IsShuffled())
			{
				songQueue.Reset();
				StopMusic();
				ModManager* instance = ModManager::GetInstance();
				if (instance)
				{
					instance->DispatchEvent(ModEvent{
						ModEventType::MusicPlayerStopped,
						this,
						nullptr
					});
				}
			}
			else
			{
				songQueue.Shuffle();
				ModManager* instance = ModManager::GetInstance();
				if (instance)
				{
					instance->DispatchEvent(ModEvent{
						ModEventType::MusicPlayerShuffled,
						this,
						nullptr
						});
				}
				PlayNextSong();
			}
			break;
		}
		case UIButtonAction::TOGGLE_LOOP_MODE:
		{
			loopMode = static_cast<LoopMode>(
				(static_cast<int>(loopMode) + 1) % static_cast<int>(LoopMode::COUNT)
			);
			Logging::Write(logPrefix, "Loop mode changed to: %d", static_cast<int>(loopMode));
			break;
		}
		default:
			break;
	}
}

void MusicPlayer::PlayMusicHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "Play Music Hook";
	const bool blockerPausedByActiveBlocker = currentMusicPausedByBlocker.load() && HasActiveMusicBlocker();
	if (!blockerPausedByActiveBlocker)
	{
		Logging::Write(logPrefix, "PlayMusic called with arg1: %p, arg2: %p, arg3: %p, arg4: %p",
			arg1, arg2, arg3, arg4
		);
	}
	if (!playMusicFuncRCXAddress)
	{
		playMusicFuncRCXAddress = reinterpret_cast<uintptr_t>(arg1);
		Logging::Write(logPrefix, "1st arg address set to: %p", (void*)playMusicFuncRCXAddress);
	}

	auto silenceIt = ModConfiguration::Databases::interruptorDatabase.find("Silence");
	if (silenceIt != ModConfiguration::Databases::interruptorDatabase.end())
	{
		const MusicData& silenceData = silenceIt->second;
		if (reinterpret_cast<uintptr_t>(arg3) == silenceData.address)
		{
			uintptr_t arg2Address = reinterpret_cast<uintptr_t>(arg2);
			if (playMusicFuncRDXAddress != arg2Address)
			{
				playMusicFuncRDXAddress = arg2Address;
				Logging::Write(logPrefix, "2nd arg address set to: %p", (void*)playMusicFuncRDXAddress);
			}
		}
	}

	const char* gameInterruptorName = nullptr;
	bool gameCalledSilence = false;
	gameCalledInterruptor = false;
	for (const auto& [name, interruptorData] : ModConfiguration::Databases::interruptorDatabase)
	{
		if (reinterpret_cast<uintptr_t>(arg3) == interruptorData.address)
		{
			gameInterruptorName = interruptorData.name ? interruptorData.name : name.c_str();
			gameCalledSilence = name == "Silence";
			gameCalledInterruptor = !gameCalledSilence || blockerPausedByActiveBlocker;
			break;
		}
	}

	if (gameCalledSilence && blockerPausedByActiveBlocker)
	{
		return;
	}

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play music function is not found, cannot play music");
		return;
	}

	gameCalledSong = false;
	const MusicData* songToPlay = nullptr;
	if (!gameCalledInterruptor)
	{
		for (const auto& [name, musicData] : ModConfiguration::Databases::songDatabase)
		{
			if (name == "Music Pool Start")
			{
				continue; // Skip pool music start, it's in the database for dev purposes
			}

			if (reinterpret_cast<uintptr_t>(arg3) == musicData.address)
			{
				gameCalledSong = true;
				songToPlay = &musicData;
				break;
			}
		}
	}

	if (songToPlay && gameCalledSong && !ModConfiguration::allowScriptedSongs)
	{
		Logging::Write(logPrefix,
			"Configuration does not allow scripted songs, skipping song %s",
			songToPlay->name
		);
		return;
	}

	if ((gameCalledInterruptor || gameCalledSong) && (currentMusicData || pendingMusicData))
	{
		const char* interruptionReason = gameCalledInterruptor
			? (gameInterruptorName ? gameInterruptorName : "game interruptor")
			: (songToPlay && songToPlay->name ? songToPlay->name : "game music");
		if (blockerPausedByActiveBlocker)
		{
			if (!gameCalledSilence)
			{
				Logging::Write(logPrefix,
					"Blocked game music interruption \"%s\" while area music is blocker-paused",
					interruptionReason
				);
			}
			return;
		}
		else
		{
			if (currentMusicPausedByBlocker.exchange(false) || areaMusicPauseStateActive.load())
			{
				SetAreaMusicPauseState(false);
			}
			CancelPendingAreaMusicTransition(interruptionReason);
			if (AreaMusicManager::UsesOverride(currentMusicData))
			{
				AreaMusicManager::Unset();
			}
			currentMusicData = nullptr;
			currentMusicIsPlaying.store(false);
			currentMusicPlayTime.store(0);
			currentMusicMaxLength.store(0);

			ModManager* instance = ModManager::GetInstance();
			if (instance)
			{
				const char* interruptionMessage = "Music player interrupted.";
				instance->DispatchEvent(ModEvent{
					ModEventType::MusicPlayerInterrupted,
					nullptr,
					&interruptionMessage
				});
			}
		}
	}

	if (
		songToPlay
		&& songToPlay->name
		&& std::strcmp(songToPlay->name, AreaMusicManager::TargetTrackName) == 0
	)
	{
		AreaMusicManager::Unset();
	}

	/*if (!gameCalledSong && !gameCalledInterruptor)
	{
		Logging::Write(logPrefix, "Game called PlayMusic function with arg3: %p", arg3);
	}*/

	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::PlayUISoundHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "Play UI Sound";

	if ((currentMusicData || pendingMusicData) && arg1 && arg2)
	{
		std::vector<uint8_t> targetBytes;
		std::vector<bool> masks;
		for (const auto& nameInterruptorPair : ModConfiguration::Databases::interruptorUIDatabase)
		{
			bool match = true;
			const MusicData& interruptor = nameInterruptorPair.second;

			targetBytes.clear();
			masks.clear();
			MemoryUtils::ParseHexString(interruptor.signature, targetBytes, masks);
			for (size_t i = 0; i < targetBytes.size(); i++)
			{
				if (masks[i])
				{
					if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(arg2) + i) != targetBytes[i])
					{
						//Logging::Write(logPrefix, "Interruptor %s not matched at offset %zu", interruptor.name, i);
						match = false;
						break; // not matched, skip to next interruptor
					}
				}
			}

			if (match)
			{
				Logging::Write(logPrefix, "Interruptor %s matched, stopping autoplay", interruptor.name);
				if (currentMusicPausedByBlocker.load() && HasActiveMusicBlocker())
				{
					Logging::Write(logPrefix,
						"UI interruptor \"%s\" occurred while area music is blocker-paused; preserving paused track",
						interruptor.name
					);
				}
				else
				{
					if (currentMusicPausedByBlocker.exchange(false) || areaMusicPauseStateActive.load())
					{
						SetAreaMusicPauseState(false);
					}
					CancelPendingAreaMusicTransition(interruptor.name);
					if (AreaMusicManager::UsesOverride(currentMusicData))
					{
						AreaMusicManager::Unset();
					}
					currentMusicData = nullptr;
					currentMusicIsPlaying.store(false);
					currentMusicMaxLength.store(0);

					ModManager* instance = ModManager::GetInstance();
					if (instance)
					{
						std::string interruptionReasonStr = std::string(interruptor.name) + ": music player interrupted.";
						const char* interruptionReason = interruptionReasonStr.c_str();
						instance->DispatchEvent(ModEvent{
							ModEventType::MusicPlayerInterrupted,
							nullptr,
							&interruptionReason
						});
					}
				}

				break;
			}
		}
	}

	//Logging::Write(logPrefix, "PlayUISound called with arg2: %p", arg2);

	const FunctionData* playUISoundFuncData = ModManager::GetFunctionData("PlayUISound");
	if (!playUISoundFuncData || !playUISoundFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play UI sound function is not found, cannot play sound.");
		return;
	}
	reinterpret_cast<GenericFunction_t>(playUISoundFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

void MusicPlayer::ShowMusicDescriptionCoreHook(void* arg1, void* arg2, void* arg3, void* arg4)
{
	constexpr const char* logPrefix = "Show Music Description Core Hook";

	const FunctionData* showMusicDescriptionCoreFuncData = ModManager::GetFunctionData("ShowMusicDescriptionCore");
	if (!showMusicDescriptionCoreFuncData || !showMusicDescriptionCoreFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original show music description core function was not hooked, cannot show description");
		return;
	}

	if (gameCalledSong && !ModConfiguration::allowScriptedSongs)
	{
		Logging::Write(logPrefix, "Configuration does not allow scripted songs, skipping song description");
		return;
	}

	reinterpret_cast<GenericFunction_t>(showMusicDescriptionCoreFuncData->originalFunction)(arg1, arg2, arg3, arg4);
}

bool MusicPlayer::SetAreaMusicPauseState(bool paused)
{
	if (paused)
	{
		if (areaMusicPauseStateActive.exchange(true))
		{
			return true;
		}
	}
	else
	{
		if (!areaMusicPauseStateActive.exchange(false))
		{
			return true;
		}
	}

	auto postEventFallback = [paused](const char* missingControl) {
		if (PostNativeAreaMusicEvent(paused))
		{
			Logging::Write(logPrefix, "Native area music %s unavailable; posted %s event fallback",
				missingControl,
				paused ? "pause" : "resume"
			);
			return true;
		}

		areaMusicPauseStateActive.store(!paused);
		Logging::Write(logPrefix, "Native area music %s unavailable, cannot %s area music",
			missingControl,
			paused ? "pause" : "resume"
		);
		return false;
	};

	NativeAreaMusicControls& controls = ResolveNativeAreaMusicControls();
	if (!controls.pauseFunction || !controls.resumeFunction || !controls.contextSlot)
	{
		return postEventFallback("controls");
	}

	uintptr_t context = ReadNativeAreaMusicContext(controls.contextSlot);
	if (!context)
	{
		return postEventFallback("context");
	}

	volatile long* pauseCounter = reinterpret_cast<volatile long*>(
		context + NativeAreaMusicPauseCounterOffset
	);
	long counterBefore = *pauseCounter;
	if (!paused && counterBefore < 1)
	{
		InterlockedExchange(pauseCounter, 1);
		counterBefore = 1;
	}

	uintptr_t functionAddress = paused
		? controls.pauseFunction
		: controls.resumeFunction;
	reinterpret_cast<NativeAreaMusicFunc>(functionAddress)(reinterpret_cast<void*>(context));

	long counterAfter = *pauseCounter;
	if (paused && counterAfter <= counterBefore)
	{
		InterlockedIncrement(pauseCounter);
		counterAfter = *pauseCounter;
		PostNativeAreaMusicEvent(true);
	}

	Logging::Write(logPrefix, "Called native area music %s function at %p (context %p, counter %ld -> %ld)",
		paused ? "pause" : "resume",
		reinterpret_cast<void*>(functionAddress),
		reinterpret_cast<void*>(context),
		counterBefore,
		counterAfter
	);
	return true;
}

bool MusicPlayer::HasActiveMusicBlocker()
{
	return btTerritoryBlocksMusic.load()
		|| muleTerritoryBlocksMusic.load()
		|| chiralNetworkBlocksMusic.load();
}

void MusicPlayer::InstallMusicAddressWatcher()
{
	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	if (!currentMusicData)
	{
		return;
	}

	if (!playingLoopAddress)
	{
		Logging::Write(logPrefix, "PlayingLoop function signature was not found, cannot install music watcher");
		return;
	}

	uintptr_t addressToWatch = currentMusicData->address + musicAddressAccessOffset;
	musicAddressWatcher = std::make_unique<BreakpointWatcher>(
		[] {
			if (currentMusicPausedByBlocker.load())
			{
				return;
			}

			std::chrono::milliseconds songDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - currentMusicStartTime
			);
			currentMusicPlayTime.store(songDuration.count());
		},
		[] {
			constexpr const char* logPrefix = "Music Address Watcher";
			if (currentMusicPausedByBlocker.load())
			{
				Logging::Write(logPrefix, "Ignoring access timeout while area music is paused by a blocker");
				return;
			}

			std::chrono::milliseconds songDuration =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - currentMusicStartTime
				);
			Logging::Write(logPrefix, "Music stopped playing... Duration was %lld ms", songDuration.count());
			currentMusicIsPlaying.store(false);
		},
		watcherPollingInterval,
		timeTillAutoplay
	);
	musicAddressWatcher->Install(addressToWatch, playingLoopAddress, watchedInstructionLength);
}

bool MusicPlayer::PauseCurrentMusicForBlocker(const char* reason)
{
	if (pendingMusicData && !currentMusicData)
	{
		CancelPendingAreaMusicTransition(reason);
		return false;
	}

	if (!currentMusicData)
	{
		return false;
	}

	if (currentMusicPausedByBlocker.load())
	{
		return true;
	}

	if (currentMusicIsPlaying.load())
	{
		const std::chrono::milliseconds songDuration =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - currentMusicStartTime
			);
		currentMusicPlayTime.store(songDuration.count());
	}

	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	currentMusicPausedByBlocker.store(true);
	currentMusicIsPlaying.store(false);
	if (!PlaySilenceForAreaMusicTransition())
	{
		currentMusicPausedByBlocker.store(false);
		return false;
	}

	Logging::Write(logPrefix,
		"Paused area music \"%s\"%s%s",
		currentMusicData->name ? currentMusicData->name : "",
		reason && reason[0] ? ": " : "",
		reason && reason[0] ? reason : ""
	);
	return true;
}

bool MusicPlayer::ResumeCurrentMusicForBlocker(const char* reason)
{
	if (HasActiveMusicBlocker())
	{
		return false;
	}

	if (!currentMusicPausedByBlocker.load())
	{
		if (areaMusicPauseStateActive.load())
		{
			SetAreaMusicPauseState(false);
		}
		return false;
	}

	if (!currentMusicData)
	{
		currentMusicPausedByBlocker.store(false);
		SetAreaMusicPauseState(false);
		return false;
	}

	return RestartCurrentMusicFromSavedPosition(reason);
}

bool MusicPlayer::RestartCurrentMusicFromSavedPosition(const char* reason)
{
	const MusicData* data = currentMusicData;
	if (!data)
	{
		return false;
	}

	const long long resumeMs = (std::max)(0LL, currentMusicPlayTime.load());
	const bool usesAreaOverride = AreaMusicManager::UsesOverride(data);
	const AreaMusicManager::AreaMusicChain* nativeChain =
		(!usesAreaOverride && resumeMs > 0)
			? AreaMusicManager::LookupChainForSong(data->name)
			: nullptr;
	if (usesAreaOverride && resumeMs > 0)
	{
		// Custom-track path: patches DBSS chain via Register() after PlayMusic.
		AreaMusicManager::SetNextPlaybackStartOffsetMs(resumeMs);
	}
	else if (nativeChain)
	{
		// Native area song path: stash the offset so PlayMusic applies it to the resolved chain inline.
		pendingNativeOffsetMs.store(resumeMs);
	}

	if (areaMusicPauseStateActive.load())
	{
		SetAreaMusicPauseState(false);
	}

	currentMusicData = nullptr;
	currentMusicPausedByBlocker.store(false);
	currentMusicIsPlaying.store(false);
	gameCalledInterruptor = false;
	gameCalledSong = false;
	PlayMusic(data, false);

	if (currentMusicData != data || !currentMusicIsPlaying.load())
	{
		if (usesAreaOverride)
		{
			AreaMusicManager::SetNextPlaybackStartOffsetMs(0);
		}
		if (nativeChain)
		{
			AreaMusicManager::RestoreNativeAreaMusicOffset();
		}
		pendingNativeOffsetMs.store(0);
		currentMusicData = data;
		currentMusicPausedByBlocker.store(false);
		currentMusicIsPlaying.store(false);
		Logging::Write(logPrefix,
			"Could not restart area music \"%s\" after blocker cleared",
			data->name ? data->name : ""
		);
		return false;
	}

	if ((usesAreaOverride || nativeChain) && resumeMs > 0)
	{
		currentMusicPlayTime.store(resumeMs);
		currentMusicStartTime = std::chrono::steady_clock::now()
			- std::chrono::milliseconds(resumeMs);
	}

	if ((usesAreaOverride || nativeChain) && resumeMs > 0)
	{
		Logging::Write(logPrefix,
			"Resumed area music \"%s\" from %lld ms%s%s%s",
			data->name ? data->name : "",
			resumeMs,
			nativeChain ? " (native chain)" : "",
			reason && reason[0] ? ": " : "",
			reason && reason[0] ? reason : ""
		);
	}
	else
	{
		Logging::Write(logPrefix,
			"Restarted area music \"%s\" after blocker clear%s%s",
			data->name ? data->name : "",
			reason && reason[0] ? ": " : "",
			reason && reason[0] ? reason : ""
		);
	}
	return true;
}

bool MusicPlayer::PlaySilenceForAreaMusicTransition()
{
	auto silenceIt = ModConfiguration::Databases::interruptorDatabase.find("Silence");
	if (silenceIt == ModConfiguration::Databases::interruptorDatabase.end() || !silenceIt->second.address)
	{
		Logging::Write(logPrefix, "Silence music data not found, cannot restart area music cleanly");
		return false;
	}

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play music function was not hooked, cannot restart area music cleanly");
		return false;
	}

	void* arg1 = reinterpret_cast<void*>(playMusicFuncRCXAddress);
	void* arg2 = reinterpret_cast<void*>(playMusicFuncRDXAddress);
	if (!arg1 || !arg2)
	{
		Logging::Write(logPrefix, "PlayMusic arguments are not initialized, cannot restart area music cleanly");
		return false;
	}

	void* arg3 = reinterpret_cast<void*>(silenceIt->second.address);
	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, 0);
	Logging::Write(logPrefix, "Played Silence before restarting area music template");
	return true;
}

bool MusicPlayer::QueueAreaMusicTransition(const MusicData* data, bool displayDescription)
{
	if (!data || !AreaMusicManager::IsTemplateTrack(data))
	{
		return false;
	}

	if (currentMusicPausedByBlocker.exchange(false) || areaMusicPauseStateActive.load())
	{
		SetAreaMusicPauseState(false);
	}

	if (!PlaySilenceForAreaMusicTransition())
	{
		return false;
	}

	AreaMusicManager::Unset();

	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
	currentMusicPlayTime.store(0);
	currentMusicMaxLength.store(0);
	currentMusicPausedByBlocker.store(false);
	pendingMusicData = data;
	pendingMusicDisplayDescription = displayDescription;
	pendingMusicOverridePrepared = false;
	pendingMusicStartTime = std::chrono::steady_clock::now();
	return true;
}

bool MusicPlayer::ShowMusicDescription(const MusicData* data)
{
	if (!data || data->type != MusicType::SONG)
	{
		return false;
	}

	const FunctionData* showMusicDescriptionFuncData = ModManager::GetFunctionData("ShowMusicDescription");
	if (!showMusicDescriptionFuncData || !showMusicDescriptionFuncData->address)
	{
		Logging::Write(logPrefix, "Show music description function not found, cannot show description");
		return false;
	}

	GenericFunction_t showMusicDescriptionFunc = reinterpret_cast<GenericFunction_t>(
		showMusicDescriptionFuncData->address
	);

	if (descriptionDisplayed)
	{
		descriptionDisplayed = false;
		showMusicDescriptionFunc(nullptr, nullptr, nullptr, nullptr); // clear
	}

	if (data->descriptionID)
	{
		void* descriptionId = reinterpret_cast<void*>(data->descriptionID);
		Logging::Write(logPrefix, "Showing song description for ID: %p", descriptionId);
		showMusicDescriptionFunc(nullptr, descriptionId, nullptr, nullptr);

		lastDisplayTime = std::chrono::steady_clock::now();
		descriptionDisplayed = true;
		return true;
	}

	if (!data->name || !data->name[0])
	{
		return false;
	}

	const FunctionData* showMusicDescriptionFromTextFuncData =
		ModManager::GetFunctionData("ShowMusicDescriptionFromText");
	if (!showMusicDescriptionFromTextFuncData || !showMusicDescriptionFromTextFuncData->address)
	{
		Logging::Write(logPrefix, "Show music description from text function not found, cannot show custom description");
		return false;
	}

	std::wstring titleText = Utils::Utf8ToWstring("\"" + std::string(data->name) + "\"");
	if (titleText.empty())
	{
		Logging::Write(logPrefix, "Could not convert song title to UTF-16, cannot show custom description");
		return false;
	}

	std::wstring artistText;
	const wchar_t* artistTextList[1] = {};
	int32_t artistTextCount = 0;
	if (data->artist && data->artist[0])
	{
		artistText = Utils::Utf8ToWstring(data->artist);
		if (!artistText.empty())
		{
			artistTextList[0] = artistText.c_str();
			artistTextCount = 1;
		}
	}

	using ShowMusicDescriptionFromText_t = void (*)(
		uint32_t,
		const wchar_t*,
		const wchar_t**,
		int32_t,
		const wchar_t**,
		int32_t,
		float
	);

	ShowMusicDescriptionFromText_t showMusicDescriptionFromTextFunc =
		reinterpret_cast<ShowMusicDescriptionFromText_t>(showMusicDescriptionFromTextFuncData->address);
	showMusicDescriptionFromTextFunc(
		0,
		titleText.c_str(),
		artistTextCount > 0 ? artistTextList : nullptr,
		artistTextCount,
		nullptr,
		0,
		0.0f
	);

	Logging::Write(logPrefix,
		"Showing custom song description: \"%s\" - \"%s\"",
		data->name,
		data->artist ? data->artist : ""
	);
	lastDisplayTime = std::chrono::steady_clock::now();
	descriptionDisplayed = true;
	return true;
}

void MusicPlayer::PlayMusic(const MusicData* data, bool displayDescription=true)
{
	if (musicAddressWatcher)
	{
		musicAddressWatcher->Uninstall();
		musicAddressWatcher.reset();
	}

	// Any prior native-area-song offset patch (BeginTrim / segment duration) is undone here so each playback starts
	// from a clean Wwise state. If a pending offset has been queued by RestartCurrentMusicFromSavedPosition, the
	// resolved chain gets re-patched below, after the per-track logic has identified the song.
	AreaMusicManager::RestoreNativeAreaMusicOffset();

	if (!data || !data->address)
	{
		Logging::Write(logPrefix, "Invalid music data provided, cannot play music");
		pendingNativeOffsetMs.store(0);
		return;
	}

	const long long stagedNativeOffsetMs = pendingNativeOffsetMs.exchange(0);
	const AreaMusicManager::AreaMusicChain* diagnosticChain =
		(data->name && !AreaMusicManager::UsesOverride(data))
			? AreaMusicManager::LookupChainForSong(data->name)
			: nullptr;
	if (diagnosticChain)
	{
		AreaMusicManager::DiagnosticLogChainState(diagnosticChain, "pre-patch");
	}
	if (stagedNativeOffsetMs > 0 && !AreaMusicManager::UsesOverride(data) && data->name)
	{
		if (diagnosticChain)
		{
			AreaMusicManager::PatchNativeAreaMusicOffset(diagnosticChain, stagedNativeOffsetMs);
		}
	}
	if (diagnosticChain)
	{
		AreaMusicManager::DiagnosticLogChainState(diagnosticChain, "post-patch-pre-PlayMusic");
	}

	const FunctionData* playMusicFuncData = ModManager::GetFunctionData("PlayMusic");
	if (!playMusicFuncData || !playMusicFuncData->originalFunction)
	{
		Logging::Write(logPrefix, "Original play music function was not hooked, cannot play music");
		return;
	}

	// Always reset flags to be safe
	gameCalledSong = false;
	gameCalledInterruptor = false;

	if (
		currentMusicData
		&& AreaMusicManager::IsTemplateTrack(currentMusicData)
		&& AreaMusicManager::IsTemplateTrack(data)
	)
	{
		Logging::Write(logPrefix, "Queueing area music restart for \"%s\"", data->name);
		if (QueueAreaMusicTransition(data, displayDescription))
		{
			return;
		}
		Logging::Write(logPrefix, "Area music restart transition failed; trying immediate playback");
	}

	if (
		ModConfiguration::showSongDescription && displayDescription
	)
	{
		ShowMusicDescription(data);
	}

	void* arg1 = reinterpret_cast<void*>(playMusicFuncRCXAddress);
	if (!arg1)
	{
		Logging::Write(logPrefix, "Failed to find play music first arg");
		return;
	}
	Logging::Write(logPrefix, "First argument read successfully: %p", arg1);

	void* arg2 = reinterpret_cast<void*>(playMusicFuncRDXAddress);
	if (!arg2)
	{
		Logging::Write(logPrefix, "Failed to find play music second arg");
		return;
	}
	Logging::Write(logPrefix, "Second argument read successfully: %p", arg2);

	void* arg3 = reinterpret_cast<void*>(data->address);
	Logging::Write(logPrefix, "Third argument read successfully: %p", arg3);

	long long playbackMaxLength = data->maxLength;
	if (AreaMusicManager::UsesOverride(data))
	{
		if (!AreaMusicManager::Register(data))
		{
			Logging::Write(logPrefix,
				"Area music override was not registered for \"%s\"; canceling playback",
				data->name ? data->name : ""
			);
			AreaMusicManager::Unset();
			PlaySilenceForAreaMusicTransition();
			currentMusicData = nullptr;
			currentMusicIsPlaying.store(false);
			currentMusicPlayTime.store(0);
			currentMusicMaxLength.store(0);
			currentMusicPausedByBlocker.store(false);
			return;
		}
		playbackMaxLength = AreaMusicManager::GetRegisteredEffectiveDurationMs(data);
		Logging::Write(logPrefix,
			AreaMusicManager::DurationControlledByWwise()
				? "Using patched Wwise media metadata for custom area track \"%s\"; runtime duration guard is %lld ms"
				: "Wwise media metadata was not patched for custom area track \"%s\"; runtime duration guard is %lld ms",
			data->name ? data->name : "",
			playbackMaxLength
		);
	}
	else
	{
		AreaMusicManager::Unset();
	}

	currentMusicPlayTime.store(0);
	currentMusicMaxLength.store(playbackMaxLength);
	currentMusicPausedByBlocker.store(false);
	currentMusicStartTime = std::chrono::steady_clock::now();
	// Open a chain-capture window so any music-typed Wwise object lookup that follows gets logged with the song name.
	// The game's PlayMusic for an area song triggers Wwise to look up the active music-switch, segment, track, source
	// in order; logging those tells us the chain to patch for offset-resume.
	AreaMusicManager::OpenChainCaptureWindow(data->name ? data->name : "", 2000);
	reinterpret_cast<GenericFunction_t>(playMusicFuncData->originalFunction)(arg1, arg2, arg3, 0);
	if (diagnosticChain)
	{
		AreaMusicManager::DiagnosticLogChainState(diagnosticChain, "post-PlayMusic-return");
	}
	Logging::Write(logPrefix,
		"Playing BGM: \"%s\" (runtime duration guard %lld ms)",
		data->name,
		playbackMaxLength
	);

	if (data->type == MusicType::SONG || data->type == MusicType::AMBIENT)
	{
		currentMusicData = data;
		currentMusicIsPlaying.store(true);
		InstallMusicAddressWatcher();
	}
}

void MusicPlayer::PlayCurrentSong()
{
	if (songQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No current song available in the queue");
		return;
	}

	// if no song is currently playing, display description if mod setting allows it
	// don't display description again if looping same song
	bool displayDescription = songQueue.GetCurrentIndex() < 0;

	const MusicData* currentSong = songQueue.GetCurrent();
	if (!currentSong)
	{
		Logging::Write(logPrefix, "No current song available in the queue");
		return;
	}

	PlayMusic(currentSong, displayDescription);
}

void MusicPlayer::PlayNextSong()
{
	if (songQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No next songs available in the queue");
		return;
	}

	const MusicData* nextSong = songQueue.GetNext();
	if (!nextSong)
	{
		Logging::Write(logPrefix, "No next songs available in the queue");
		return;
	}
	Logging::Write(logPrefix, "Attempting to play next song: %s", nextSong->name);
	PlayMusic(nextSong);
}

void MusicPlayer::PlayPreviousSong()
{
	if (songQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No previous song available in the queue");
		return;
	}

	const MusicData* previousSong = songQueue.GetPrevious();
	if (!previousSong)
	{
		Logging::Write(logPrefix, "No previous song available in the queue");
		return;
	}
	Logging::Write(logPrefix, "Attempting to play previous song: %s", previousSong->name);
	PlayMusic(previousSong);
}

void MusicPlayer::PlayByName(const std::string& name)
{
	if (name.empty())
	{
		Logging::Write(logPrefix, "Cannot find music with an empty name");
		return;
	}

	auto interruptorIt = ModConfiguration::Databases::interruptorDatabase.find(name);
	if (interruptorIt != ModConfiguration::Databases::interruptorDatabase.end())
	{
		const MusicData* interruptorMusicData = &(interruptorIt->second);
		PlayMusic(interruptorMusicData);
		return;
	}

	auto songIt = ModConfiguration::Databases::songDatabase.find(name);
	if (songIt != ModConfiguration::Databases::songDatabase.end())
	{
		const MusicData* songMusicData = &(songIt->second);
		PlayMusic(songMusicData);
		return;
	}

	auto customSongIt = ModConfiguration::Databases::customSongDatabase.find(name);
	if (customSongIt != ModConfiguration::Databases::customSongDatabase.end())
	{
		const MusicData* customSongMusicData = &(customSongIt->second);
		PlayMusic(customSongMusicData);
		return;
	}
}

void MusicPlayer::StopMusic()
{
	if (currentMusicPausedByBlocker.exchange(false) || areaMusicPauseStateActive.load())
	{
		SetAreaMusicPauseState(false);
	}

	CancelPendingAreaMusicTransition("stop requested");

	if (!currentMusicData)
	{
		return;
	}

	auto it = ModConfiguration::Databases::interruptorDatabase.find("Silence");
	if (it == ModConfiguration::Databases::interruptorDatabase.end())
	{
		Logging::Write(logPrefix, "Silence music data not found, cannot stop music");
		return;
	}

	const MusicData* silenceData = &(it->second);
	PlayMusic(silenceData, false); // Play silence to stop current music
	currentMusicData = nullptr;
	currentMusicIsPlaying.store(false);
	currentMusicPausedByBlocker.store(false);
	currentMusicMaxLength.store(0);
}

void MusicPlayer::PlayNextInPool()
{
	if (poolQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No next music available in the pool");
		return;
	}

	const MusicData* nextPoolMusic = poolQueue.GetNext();
	if (!nextPoolMusic)
	{
		Logging::Write(logPrefix, "No next music available in the pool");
		return;
	}

	Logging::Write(logPrefix, "Attempting to play next music from pool: %s", nextPoolMusic->name);
	MemoryUtils::PrintBytesAtAddress(nextPoolMusic->address, 24);

	PlayMusic(nextPoolMusic);
}

void MusicPlayer::PlayPreviousInPool()
{
	if (poolQueue.IsEmpty())
	{
		Logging::Write(logPrefix, "No previous music available in the pool");
		return;
	}

	const MusicData* previousPoolMusic = poolQueue.GetPrevious();
	if (!previousPoolMusic)
	{
		Logging::Write(logPrefix, "No previous music available in the pool");
		return;
	}

	Logging::Write(logPrefix, "Attempting to play previous music from pool: %s", previousPoolMusic->name);
	MemoryUtils::PrintBytesAtAddress(previousPoolMusic->address, 24);

	PlayMusic(previousPoolMusic);
}

void MusicPlayer::OnInputPress(const InputCode& inputCode)
{
	if (inputCode.source == InputSource::KBM)
	{
		if (ModConfiguration::devMode)
		{
			if (inputCode.code == VK_F1)
			{
				PlayNextInPool();
			}
			if (inputCode.code == VK_F2)
			{
				PlayPreviousInPool();
			}
			if (inputCode.code == VK_F3)
			{
				static uint16_t currentDescriptionId = 0;

				const FunctionData* showSongDescriptionFuncData = ModManager::GetFunctionData("ShowMusicDescription");
				if (!showSongDescriptionFuncData || !showSongDescriptionFuncData->address)
				{
					Logging::Write(logPrefix, "Show music description function not found, cannot show description");
				}
				else
				{
					void* descriptionId = reinterpret_cast<void*>(currentDescriptionId++);
					Logging::Write(logPrefix, "Showing song description for ID: %d", descriptionId);
					GenericFunction_t showMusicDescriptionFunc = reinterpret_cast<GenericFunction_t>(
						showSongDescriptionFuncData->address
					);
					showMusicDescriptionFunc(nullptr, descriptionId, nullptr, nullptr);
				}
			}
		}

		if (inputCode.code == VK_F9)
		{
			StopMusic();
			ModManager* instance = ModManager::GetInstance();
			if (instance)
			{
				instance->DispatchEvent(ModEvent{
					ModEventType::MusicPlayerStopped,
					this,
					nullptr
				});
			}
		}
	}
}
