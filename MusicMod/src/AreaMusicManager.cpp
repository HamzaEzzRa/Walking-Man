#include "AreaMusicManager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <Windows.h>

#include "AudioDecoder.h"
#include "GameData.h"
#include "Logger.h"
#include "MinHook.h"
#include "ModConfiguration.h"
#include "ModManager.h"

namespace
{
	constexpr size_t wwiseChunkHeaderSize = 8;
	constexpr size_t hircItemHeaderSize = 5;

	constexpr size_t musicTrackMinSize = 0x47;
	constexpr size_t musicTrackSourceCountOffset = 0x05;
	constexpr size_t musicTrackSourcePluginOffset = 0x09;
	constexpr size_t musicTrackSourceTypeOffset = 0x0d;
	constexpr size_t musicTrackSourceIdOffset = 0x0e;
	constexpr size_t musicTrackMediaSizeOffset = 0x12;
	constexpr size_t musicTrackSourceBitsOffset = 0x16;
	constexpr size_t musicTrackClipCountOffset = 0x17;
	constexpr size_t musicTrackClipTrackIdOffset = 0x1b;
	constexpr size_t musicTrackClipSourceIdOffset = 0x1f;
	constexpr size_t musicTrackClipStartOffset = 0x27;
	constexpr size_t musicTrackClipBeginTrimOffset = 0x2f;
	constexpr size_t musicTrackClipEndTrimOffset = 0x37;
	constexpr size_t musicTrackSourceDurationOffset = 0x3f;

	constexpr size_t musicSegmentMinSize = 0x87;
	constexpr size_t musicSegmentRanSeqIdOffset = 0x0c;
	constexpr size_t musicSegmentTrackIdOffset = 0x27;
	constexpr size_t musicSegmentDurationOffset = 0x46;
	constexpr size_t musicSegmentEndMarkerOffset = 0x66;
	constexpr size_t musicSegmentPostMarkerDurationOffset = 0x7f;

	constexpr const char* wwiseObjectLookupFunctionName = "WwiseObjectLookup";
	constexpr size_t wwiseObjectReleaseVtableOffset = 0x10;
	constexpr size_t liveSegmentMarkerStride = 0x10;
	constexpr size_t liveSegmentMarkerPositionOffset = 0x04;
	constexpr size_t liveTrackSourceStride = 0x10;
	constexpr size_t liveTrackSourceObjectOffset = 0x08;
	constexpr size_t liveTrackSourceObjectSize = 0x28;
	constexpr size_t liveTrackTimingStride = 0x1c;
	constexpr uint32_t wwiseTicksPerMillisecond = 48;

	template<typename T>
	T ReadUnaligned(const uint8_t* data)
	{
		T value{};
		std::memcpy(&value, data, sizeof(T));
		return value;
	}

	template<typename T>
	void WriteUnaligned(uint8_t* data, T value)
	{
		std::memcpy(data, &value, sizeof(T));
	}

	uint32_t MillisecondsToWwiseTicks(long long milliseconds)
	{
		if (milliseconds <= 0)
		{
			return 1;
		}

		const unsigned long long ticks =
			static_cast<unsigned long long>(milliseconds) * wwiseTicksPerMillisecond;
		if (ticks > (std::numeric_limits<uint32_t>::max)())
		{
			return (std::numeric_limits<uint32_t>::max)();
		}

		return static_cast<uint32_t>(ticks);
	}

	uint32_t MillisecondsToWwiseTicksAllowZero(long long milliseconds)
	{
		return milliseconds <= 0 ? 0 : MillisecondsToWwiseTicks(milliseconds);
	}

	long long ClampSourceStartOffsetMs(long long sourceStartMs, long long sourceDurationMs)
	{
		if (sourceStartMs <= 0 || sourceDurationMs <= 1)
		{
			return 0;
		}

		return (std::min)(sourceStartMs, sourceDurationMs - 1);
	}

	bool HasMemoryProtection(uintptr_t address, size_t size, DWORD allowedMask)
	{
		if (!address || size == 0)
		{
			return false;
		}
		if (address > (std::numeric_limits<uintptr_t>::max)() - size)
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
		{
			return false;
		}

		const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
		const uintptr_t regionEnd = regionStart + static_cast<uintptr_t>(mbi.RegionSize);
		const DWORD protection = mbi.Protect & 0xff;
		return mbi.State == MEM_COMMIT
			&& !(mbi.Protect & PAGE_GUARD)
			&& address >= regionStart
				&& address + size <= regionEnd
				&& (protection & allowedMask) != 0;
	}

	bool HasReadableMemory(uintptr_t address, size_t size)
	{
		return HasMemoryProtection(
			address,
			size,
			PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
			| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY
		);
	}

	bool HasWritableMemory(uintptr_t address, size_t size)
	{
		return HasMemoryProtection(
			address,
			size,
			PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY
		);
	}

	bool HasExecutableMemory(uintptr_t address, size_t size)
	{
		return HasMemoryProtection(
			address,
			size,
			PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY
		);
	}

	bool CallWwiseReleaseNoUnwind(void* object, uintptr_t releaseAddress, uint32_t* exceptionCode)
	{
		using ReleaseFn = void(__fastcall*)(void*);
		__try
		{
			reinterpret_cast<ReleaseFn>(releaseAddress)(object);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (exceptionCode)
			{
				*exceptionCode = GetExceptionCode();
			}
			return false;
		}
	}

	bool LookupWwiseObjectNoUnwind(
		uintptr_t lookupAddress,
		void* registry,
		uint32_t objectId,
		int tableIndex,
		void** object,
		uint32_t* exceptionCode
	)
	{
		using LookupFn = void* (__cdecl*)(void*, uint32_t, int);
		__try
		{
			if (object)
			{
				*object = reinterpret_cast<LookupFn>(lookupAddress)(registry, objectId, tableIndex);
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (exceptionCode)
			{
				*exceptionCode = GetExceptionCode();
			}
			if (object)
			{
				*object = nullptr;
			}
			return false;
		}
	}

	void ReleaseWwiseObject(void* object)
	{
		if (!object)
		{
			return;
		}

		const uintptr_t objectAddress = reinterpret_cast<uintptr_t>(object);
		if (!HasReadableMemory(objectAddress, sizeof(uintptr_t)))
		{
			return;
		}

		uintptr_t vtable = *reinterpret_cast<uintptr_t*>(object);
		if (!HasReadableMemory(vtable + wwiseObjectReleaseVtableOffset, sizeof(uintptr_t)))
		{
			return;
		}

		using ReleaseFn = void(__fastcall*)(void*);
		auto release = reinterpret_cast<ReleaseFn>(
			*reinterpret_cast<uintptr_t*>(vtable + wwiseObjectReleaseVtableOffset)
		);
		if (
			release
			&& HasExecutableMemory(reinterpret_cast<uintptr_t>(release), 16)
		)
		{
			uint32_t exceptionCode = 0;
			if (!CallWwiseReleaseNoUnwind(object, reinterpret_cast<uintptr_t>(release), &exceptionCode))
			{
				constexpr const char* logPrefix = "Area Music Metadata";
				Logging::Write(logPrefix,
					"Wwise object release raised exception 0x%08x for object %p",
					exceptionCode,
					object
				);
			}
		}
	}

}

struct AreaMusicManager::LiveWwiseOffsets
{
	size_t segmentMarkerArray;
	size_t segmentMarkerCount;
	size_t segmentDuration;
	size_t trackSourceArray;
	size_t trackSourceCount;
	size_t trackTimingArray;
	size_t trackTimingCount;
	size_t trackSourceDuration;
};

const AreaMusicManager::LiveWwiseOffsets& AreaMusicManager::GetLiveWwiseOffsets()
{
	static constexpr LiveWwiseOffsets dc{
		0x110,
		0x118,
		0x120,
		0xd8,
		0xe0,
		0x100,
		0x108,
		0x110
	};

	static constexpr LiveWwiseOffsets standard{
		0x110,
		0x118,
		0x120,
		0xc8,
		0xd0,
		0xf0,
		0xf8,
		0x100
	};

	return ModConfiguration::gameVersion == GameVersion::STANDARD
		? standard
		: dc;
}

AreaMusicManager::AreaMusicManager()
{
	Initialize();
}

void AreaMusicManager::OnEvent(const ModEvent& event)
{
	switch (event.type)
	{
	case ModEventType::FrameRendered:
	{
		InstallWwiseObjectLookupHook();
		break;
	}
	case ModEventType::PreExitTriggered:
	{
		Unset();
		RetireBuffer();
		break;
	}
	default:
		break;
	}
}

bool AreaMusicManager::UsesOverride(const MusicData* data)
{
	return data && data->customAreaTrack && data->customWemPath;
}

bool AreaMusicManager::IsTemplateTrack(const MusicData* data)
{
	return UsesOverride(data)
		|| (
			data
			&& data->name
			&& std::strcmp(data->name, TargetTrackName) == 0
		);
}

void AreaMusicManager::SetNextPlaybackStartOffsetMs(long long sourceStartMs)
{
	nextPlaybackStartOffsetMs.store((std::max)(0LL, sourceStartMs));
}

bool AreaMusicManager::DurationControlledByWwise()
{
	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	return areaMusicOverrideMetadataPatched;
}

void AreaMusicManager::Initialize()
{
	if (areaMusicMetadataHooksInitialized)
	{
		return;
	}

	areaMusicMetadataHooksInitialized = true;

	HMODULE mainModule = GetModuleHandleA(nullptr);
	if (!mainModule)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Failed to get main module handle for Wwise bank hooks");
		return;
	}

	loadBankMemoryCopyFunc = reinterpret_cast<AkLoadBankMemoryFn>(
		GetProcAddress(mainModule, loadBankMemoryCopyExportName)
	);
	loadBankMemoryViewFunc = reinterpret_cast<AkLoadBankMemoryFn>(
		GetProcAddress(mainModule, loadBankMemoryViewExportName)
	);
	unloadBankMemoryFunc = reinterpret_cast<AkUnloadBankMemoryFn>(
		GetProcAddress(mainModule, unloadBankMemoryExportName)
	);
	setStateFunc = reinterpret_cast<AkSetStateFn>(
		GetProcAddress(mainModule, setStateExportName)
	);

	auto installHook = [](AkLoadBankMemoryFn target, void* hook, void** original, const char* name)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		if (!target)
		{
			Logging::Write(logPrefix, "Wwise %s export was not resolved", name);
			return;
		}

		MH_STATUS created = MH_CreateHook(reinterpret_cast<LPVOID>(target), hook, original);
		if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
		{
			Logging::Write(logPrefix, "Failed to create Wwise %s hook: %d", name, created);
			return;
		}

		MH_STATUS enabled = MH_EnableHook(reinterpret_cast<LPVOID>(target));
		if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
		{
			Logging::Write(logPrefix, "Failed to enable Wwise %s hook: %d", name, enabled);
			return;
		}

		Logging::Write(logPrefix, "Installed Wwise %s hook at %p", name, reinterpret_cast<void*>(target));
	};

	installHook(
		loadBankMemoryCopyFunc,
		reinterpret_cast<void*>(&AreaMusicManager::LoadBankMemoryCopyHook),
		reinterpret_cast<void**>(&originalLoadBankMemoryCopyFunc),
		"LoadBankMemoryCopy"
	);
	installHook(
		loadBankMemoryViewFunc,
		reinterpret_cast<void*>(&AreaMusicManager::LoadBankMemoryViewHook),
		reinterpret_cast<void**>(&originalLoadBankMemoryViewFunc),
		"LoadBankMemoryView"
	);

	if (setStateFunc && !setStateHookInitialized)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		MH_STATUS created = MH_CreateHook(
			reinterpret_cast<LPVOID>(setStateFunc),
			reinterpret_cast<void*>(&AreaMusicManager::SetStateHook),
			reinterpret_cast<void**>(&originalSetStateFunc)
		);
		if (created == MH_OK || created == MH_ERROR_ALREADY_CREATED)
		{
			MH_STATUS enabled = MH_EnableHook(reinterpret_cast<LPVOID>(setStateFunc));
			if (enabled == MH_OK || enabled == MH_ERROR_ENABLED)
			{
				setStateHookInitialized = true;
				Logging::Write(logPrefix,
					"Installed Wwise SetState hook at %p",
					reinterpret_cast<void*>(setStateFunc)
				);
			}
			else
			{
				Logging::Write(logPrefix, "Failed to enable Wwise SetState hook: %d", enabled);
			}
		}
		else
		{
			Logging::Write(logPrefix, "Failed to create Wwise SetState hook: %d", created);
		}
	}
	else if (!setStateFunc)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Wwise SetState export was not resolved");
	}
}

bool AreaMusicManager::InstallWwiseObjectLookupHook()
{
	if (wwiseObjectLookupHookInitialized)
	{
		return originalWwiseObjectLookupFunc != nullptr;
	}

	const FunctionData* lookupFuncData = ModManager::GetFunctionData(wwiseObjectLookupFunctionName);
	if (!lookupFuncData || !lookupFuncData->address)
	{
		return false;
	}

	if (!ModManager::TryHookFunction(
		wwiseObjectLookupFunctionName,
		reinterpret_cast<void*>(&AreaMusicManager::WwiseObjectLookupHook)
	))
	{
		wwiseObjectLookupHookInitialized = true;
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Failed to install Wwise object lookup hook");
		return false;
	}

	wwiseObjectLookupHookInitialized = true;
	originalWwiseObjectLookupFunc =
		reinterpret_cast<WwiseObjectLookupFn>(lookupFuncData->originalFunction);
	if (!originalWwiseObjectLookupFunc)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Wwise object lookup hook installed without an original function");
		return false;
	}

	constexpr const char* logPrefix = "Area Music Metadata";
	Logging::Write(logPrefix,
		"Installed Wwise object lookup hook at %p",
		reinterpret_cast<void*>(lookupFuncData->address)
	);
	return true;
}

void AreaMusicManager::CaptureWwiseObjectRegistry(void* registry)
{
	if (!registry)
	{
		return;
	}

	const uintptr_t registryAddress = reinterpret_cast<uintptr_t>(registry);
	if (!HasWritableMemory(registryAddress, 0x80))
	{
		return;
	}

	const uintptr_t previous = wwiseObjectRegistryAddress.exchange(
		registryAddress,
		std::memory_order_acq_rel
	);
	if (previous != registryAddress)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Captured Wwise object registry at %p", registry);
	}
}

void* __cdecl AreaMusicManager::WwiseObjectLookupHook(void* registry, uint32_t objectId, int tableIndex)
{
	WwiseObjectLookupFn original = originalWwiseObjectLookupFunc;
	if (!original)
	{
		const FunctionData* lookupFuncData = ModManager::GetFunctionData(wwiseObjectLookupFunctionName);
		original =
			lookupFuncData
			? reinterpret_cast<WwiseObjectLookupFn>(lookupFuncData->originalFunction)
			: nullptr;
		originalWwiseObjectLookupFunc = original;
	}

	if (!original)
	{
		return nullptr;
	}

	void* object = original(registry, objectId, tableIndex);
	const uintptr_t capturedRegistryAddress = wwiseObjectRegistryAddress.load(std::memory_order_acquire);
	if (
		registry
		&& reinterpret_cast<uintptr_t>(registry) != capturedRegistryAddress
		&& (object || !capturedRegistryAddress)
	)
	{
		CaptureWwiseObjectRegistry(registry);
	}

	// Chain-capture: while a window is open, log any music-typed lookup so we can discover the segment/track/source
	// chain for the song that just started playing. The window is opened by MusicPlayer::PlayMusic.
	const long long nowMs =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count();
	if (nowMs < chainCaptureExpiryMs.load(std::memory_order_acquire))
	{
		uint8_t hircType = 0;
		{
			std::lock_guard<std::mutex> idLock(musicIdSetMutex);
			auto it = musicHircIdToType.find(objectId);
			if (it != musicHircIdToType.end())
			{
				hircType = it->second;
			}
		}
		if (hircType != 0)
		{
			std::string tag;
			{
				std::lock_guard<std::mutex> tagLock(chainCaptureTagMutex);
				tag = chainCaptureTag;
			}
			const char* typeName =
				hircType == 0x0a ? "Segment" :
				hircType == 0x0b ? "Track" :
				hircType == 0x0c ? "SwitchCntr" :
				hircType == 0x0d ? "RanSeqCntr" :
				"?";
			constexpr const char* logPrefix = "Area Music Chain";
			Logging::Write(logPrefix,
				"Lookup tag=\"%s\" type=%s id=%u (0x%08X)",
				tag.c_str(),
				typeName,
				objectId,
				objectId
			);
		}
	}

	return object;
}

void AreaMusicManager::OpenChainCaptureWindow(const char* tag, long long durationMs)
{
	const long long nowMs =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count();
	chainCaptureExpiryMs.store(nowMs + durationMs, std::memory_order_release);
	{
		std::lock_guard<std::mutex> tagLock(chainCaptureTagMutex);
		chainCaptureTag = tag ? tag : "";
	}
}

void AreaMusicManager::DiagnosticLogChainState(const AreaMusicChain* chain, const char* tag)
{
	if (!chain) return;
	void* segment = LookupWwiseObject(chain->segmentId);
	void* track = LookupWwiseObject(chain->trackId);
	if (!segment || !track)
	{
		Logging::Write("Chain State", "[%s] chain=\"%s\" could not look up segment(%u)/track(%u)",
			tag ? tag : "", chain->songName, chain->segmentId, chain->trackId);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return;
	}

	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();

	const uint32_t segDurationTicks =
		HasReadableMemory(reinterpret_cast<uintptr_t>(segmentBytes + offsets.segmentDuration), sizeof(uint32_t))
			? ReadUnaligned<uint32_t>(segmentBytes + offsets.segmentDuration) : 0xFFFFFFFFu;
	const uint32_t trackSrcDurTicks =
		HasReadableMemory(reinterpret_cast<uintptr_t>(trackBytes + offsets.trackSourceDuration), sizeof(uint32_t))
			? ReadUnaligned<uint32_t>(trackBytes + offsets.trackSourceDuration) : 0xFFFFFFFFu;

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(trackBytes + offsets.trackTimingArray);
	const uint32_t timingCount = ReadUnaligned<uint32_t>(trackBytes + offsets.trackTimingCount);
	uint32_t timing0PlayAt = 0xFFFFFFFFu, timing0Clip = 0xFFFFFFFFu, timing0SrcDur = 0xFFFFFFFFu, timing0BeginTrim = 0xFFFFFFFFu;
	if (
		timingArray
		&& timingCount > 0
		&& HasReadableMemory(reinterpret_cast<uintptr_t>(timingArray), liveTrackTimingStride)
	)
	{
		timing0PlayAt = ReadUnaligned<uint32_t>(timingArray + 0x0c);
		timing0Clip = ReadUnaligned<uint32_t>(timingArray + 0x10);
		timing0SrcDur = ReadUnaligned<uint32_t>(timingArray + 0x14);
		timing0BeginTrim = ReadUnaligned<uint32_t>(timingArray + 0x18);
	}

	Logging::Write("Chain State",
		"[%s] \"%s\" segDuration=%u trackSrcDur=%u | timing[0] playAt=%u clip=%u srcDur=%u beginTrim=%u (timingCount=%u)",
		tag ? tag : "", chain->songName,
		segDurationTicks, trackSrcDurTicks,
		timing0PlayAt, timing0Clip, timing0SrcDur, timing0BeginTrim,
		timingCount
	);

	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
}


namespace
{
	// Captured by hooking AK::SoundEngine::SetState + WwiseObjectLookup. Each chain is a
	// {ranseq, segment, track, source} tuple inside CAkMusicSwitchCntr 878146756. Patching segment + track
	// timing fields adjusts where playback starts for the named song without touching its source media.
	constexpr AreaMusicManager::AreaMusicChain knownAreaMusicChains[] = {
		{"Don't Be So Serious",              124768037,  515599299,   864381405,   14330364},
		{"Bones",                            626513893,  924803744,   585844366,   443301537},
		{"Poznan",                           896125235,  569475449,   905168316,   112514829},
		{"Anything You Need",                608762905,  871006524,   353167231,   990235427},
		{"Easy Way Out",                     922675051,  136040502,   411554128,   573918731},
		{"I'm Leaving",                      504066054,  1004761732,  962966379,   10799056},
		{"Give Up",                          558666898,  233422843,   312215729,   409126050},
		{"Gosia",                            359904631,  316694681,   928172596,   1049365324},
		{"Without You",                      598222498,  352981518,   746624823,   234016975},
		{"Breathe In",                       821147021,  849466441,   318759929,   811953013},
		{"Because We Have To",               925185256,  667964839,   747024383,   44821658},
		{"St. Eriksplan",                    799932185,  520902395,   765370839,   894314852},
		{"Rolling Over",                     899700136,  992828349,   542864971,   271832813},
		{"Once in a Long, Long While...",    481140154,  80937257,    396340672,   534432074},
		{"The Machine",                      177160982,  209990408,   878567193,   931685189},
		{"Patience",                         356011358,  62893057,    704429786,   1065659883},
		{"Not Around",                       970173279,  943762408,   44283622,    428823264},
		{"Please Don't Stop (Chapter 1)",    524824844,  395048971,   533936954,   381534270},
		{"Tonight, tonight, tonight",        339126489,  1031914594,  1028497934,  819564709},
		{"Please Don't Stop (Chapter 2)",    508496364,  980712044,   438536405,   574698350},
		{"Half Asleep",                      1049968,    794164252,   240303610,   320869737},
		{"Waiting (10 Years)",               927076567,  445852071,   315451892,   1054849995},
		{"Nobody Else",                      440407878,  1046003322,  205774222,   300265341},
		{"Asylums For The Feeling",          1023121111, 22286178,    993579869,   208171504},
		{"Almost Nothing",                   1035153539, 190996573,   61205935,    772358996},
		{"BB's Theme",                       727898186,  161525664,   47642293,    554273859},
		{"Pale Yellow",                      624312543,  172520581,   372066990,   630551876},
		{"Goliath",                          129927267,  250138303,   144702745,   210092545},
		{"Control",                          370578840,  899176194,   529120711,   674482479},
		{"Other Me",                         881156219,  506330902,   260773589,   628634042},
		{"Fragile",                          209950558,  771416739,   359994144,   152808688},
		{"Ambient 1",                        174738457,  906105034,   997241961,   612428356},
		{"Ambient 4",                        663805971,  87624654,    1012293903,  378980528},
		{"Ambient 5",                        421722346,  113873444,   901357345,   491944249},
		{"Ambient 6",                        437838398,  397518684,   258488343,   459118830},
		{"Ambient 7",                        475318914,  321848901,   1057275913,  43133207},
		{"Almost Nothing (Instrumental)",    361567052,  828378002,   424219496,   303176799},
		{"Almost Nothing (No Beatbox)",      800867414,  165284959,   57199966,    1019908505},
	};
}

const AreaMusicManager::AreaMusicChain* AreaMusicManager::LookupChainForSong(const char* songName)
{
	if (!songName)
	{
		return nullptr;
	}

	for (const AreaMusicChain& chain : knownAreaMusicChains)
	{
		if (chain.songName && std::strcmp(songName, chain.songName) == 0)
		{
			return &chain;
		}
	}
	return nullptr;
}

bool AreaMusicManager::PatchNativeAreaMusicOffset(const AreaMusicChain* chain, long long sourceStartMs)
{
	if (!chain || sourceStartMs <= 0)
	{
		return false;
	}

	// First clear any prior patch so we always start from the original Wwise metadata.
	RestoreNativeAreaMusicOffset();

	void* segment = LookupWwiseObject(chain->segmentId);
	void* track = LookupWwiseObject(chain->trackId);
	if (!segment || !track)
	{
		constexpr const char* logPrefix = "Area Music Offset";
		Logging::Write(logPrefix,
			"Could not locate live Wwise objects for chain (segment=%u track=%u): segment=%p track=%p",
			chain->segmentId,
			chain->trackId,
			segment,
			track
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();

	if (
		!HasWritableMemory(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t))
		|| !HasWritableMemory(reinterpret_cast<uintptr_t>(trackBytes), offsets.trackSourceDuration + sizeof(uint32_t))
	)
	{
		constexpr const char* logPrefix = "Area Music Offset";
		Logging::Write(logPrefix, "Refusing native offset patch: segment or track memory is not writable");
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);

	// Source duration: read the segment's segmentDuration (matches what the existing custom-track patcher writes there
	// for the custom WEM duration; on a native, unmodified Wwise object it equals the full source duration in ticks).
	// The track's `trackSourceDuration` at +0x110 is NOT reliable on native tracks — it tends to hold a smaller runtime
	// value unrelated to actual source length.
	const uint32_t origSegmentDurationTicks = ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration
	);
	if (origSegmentDurationTicks == 0)
	{
		constexpr const char* logPrefix = "Area Music Offset";
		Logging::Write(logPrefix, "Cannot patch \"%s\": live segment duration is 0", chain->songName);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	const long long sourceDurationMs =
		static_cast<long long>(origSegmentDurationTicks) / wwiseTicksPerMillisecond;
	// Sanity guard: when Wwise hasn't fully initialized the segment yet (typically on the first activation of a
	// state in a process lifetime) `segmentDuration` can be stale or near-zero. Patching with a tiny duration
	// truncates playback to a few ms and the song auto-advances, which is worse than just restarting at offset 0.
	// Skip the patch in that case and let the song restart cleanly; the next pause/resume will see a properly
	// initialized segment and apply the offset.
	if (sourceDurationMs < 1000 || sourceDurationMs > (60LL * 60 * 1000))
	{
		constexpr const char* logPrefix = "Area Music Offset";
		Logging::Write(logPrefix,
			"Skipping native offset patch for \"%s\": live segment duration is implausible (%lld ms from %u ticks)",
			chain->songName,
			sourceDurationMs,
			origSegmentDurationTicks
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}
	const long long clampedStartMs = ClampSourceStartOffsetMs(sourceStartMs, sourceDurationMs);
	const long long effectiveDurationMs = (std::max)(1LL, sourceDurationMs - clampedStartMs);
	const uint32_t sourceStartTicks = MillisecondsToWwiseTicksAllowZero(clampedStartMs);
	const uint32_t effectiveDurationTicks = MillisecondsToWwiseTicks(effectiveDurationMs);

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingCount > 256
		|| (timingArray && timingCount > 0
			&& !HasWritableMemory(reinterpret_cast<uintptr_t>(timingArray),
				static_cast<size_t>(timingCount) * liveTrackTimingStride))
	)
	{
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	// The track's first timing entry's BeginTrimOffset is the existing source-skip the bank ships with — many area
	// songs have a non-zero intro trim (e.g. DBSS has ~13.9 s of pre-song WEM data that's normally skipped). The
	// user's saved playtime is measured in "audible time" (clock starts when PlayMusic fires), so the new BeginTrim
	// must be ORIGINAL + saved-playtime, not just saved-playtime — otherwise resume plays from earlier in the WEM
	// than the user expects.
	uint32_t origBeginTrimTicks = 0;
	if (timingArray && timingCount > 0)
	{
		origBeginTrimTicks = ReadUnaligned<uint32_t>(timingArray + 0x18);
	}
	const uint32_t newBeginTrimTicks = origBeginTrimTicks + sourceStartTicks;

	const uint32_t origSourceDurationTicks = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration
	);

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
		);
	const uint32_t markerCount = ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerCount > 256
		|| (markerArray && markerCount > 0
			&& !HasWritableMemory(reinterpret_cast<uintptr_t>(markerArray),
				static_cast<size_t>(markerCount) * liveSegmentMarkerStride))
		)
	{
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	NativeAreaMusicBackup backup;
	backup.valid = true;
	backup.segmentId = chain->segmentId;
	backup.trackId = chain->trackId;
	backup.origSegmentDurationTicks = ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration
	);
	backup.origTrackSourceDurationTicks = origSourceDurationTicks;
	if (markerArray && markerCount > 0)
	{
		backup.origMarkerPositionTicks.reserve(markerCount);
		for (uint32_t i = 0; i < markerCount; ++i)
		{
			backup.origMarkerPositionTicks.push_back(ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			));
		}
	}
	if (timingArray && timingCount > 0)
	{
		const size_t timingBytes = static_cast<size_t>(timingCount) * liveTrackTimingStride;
		backup.origTrackTimingBytes.assign(timingArray, timingArray + timingBytes);
	}

	WriteUnaligned<uint32_t>(segmentBytes + offsets.segmentDuration, effectiveDurationTicks);
	if (markerArray && markerCount > 0)
	{
		for (uint32_t i = 0; i < markerCount; ++i)
		{
			const uint32_t markerTicks = ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			);
			if (markerTicks > 0 && markerTicks != effectiveDurationTicks)
			{
				WriteUnaligned<uint32_t>(
					markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
					+ liveSegmentMarkerPositionOffset,
					effectiveDurationTicks
				);
			}
		}
	}

	if (timingArray && timingCount > 0)
	{
		for (uint32_t i = 0; i < timingCount; ++i)
		{
			uint8_t* entry = timingArray + (static_cast<size_t>(i) * liveTrackTimingStride);
			WriteUnaligned<uint32_t>(entry + 0x0c, 0);                            // PlayAt
			WriteUnaligned<uint32_t>(entry + 0x10, effectiveDurationTicks);       // ClipDuration
			WriteUnaligned<uint32_t>(entry + 0x18, newBeginTrimTicks);            // BeginTrimOffset (orig + offset)
		}
	}

	nativeAreaMusicBackup = std::move(backup);

	constexpr const char* logPrefix = "Area Music Offset";
	Logging::Write(logPrefix,
		"Patched native offset for \"%s\" (segment=%u track=%u source=%u) sourceDur=%lld ms, audibleStart=%lld ms, effective=%lld ms, origBeginTrim=%u ticks, newBeginTrim=%u ticks",
		chain->songName,
		chain->segmentId,
		chain->trackId,
		chain->sourceId,
		sourceDurationMs,
		clampedStartMs,
		effectiveDurationMs,
		origBeginTrimTicks,
		newBeginTrimTicks
	);

	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
	return true;
}

void AreaMusicManager::RestoreNativeAreaMusicOffset()
{
	NativeAreaMusicBackup backup;
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		if (!nativeAreaMusicBackup.valid)
		{
			return;
		}
		backup = nativeAreaMusicBackup;
		nativeAreaMusicBackup = {};
	}

	void* segment = LookupWwiseObject(backup.segmentId);
	void* track = LookupWwiseObject(backup.trackId);
	if (!segment || !track)
	{
		constexpr const char* logPrefix = "Area Music Offset";
		Logging::Write(logPrefix,
			"Could not restore native offset (segment=%u track=%u not found)",
			backup.segmentId,
			backup.trackId
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return;
	}

	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);

	if (HasWritableMemory(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t)))
	{
		WriteUnaligned<uint32_t>(
			segmentBytes + offsets.segmentDuration,
			backup.origSegmentDurationTicks
		);
	}

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
	);
	const uint32_t markerCount = ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerArray
		&& markerCount == backup.origMarkerPositionTicks.size()
		&& markerCount > 0
		&& HasWritableMemory(
			reinterpret_cast<uintptr_t>(markerArray),
			static_cast<size_t>(markerCount) * liveSegmentMarkerStride
		)
	)
	{
		for (uint32_t i = 0; i < markerCount; ++i)
		{
			WriteUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset,
				backup.origMarkerPositionTicks[i]
			);
		}
	}

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingArray
		&& !backup.origTrackTimingBytes.empty()
		&& static_cast<size_t>(timingCount) * liveTrackTimingStride == backup.origTrackTimingBytes.size()
		&& HasWritableMemory(reinterpret_cast<uintptr_t>(timingArray), backup.origTrackTimingBytes.size())
	)
	{
		std::memcpy(timingArray, backup.origTrackTimingBytes.data(), backup.origTrackTimingBytes.size());
	}

	constexpr const char* logPrefix = "Area Music Offset";
	Logging::Write(logPrefix,
		"Restored native offset (segment=%u track=%u)",
		backup.segmentId,
		backup.trackId
	);

	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
}

void* AreaMusicManager::LookupWwiseObjectInTable(uint32_t objectId, int tableIndex)
{
	WwiseObjectLookupFn lookup = originalWwiseObjectLookupFunc;
	const uintptr_t registryAddress = wwiseObjectRegistryAddress.load(std::memory_order_acquire);
	if (!lookup || !registryAddress)
	{
		return nullptr;
	}

	if (!HasWritableMemory(registryAddress, 0x80))
	{
		return nullptr;
	}

	void* object = nullptr;
	uint32_t exceptionCode = 0;
	if (!LookupWwiseObjectNoUnwind(
		reinterpret_cast<uintptr_t>(lookup),
		reinterpret_cast<void*>(registryAddress),
		objectId,
		tableIndex,
		&object,
		&exceptionCode
	))
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix,
			"Wwise object lookup raised exception 0x%08x for object %u in table %d (registry %p)",
			exceptionCode,
			objectId,
			tableIndex,
			reinterpret_cast<void*>(registryAddress)
		);
		return nullptr;
	}
	return object;
}

void* AreaMusicManager::LookupWwiseObject(uint32_t objectId)
{
	void* object = LookupWwiseObjectInTable(objectId, 0);
	if (object)
	{
		return object;
	}
	return LookupWwiseObjectInTable(objectId, 1);
}

void AreaMusicManager::ClearAreaMusicMetadataPatch()
{
	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	areaMusicMetadataPatch = {};
}

long long AreaMusicManager::GetRegisteredEffectiveDurationMs(const MusicData* data)
{
	if (!UsesOverride(data))
	{
		return data ? data->maxLength : 0;
	}

	const long long sourceDurationMs =
		areaMusicOverrideBuffer && areaMusicOverrideBuffer->durationMs > 0
		? areaMusicOverrideBuffer->durationMs
		: 0;
	if (sourceDurationMs <= 0)
	{
		return data && data->maxLength > 0 ? data->maxLength : 0;
	}

	return (std::max)(1LL, sourceDurationMs);
}

bool AreaMusicManager::PatchAreaMusicHircMetadata(
	uint8_t* hirc,
	size_t hircSize,
	const AreaMusicMetadataPatch* patch
)
{
	if (!hirc || hircSize < sizeof(uint32_t))
	{
		return false;
	}

	auto matchTrack = [&](uint8_t* body, uint32_t bodySize)
	{
		if (bodySize < musicTrackMinSize)
		{
			return false;
		}

		const uint32_t sourcePluginId = ReadUnaligned<uint32_t>(
			body + musicTrackSourcePluginOffset
		);
		const bool supportedSourcePlugin =
			sourcePluginId == areaMusicOverrideSourcePluginId
			|| sourcePluginId == areaMusicOverridePcmSourcePluginId
			|| (patch && sourcePluginId == patch->sourcePluginId);
		const uint8_t sourceType = body[musicTrackSourceTypeOffset];
		const bool supportedSourceType = sourceType == 0 || sourceType == 2;

		if (
			ReadUnaligned<uint32_t>(body) != areaMusicOverrideTrackId
			|| ReadUnaligned<uint32_t>(body + musicTrackSourceCountOffset) != 1
			|| !supportedSourcePlugin
			|| !supportedSourceType
			|| ReadUnaligned<uint32_t>(body + musicTrackSourceIdOffset)
				!= areaMusicOverrideMediaId
			|| ReadUnaligned<uint32_t>(body + musicTrackClipCountOffset) != 1
			|| ReadUnaligned<uint32_t>(body + musicTrackClipTrackIdOffset) != 0
			|| ReadUnaligned<uint32_t>(body + musicTrackClipSourceIdOffset)
				!= areaMusicOverrideMediaId
		)
		{
			return false;
		}

		return true;
	};

	auto matchSegment = [&](uint8_t* body, uint32_t bodySize)
	{
		if (bodySize < musicSegmentMinSize)
		{
			return false;
		}

		return !(
			ReadUnaligned<uint32_t>(body) != areaMusicOverrideSegmentId
			|| ReadUnaligned<uint32_t>(body + musicSegmentRanSeqIdOffset)
				!= areaMusicOverrideRanSeqId
			|| ReadUnaligned<uint32_t>(body + musicSegmentTrackIdOffset)
				!= areaMusicOverrideTrackId
		);
	};

	uint8_t* trackBody = nullptr;
	uint8_t* segmentBody = nullptr;
	const uint32_t itemCount = ReadUnaligned<uint32_t>(hirc);
	size_t itemOffset = sizeof(uint32_t);
	for (
		uint32_t i = 0;
		i < itemCount && itemOffset + hircItemHeaderSize <= hircSize;
		++i
	)
	{
		uint8_t* item = hirc + itemOffset;
		const uint8_t type = item[0];
		const uint32_t bodySize = ReadUnaligned<uint32_t>(item + 1);
		if (bodySize > hircSize - itemOffset - hircItemHeaderSize)
		{
			break;
		}

		uint8_t* body = item + hircItemHeaderSize;
		if (type == 0x0b && !trackBody && matchTrack(body, bodySize))
		{
			trackBody = body;
		}
		else if (type == 0x0a && !segmentBody && matchSegment(body, bodySize))
		{
			segmentBody = body;
		}

		itemOffset += hircItemHeaderSize + bodySize;
	}

	if (!trackBody || !segmentBody)
	{
		return false;
	}

	if (!patch)
	{
		return true;
	}

	const double sourceDurationMs = patch->durationMs > 0
		? static_cast<double>(patch->durationMs)
		: ReadUnaligned<double>(trackBody + musicTrackSourceDurationOffset);
	const double sourceStartMs = static_cast<double>(
		ClampSourceStartOffsetMs(patch->sourceStartMs, static_cast<long long>(sourceDurationMs))
	);
	constexpr double clipPlayAtMs = 0.0;
	const double beginTrimMs = sourceStartMs;
	constexpr double endTrimMs = 0.0;

	if (patch->mediaSize > 0)
	{
		WriteUnaligned<uint32_t>(trackBody + musicTrackMediaSizeOffset, patch->mediaSize);
	}
	WriteUnaligned<uint32_t>(trackBody + musicTrackSourcePluginOffset, patch->sourcePluginId);
	trackBody[musicTrackSourceTypeOffset] = 0;
	WriteUnaligned<uint32_t>(trackBody + musicTrackSourceIdOffset, patch->mediaId);
	WriteUnaligned<uint32_t>(trackBody + musicTrackClipSourceIdOffset, patch->mediaId);
	WriteUnaligned<double>(trackBody + musicTrackClipStartOffset, clipPlayAtMs);
	WriteUnaligned<double>(trackBody + musicTrackClipBeginTrimOffset, beginTrimMs);
	WriteUnaligned<double>(trackBody + musicTrackClipEndTrimOffset, endTrimMs);
	WriteUnaligned<double>(trackBody + musicTrackSourceDurationOffset, sourceDurationMs);
	trackBody[musicTrackSourceBitsOffset] = 0;

	const double originalSegmentDurationMs = ReadUnaligned<double>(
		segmentBody + musicSegmentDurationOffset
	);

	double effectiveDurationMs = patch->durationMs > 0
		? static_cast<double>(patch->durationMs)
		: originalSegmentDurationMs;
	effectiveDurationMs -= sourceStartMs;
	effectiveDurationMs = (std::max)(1.0, effectiveDurationMs);

	WriteUnaligned<double>(segmentBody + musicSegmentDurationOffset, effectiveDurationMs);
	WriteUnaligned<double>(segmentBody + musicSegmentEndMarkerOffset, effectiveDurationMs);
	WriteUnaligned<double>(segmentBody + musicSegmentPostMarkerDurationOffset, effectiveDurationMs);

	return true;
}

bool AreaMusicManager::FindAreaMusicBankMetadata(const uint8_t* bankData, size_t bankSize)
{
	if (!bankData || bankSize < wwiseChunkHeaderSize)
	{
		return false;
	}

	size_t offset = 0;
	while (offset + wwiseChunkHeaderSize <= bankSize)
	{
		const uint32_t chunkSize = ReadUnaligned<uint32_t>(bankData + offset + 4);
		if (chunkSize > bankSize - offset - wwiseChunkHeaderSize)
		{
			break;
		}

		if (std::memcmp(bankData + offset, "HIRC", 4) == 0 && chunkSize >= sizeof(uint32_t))
		{
			uint8_t* hirc = const_cast<uint8_t*>(bankData + offset + wwiseChunkHeaderSize);
			if (PatchAreaMusicHircMetadata(hirc, chunkSize, nullptr))
			{
				return true;
			}
		}

		offset += wwiseChunkHeaderSize + chunkSize;
	}

	return false;
}

bool AreaMusicManager::PatchLiveAreaMusicMetadata(
	const MusicData* data,
	long long fallbackDurationMs,
	long long requestedSourceStartMs
)
{
	if (!data || !data->customAreaTrack)
	{
		return false;
	}

	if (DurationControlledByWwise())
	{
		RestoreLiveAreaMusicMetadata();
	}

	const long long sourceDurationMs = fallbackDurationMs;
	if (sourceDurationMs <= 0)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Cannot patch live Wwise metadata for \"%s\": no duration is known", data->name ? data->name : "");
		return false;
	}

	const long long sourceStartMs = ClampSourceStartOffsetMs(
		requestedSourceStartMs,
		sourceDurationMs
	);
	const long long effectiveDurationMs = (std::max)(
		1LL,
		sourceDurationMs - sourceStartMs
	);
	const uint32_t sourceDurationTicks = MillisecondsToWwiseTicks(sourceDurationMs);
	const uint32_t effectiveDurationTicks = MillisecondsToWwiseTicks(effectiveDurationMs);
	const uint32_t sourceStartTicks = MillisecondsToWwiseTicksAllowZero(sourceStartMs);
	const uint32_t mediaId = areaMusicOverrideMediaId;
	const uint32_t mediaSize = areaMusicOverrideBuffer
		? static_cast<uint32_t>((std::min)(
			areaMusicOverrideBuffer->bytes.size(),
			static_cast<size_t>((std::numeric_limits<uint32_t>::max)())
		))
		: 0;
	const uint32_t sourcePluginId = areaMusicOverrideBuffer
		? areaMusicOverrideBuffer->sourcePluginId
		: areaMusicOverrideSourcePluginId;

	void* segment = LookupWwiseObject(areaMusicOverrideSegmentId);
	void* track = LookupWwiseObject(areaMusicOverrideTrackId);
	if (!segment || !track)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix,
			"Could not find live Wwise music objects for Area00 metadata patch (segment=%p, track=%p)",
			segment,
			track
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	LiveAreaMusicMetadataBackup backup{};
	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();
	if (
		!HasWritableMemory(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t))
		|| !HasWritableMemory(reinterpret_cast<uintptr_t>(trackBytes), offsets.trackSourceDuration + sizeof(uint32_t))
	)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Refusing live Wwise metadata patch: segment or track object memory is not writable");
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
	);
	const uint32_t markerCount = ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerCount > 256
		|| (
			markerArray
			&& markerCount > 0
			&& !HasWritableMemory(
				reinterpret_cast<uintptr_t>(markerArray),
				static_cast<size_t>(markerCount) * liveSegmentMarkerStride
			)
		)
	)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Refusing live Wwise segment patch: marker count %u is unreasonable", markerCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* sourceArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackSourceArray
	);
	const uint32_t sourceCount = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceCount
	);
	if (
		sourceCount > 256
		|| (
			sourceArray
			&& sourceCount > 0
			&& !HasWritableMemory(
				reinterpret_cast<uintptr_t>(sourceArray),
				static_cast<size_t>(sourceCount) * liveTrackSourceStride
			)
		)
	)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Refusing live Wwise track patch: source entry count %u is unreasonable", sourceCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingCount > 256
		|| (
			timingArray
			&& timingCount > 0
			&& !HasWritableMemory(
				reinterpret_cast<uintptr_t>(timingArray),
				static_cast<size_t>(timingCount) * liveTrackTimingStride
			)
		)
	)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Refusing live Wwise track patch: timing entry count %u is unreasonable", timingCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	backup.valid = true;
	backup.segmentDurationTicks = ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration
	);
	if (markerArray)
	{
		backup.markerPositionTicks.reserve(markerCount);
		for (uint32_t i = 0; i < markerCount; ++i)
		{
			backup.markerPositionTicks.push_back(ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			));
		}
	}

	backup.trackSourceEntryCount = sourceCount;
	if (sourceArray && sourceCount > 0)
	{
		const size_t sourceBytes = static_cast<size_t>(sourceCount) * liveTrackSourceStride;
		backup.trackSourceEntryBytes.assign(sourceArray, sourceArray + sourceBytes);
	}

	backup.trackSourceDurationTicks = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration
	);
	backup.trackTimingCount = timingCount;
	if (timingArray && timingCount > 0)
	{
		const size_t timingBytes = static_cast<size_t>(timingCount) * liveTrackTimingStride;
		backup.trackTimingBytes.assign(timingArray, timingArray + timingBytes);
	}

	uint32_t patchedSourceCount = 0;
	if (sourceArray && sourceCount > 0)
	{
		for (uint32_t i = 0; i < sourceCount; ++i)
		{
			uint8_t* entry = sourceArray + (static_cast<size_t>(i) * liveTrackSourceStride);
			const uint32_t sourceKey = ReadUnaligned<uint32_t>(entry);
			if (sourceKey != areaMusicOverrideMediaId && sourceCount != 1)
			{
				continue;
			}

			void* sourceObject = *reinterpret_cast<void**>(entry + liveTrackSourceObjectOffset);
			uint8_t* sourceObjectBytes = static_cast<uint8_t*>(sourceObject);
			if (
				sourceObjectBytes
				&& HasWritableMemory(
					reinterpret_cast<uintptr_t>(sourceObjectBytes),
					liveTrackSourceObjectSize
				)
			)
			{
				LiveAreaMusicMemoryBackup objectBackup{};
				objectBackup.data = sourceObjectBytes;
				objectBackup.bytes.assign(
					sourceObjectBytes,
					sourceObjectBytes + liveTrackSourceObjectSize
				);
				backup.sourceObjectBackups.push_back(std::move(objectBackup));
				WriteUnaligned<uint32_t>(sourceObjectBytes, mediaId);
				WriteUnaligned<uint32_t>(sourceObjectBytes + 0x04, mediaId);
				if (mediaSize > 0)
				{
					WriteUnaligned<uint32_t>(sourceObjectBytes + 0x08, mediaSize);
				}
				WriteUnaligned<uint32_t>(sourceObjectBytes + 0x18, sourcePluginId);
			}

			WriteUnaligned<uint32_t>(entry, mediaId);
			++patchedSourceCount;
		}
	}

	WriteUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration,
		effectiveDurationTicks
	);

	uint32_t patchedMarkerCount = 0;
	if (markerArray && markerCount > 0)
	{
		for (uint32_t i = 0; i < markerCount; ++i)
		{
			const uint32_t markerTicks = ReadUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset
			);
			if (markerTicks > 0 && markerTicks != effectiveDurationTicks)
			{
				WriteUnaligned<uint32_t>(
					markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
					+ liveSegmentMarkerPositionOffset,
					effectiveDurationTicks
				);
				++patchedMarkerCount;
			}
		}
	}

	WriteUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration,
		sourceDurationTicks
	);

	uint32_t patchedTimingCount = 0;
	if (timingArray && timingCount > 0)
	{
		for (uint32_t i = 0; i < timingCount; ++i)
		{
			uint8_t* entry = timingArray + (static_cast<size_t>(i) * liveTrackTimingStride);
			const uint32_t entryId0 = ReadUnaligned<uint32_t>(entry);
			const uint32_t entryId1 = ReadUnaligned<uint32_t>(entry + sizeof(uint32_t));
			if (
				entryId0 != areaMusicOverrideMediaId
				&& entryId1 != areaMusicOverrideMediaId
				&& timingCount != 1
			)
			{
				continue;
			}

			if (entryId0 == areaMusicOverrideMediaId)
			{
				WriteUnaligned<uint32_t>(entry, mediaId);
			}
			if (entryId1 == areaMusicOverrideMediaId || timingCount == 1)
			{
				WriteUnaligned<uint32_t>(entry + sizeof(uint32_t), mediaId);
			}
			WriteUnaligned<uint32_t>(entry + 0x0c, 0);
			WriteUnaligned<uint32_t>(entry + 0x10, effectiveDurationTicks);
			WriteUnaligned<uint32_t>(entry + 0x14, sourceDurationTicks);
			WriteUnaligned<uint32_t>(entry + 0x18, sourceStartTicks);
			++patchedTimingCount;
		}
	}

	if (patchedSourceCount == 0 || patchedTimingCount == 0)
	{
		if (sourceArray && !backup.trackSourceEntryBytes.empty())
		{
			std::memcpy(sourceArray, backup.trackSourceEntryBytes.data(), backup.trackSourceEntryBytes.size());
		}
		for (const LiveAreaMusicMemoryBackup& objectBackup : backup.sourceObjectBackups)
		{
			if (
				objectBackup.data
				&& !objectBackup.bytes.empty()
				&& HasWritableMemory(
					reinterpret_cast<uintptr_t>(objectBackup.data),
					objectBackup.bytes.size()
				)
			)
			{
				std::memcpy(objectBackup.data, objectBackup.bytes.data(), objectBackup.bytes.size());
			}
		}
		WriteUnaligned<uint32_t>(
			segmentBytes + offsets.segmentDuration,
			backup.segmentDurationTicks
		);
		if (markerArray && markerCount == backup.markerPositionTicks.size())
		{
			for (uint32_t i = 0; i < markerCount; ++i)
			{
				WriteUnaligned<uint32_t>(
					markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
					+ liveSegmentMarkerPositionOffset,
					backup.markerPositionTicks[i]
				);
			}
		}
		WriteUnaligned<uint32_t>(
			trackBytes + offsets.trackSourceDuration,
			backup.trackSourceDurationTicks
		);
		if (timingArray && !backup.trackTimingBytes.empty())
		{
			std::memcpy(timingArray, backup.trackTimingBytes.data(), backup.trackTimingBytes.size());
		}

		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix,
			"Could not find target source %u in live Wwise track metadata for \"%s\" (source entries %u/%u, timing entries %u/%u)",
			areaMusicOverrideMediaId,
			data->name ? data->name : "",
			patchedSourceCount,
			sourceCount,
			patchedTimingCount,
			timingCount
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		areaMusicOverrideMetadataPatched = false;
		return false;
	}

	liveAreaMusicMetadataBackup = std::move(backup);
	areaMusicOverrideMetadataPatched = true;

	constexpr const char* logPrefix = "Area Music Metadata";
	Logging::Write(logPrefix,
		"Patched live Area00 Wwise music metadata for \"%s\" (media id %u, source %lld ms/%u ticks, effective %lld ms/%u ticks, source start %lld ms/%u ticks, neutral trim fields, sources %u/%u, markers %u/%u, timing entries %u/%u)",
		data->name ? data->name : "",
		mediaId,
		sourceDurationMs,
		sourceDurationTicks,
		effectiveDurationMs,
		effectiveDurationTicks,
		sourceStartMs,
		sourceStartTicks,
		patchedSourceCount,
		sourceCount,
		patchedMarkerCount,
		markerCount,
		patchedTimingCount,
		timingCount
	);

	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
	return areaMusicOverrideMetadataPatched;
}

void AreaMusicManager::RestoreLiveAreaMusicMetadata()
{
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		if (!liveAreaMusicMetadataBackup.valid)
		{
			areaMusicOverrideMetadataPatched = false;
			return;
		}
	}

	LiveAreaMusicMetadataBackup backup{};
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		backup = liveAreaMusicMetadataBackup;
	}

	void* segment = LookupWwiseObject(areaMusicOverrideSegmentId);
	void* track = LookupWwiseObject(areaMusicOverrideTrackId);
	if (!segment || !track)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix,
			"Could not find live Wwise music objects while restoring Area00 metadata (segment=%p, track=%p)",
			segment,
			track
		);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		{
			std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
			liveAreaMusicMetadataBackup = {};
			areaMusicOverrideMetadataPatched = false;
		}
		return;
	}

	uint8_t* segmentBytes = static_cast<uint8_t*>(segment);
	uint8_t* trackBytes = static_cast<uint8_t*>(track);
	const LiveWwiseOffsets& offsets = GetLiveWwiseOffsets();
	if (
		!HasWritableMemory(reinterpret_cast<uintptr_t>(segmentBytes), offsets.segmentDuration + sizeof(uint32_t))
		|| !HasWritableMemory(reinterpret_cast<uintptr_t>(trackBytes), offsets.trackSourceDuration + sizeof(uint32_t))
	)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Refusing live Wwise metadata restore: segment or track object memory is not writable");
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		{
			std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
			liveAreaMusicMetadataBackup = {};
			areaMusicOverrideMetadataPatched = false;
		}
		return;
	}

	WriteUnaligned<uint32_t>(
		segmentBytes + offsets.segmentDuration,
		backup.segmentDurationTicks
	);

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + offsets.segmentMarkerArray
	);
	const uint32_t markerCount = ReadUnaligned<uint32_t>(
		segmentBytes + offsets.segmentMarkerCount
	);
	if (
		markerArray
		&& markerCount == backup.markerPositionTicks.size()
		&& HasWritableMemory(
			reinterpret_cast<uintptr_t>(markerArray),
			static_cast<size_t>(markerCount) * liveSegmentMarkerStride
		)
	)
	{
		for (uint32_t i = 0; i < markerCount; ++i)
		{
			WriteUnaligned<uint32_t>(
				markerArray + (static_cast<size_t>(i) * liveSegmentMarkerStride)
				+ liveSegmentMarkerPositionOffset,
				backup.markerPositionTicks[i]
			);
		}
	}

	uint8_t* sourceArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackSourceArray
	);
	const uint32_t sourceCount = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceCount
	);
	if (
		sourceArray
		&& sourceCount == backup.trackSourceEntryCount
		&& !backup.trackSourceEntryBytes.empty()
		&& HasWritableMemory(
			reinterpret_cast<uintptr_t>(sourceArray),
			backup.trackSourceEntryBytes.size()
		)
	)
	{
		std::memcpy(
			sourceArray,
			backup.trackSourceEntryBytes.data(),
			backup.trackSourceEntryBytes.size()
		);
	}

	for (const LiveAreaMusicMemoryBackup& objectBackup : backup.sourceObjectBackups)
	{
		if (
			objectBackup.data
			&& !objectBackup.bytes.empty()
			&& HasWritableMemory(
				reinterpret_cast<uintptr_t>(objectBackup.data),
				objectBackup.bytes.size()
			)
		)
		{
			std::memcpy(objectBackup.data, objectBackup.bytes.data(), objectBackup.bytes.size());
		}
	}

	WriteUnaligned<uint32_t>(
		trackBytes + offsets.trackSourceDuration,
		backup.trackSourceDurationTicks
	);

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + offsets.trackTimingArray
	);
	const uint32_t timingCount = ReadUnaligned<uint32_t>(
		trackBytes + offsets.trackTimingCount
	);
	if (
		timingArray
		&& timingCount == backup.trackTimingCount
		&& !backup.trackTimingBytes.empty()
		&& HasWritableMemory(
			reinterpret_cast<uintptr_t>(timingArray),
			backup.trackTimingBytes.size()
		)
	)
	{
		std::memcpy(
			timingArray,
			backup.trackTimingBytes.data(),
			backup.trackTimingBytes.size()
		);
	}

	constexpr const char* logPrefix = "Area Music Metadata";
	Logging::Write(logPrefix, "Restored live Area00 Wwise music metadata");
	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		liveAreaMusicMetadataBackup = {};
		areaMusicOverrideMetadataPatched = false;
	}
}

void AreaMusicManager::RememberAreaMusicBankMemory(
	const void* bankData,
	size_t bankSize,
	uint32_t bankId,
	const char* source
)
{
	if (!bankData || bankSize < wwiseChunkHeaderSize)
	{
		return;
	}

	uint8_t* writableBankData = const_cast<uint8_t*>(
		static_cast<const uint8_t*>(bankData)
	);

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	for (const AreaMusicBankMemoryView& view : areaMusicBankMemoryViews)
	{
		if (view.data == writableBankData && view.size == bankSize)
		{
			const_cast<AreaMusicBankMemoryView&>(view).bankId = bankId ? bankId : view.bankId;
			return;
		}
	}

	AreaMusicBankMemoryView view{};
	view.data = writableBankData;
	view.originalData = writableBankData;
	view.size = bankSize;
	view.bankId = bankId;
	view.originalBytes.assign(writableBankData, writableBankData + bankSize);
	areaMusicBankMemoryViews.push_back(std::move(view));
	constexpr const char* logPrefix = "Area Music Metadata";
	Logging::Write(logPrefix,
		"Remembered Area00 Wwise bank memory from %s at %p (%zu bytes, bank id %u)",
		source ? source : "unknown",
		writableBankData,
		bankSize,
		bankId
	);

	// Walk the HIRC chunk to collect all music-related item IDs (segment / track / switch-cntr / ranseq). The set lets
	// us filter WwiseObjectLookup logging down to just music lookups, so when the game looks up Half Asleep's chain
	// during PlayMusic we see segment / track / ranseq IDs in the log without drowning in unrelated lookups.
	{
		size_t offset = 0;
		std::unordered_map<uint32_t, uint8_t> ids;
		while (offset + wwiseChunkHeaderSize <= bankSize)
		{
			const uint32_t chunkSize = ReadUnaligned<uint32_t>(writableBankData + offset + 4);
			if (chunkSize > bankSize - offset - wwiseChunkHeaderSize)
			{
				break;
			}
			if (std::memcmp(writableBankData + offset, "HIRC", 4) == 0)
			{
				uint8_t* hirc = writableBankData + offset + wwiseChunkHeaderSize;
				if (chunkSize >= sizeof(uint32_t))
				{
					const uint32_t itemCount = ReadUnaligned<uint32_t>(hirc);
					size_t itemOff = sizeof(uint32_t);
					for (uint32_t i = 0; i < itemCount && itemOff + hircItemHeaderSize <= chunkSize; ++i)
					{
						const uint8_t type = hirc[itemOff];
						const uint32_t bodySize = ReadUnaligned<uint32_t>(hirc + itemOff + 1);
						if (bodySize > chunkSize - itemOff - hircItemHeaderSize)
						{
							break;
						}
						if ((type == 0x0a || type == 0x0b || type == 0x0c || type == 0x0d)
							&& bodySize >= sizeof(uint32_t))
						{
							const uint32_t id = ReadUnaligned<uint32_t>(hirc + itemOff + hircItemHeaderSize);
							ids[id] = type;
						}
						itemOff += hircItemHeaderSize + bodySize;
					}
				}
				break;
			}
			offset += wwiseChunkHeaderSize + chunkSize;
		}

		if (!ids.empty())
		{
			std::lock_guard<std::mutex> idLock(musicIdSetMutex);
			if (musicHircIdToType.empty())
			{
				musicHircIdToType = std::move(ids);
				Logging::Write(logPrefix,
					"Indexed %zu music HIRC items (segment/track/switch/ranseq) from bank",
					musicHircIdToType.size()
				);
			}
		}
	}
}

void AreaMusicManager::RestoreAreaMusicBankMetadata()
{
	RestoreLiveAreaMusicMetadata();
}

bool AreaMusicManager::ReloadAreaMusicBankMetadata(bool patched)
{
	AreaMusicMetadataPatch patch{};
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		patch = areaMusicMetadataPatch;
	}

	if (patched && !patch.enabled)
	{
		return false;
	}

	AkLoadBankMemoryFn loadBank = originalLoadBankMemoryViewFunc
		? originalLoadBankMemoryViewFunc
		: loadBankMemoryViewFunc;
	if (!loadBank || !unloadBankMemoryFunc)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix,
			"Cannot reload Area00 Wwise bank metadata: LoadBankMemoryView=%p UnloadBank=%p",
			reinterpret_cast<void*>(loadBank),
			reinterpret_cast<void*>(unloadBankMemoryFunc)
		);
		return false;
	}

	auto patchBankBytes = [&](std::vector<uint8_t>& bankBytes)
	{
		if (!patched)
		{
			return true;
		}

		bool patchedBank = false;
		size_t offset = 0;
		while (offset + wwiseChunkHeaderSize <= bankBytes.size())
		{
			const uint32_t chunkSize = ReadUnaligned<uint32_t>(
				bankBytes.data() + offset + 4
			);
			if (chunkSize > bankBytes.size() - offset - wwiseChunkHeaderSize)
			{
				break;
			}

			if (
				std::memcmp(bankBytes.data() + offset, "HIRC", 4) == 0
				&& chunkSize >= sizeof(uint32_t)
			)
			{
				uint8_t* hirc = bankBytes.data() + offset + wwiseChunkHeaderSize;
				patchedBank = PatchAreaMusicHircMetadata(hirc, chunkSize, &patch) || patchedBank;
			}

			offset += wwiseChunkHeaderSize + chunkSize;
		}

		return patchedBank;
	};

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	for (AreaMusicBankMemoryView& view : areaMusicBankMemoryViews)
	{
		if (view.originalBytes.empty() || view.size < wwiseChunkHeaderSize)
		{
			continue;
		}

		if (!patched && !view.loadedPatched)
		{
			areaMusicOverrideMetadataPatched = false;
			return true;
		}

		std::vector<uint8_t> bankBytes = view.originalBytes;
		if (!patchBankBytes(bankBytes))
		{
			continue;
		}

		if (view.bankId && view.data)
		{
			const uint32_t unloadResult = unloadBankMemoryFunc(view.bankId, view.data);
			if (unloadResult != akSuccess)
			{
				constexpr const char* logPrefix = "Area Music Metadata";
				Logging::Write(logPrefix,
					"AK::SoundEngine::UnloadBank failed for Area00 bank %u at %p with result %u",
					view.bankId,
					view.data,
					unloadResult
				);
				return false;
			}
		}

		auto ownedBank = std::make_shared<std::vector<uint8_t>>(std::move(bankBytes));
		unsigned long newBankId = 0;
		const uint32_t loadResult = loadBank(
			ownedBank->data(),
			static_cast<unsigned long>(ownedBank->size()),
			&newBankId
		);
		if (loadResult != akSuccess)
		{
			constexpr const char* logPrefix = "Area Music Metadata";
			Logging::Write(logPrefix,
				"AK::SoundEngine::LoadBankMemoryView failed while reloading %s Area00 metadata with result %u",
				patched ? "patched" : "original",
				loadResult
			);
			if (patched)
			{
				auto restoreBank = std::make_shared<std::vector<uint8_t>>(view.originalBytes);
				unsigned long restoreBankId = 0;
				const uint32_t restoreResult = loadBank(
					restoreBank->data(),
					static_cast<unsigned long>(restoreBank->size()),
					&restoreBankId
				);
				if (restoreResult == akSuccess)
				{
					view.data = restoreBank->data();
					view.size = restoreBank->size();
					view.bankId = static_cast<uint32_t>(restoreBankId);
					view.loadedPatched = false;
					view.ownedBytes = std::move(restoreBank);
					areaMusicOverrideMetadataPatched = false;
					Logging::Write(logPrefix,
						"Restored original Area00 Wwise bank after patched reload failure (bank id %u)",
						view.bankId
					);
				}
			}
			return false;
		}

		view.data = ownedBank->data();
		view.size = ownedBank->size();
		view.bankId = static_cast<uint32_t>(newBankId);
		view.loadedPatched = patched;
		view.ownedBytes = std::move(ownedBank);
		areaMusicOverrideMetadataPatched = patched;

		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix,
			"Reloaded Area00 Wwise bank with %s metadata at %p (%zu bytes, bank id %u)",
			patched ? "patched" : "original",
			view.data,
			view.size,
			view.bankId
		);
		return true;
	}

	constexpr const char* logPrefix = "Area Music Metadata";
	Logging::Write(logPrefix,
		"Could not find remembered Area00 Wwise bank memory to reload %s metadata",
		patched ? "patched" : "original"
	);
	return false;
}

uint32_t __cdecl AreaMusicManager::SetStateHook(uint32_t stateGroup, uint32_t stateValue)
{
	// Only capture and log music state group
	if (stateGroup == areaMusicStateGroupId)
	{
		const long long timestampMs =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()
			).count();

		bool changed = false;
		{
			std::lock_guard<std::mutex> lock(lastSetStateMutex);
			if (
				!lastSetStateObservation.valid
				|| lastSetStateObservation.stateGroup != stateGroup
				|| lastSetStateObservation.stateValue != stateValue
			)
			{
				changed = true;
			}
			lastSetStateObservation = LastSetStateObservation{
				stateGroup,
				stateValue,
				timestampMs,
				true
			};
		}

		if (changed)
		{
			constexpr const char* logPrefix = "Area Music State";
			Logging::Write(logPrefix,
				"SetState(group=0x%08X, value=0x%08X)",
				stateGroup,
				stateValue
			);
		}
	}

	if (!originalSetStateFunc)
	{
		return 0;
	}
	return originalSetStateFunc(stateGroup, stateValue);
}

AreaMusicManager::LastSetStateObservation AreaMusicManager::GetLastSetStateObservation()
{
	std::lock_guard<std::mutex> lock(lastSetStateMutex);
	return lastSetStateObservation;
}

uint32_t __cdecl AreaMusicManager::LoadBankMemoryCopyHook(
	const void* bankData,
	unsigned long bankSize,
	unsigned long* outBankId
)
{
	if (!originalLoadBankMemoryCopyFunc)
	{
		return 0;
	}

	const bool containsAreaMusic =
		bankData
		&& bankSize > 0
		&& FindAreaMusicBankMetadata(
			static_cast<const uint8_t*>(bankData),
			static_cast<size_t>(bankSize)
		);
	if (containsAreaMusic)
	{
		constexpr const char* logPrefix = "Area Music Metadata";
		Logging::Write(logPrefix, "Observed Area00 Wwise bank during LoadBankMemoryCopy");
	}

	return originalLoadBankMemoryCopyFunc(bankData, bankSize, outBankId);
}

uint32_t __cdecl AreaMusicManager::LoadBankMemoryViewHook(
	const void* bankData,
	unsigned long bankSize,
	unsigned long* outBankId
)
{
	if (!originalLoadBankMemoryViewFunc)
	{
		return 0;
	}

	const bool containsAreaMusic =
		bankData
		&& bankSize > 0
		&& FindAreaMusicBankMetadata(
			static_cast<const uint8_t*>(bankData),
			static_cast<size_t>(bankSize)
		);

	const uint32_t result = originalLoadBankMemoryViewFunc(bankData, bankSize, outBankId);
	if (containsAreaMusic && result == akSuccess)
	{
		RememberAreaMusicBankMemory(
			bankData,
			bankSize,
			outBankId ? static_cast<uint32_t>(*outBankId) : 0,
			"LoadBankMemoryView"
		);
	}
	return result;
}

bool AreaMusicManager::ResolveWwiseMediaFunctions()
{
	if (wwiseMediaFunctionsResolved)
	{
		return setMediaFunc != nullptr;
	}

	wwiseMediaFunctionsResolved = true;
	HMODULE mainModule = GetModuleHandleA(nullptr);
	if (!mainModule)
	{
		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix, "Failed to get main module handle for Wwise media exports");
		return false;
	}

	setMediaFunc = reinterpret_cast<AkSetMediaFn>(
		GetProcAddress(mainModule, setMediaExportName)
	);
	unsetMediaFunc = reinterpret_cast<AkUnsetMediaFn>(
		GetProcAddress(mainModule, unsetMediaExportName)
	);

	if (!setMediaFunc)
	{
		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix, "Failed to resolve AK::SoundEngine::SetMedia export");
		return false;
	}
	if (!unsetMediaFunc)
	{
		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix, "AK::SoundEngine::UnsetMedia export was not resolved; cleanup will be skipped");
	}

	constexpr const char* logPrefix = "Area Music Override";
	Logging::Write(logPrefix,
		"Resolved Wwise SetMedia at %p and UnsetMedia at %p",
		reinterpret_cast<void*>(setMediaFunc),
		reinterpret_cast<void*>(unsetMediaFunc)
	);
	return true;
}

bool AreaMusicManager::LoadAreaMusicManager(const char* overridePath)
{
	if (!overridePath || !overridePath[0])
	{
		return false;
	}

	const std::string path = overridePath;
	if (
		areaMusicOverrideBuffer
		&& !areaMusicOverrideBuffer->bytes.empty()
		&& areaMusicOverrideBuffer->path == path
	)
	{
		return true;
	}

	if (areaMusicOverrideRegistered)
	{
		Unset();
	}
	RetireBuffer();

	auto retiredIt = std::find_if(
		retiredAreaMusicManagerBuffers.begin(),
		retiredAreaMusicManagerBuffers.end(),
		[&path](const std::unique_ptr<AreaMusicManagerBuffer>& buffer)
		{
			return buffer
				&& !buffer->bytes.empty()
				&& buffer->path == path;
		}
	);
	if (retiredIt != retiredAreaMusicManagerBuffers.end())
	{
		areaMusicOverrideBuffer = std::move(*retiredIt);
		retiredAreaMusicManagerBuffers.erase(retiredIt);

		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix,
			"Reusing retired area music audio buffer for %s (%zu bytes, source plugin 0x%08x, duration %lld ms)",
			path.c_str(),
			areaMusicOverrideBuffer->bytes.size(),
			areaMusicOverrideBuffer->sourcePluginId,
			areaMusicOverrideBuffer->durationMs
		);
		return true;
	}

	AudioDecoder::WwiseMediaBuffer media{};
	if (!AudioDecoder::LoadWwiseMedia(path, media))
	{
		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix, "Failed to load custom audio override as Wwise media: %s", path.c_str());
		return false;
	}

	areaMusicOverrideBuffer = std::make_unique<AreaMusicManagerBuffer>();
	areaMusicOverrideBuffer->path = path;
	areaMusicOverrideBuffer->bytes = std::move(media.bytes);
	areaMusicOverrideBuffer->sourcePluginId = media.sourcePluginId;
	areaMusicOverrideBuffer->durationMs = media.durationMs;
	constexpr const char* logPrefix = "Area Music Override";
	Logging::Write(logPrefix,
		"Loaded area music audio override from %s (%zu bytes, source plugin 0x%08x, duration %lld ms)",
		path.c_str(),
		areaMusicOverrideBuffer->bytes.size(),
		areaMusicOverrideBuffer->sourcePluginId,
		areaMusicOverrideBuffer->durationMs
	);
	return true;
}

bool AreaMusicManager::Register(const MusicData* data)
{
	if (!UsesOverride(data))
	{
		return true;
	}

	const char* overridePath = data->customWemPath;
	const uint32_t mediaId = areaMusicOverrideMediaId;
	if (!ResolveWwiseMediaFunctions() || !LoadAreaMusicManager(overridePath))
	{
		return false;
	}

	if (!areaMusicOverrideBuffer || areaMusicOverrideBuffer->bytes.empty())
	{
		return false;
	}

	const long long sourceDurationMs =
		areaMusicOverrideBuffer && areaMusicOverrideBuffer->durationMs > 0
		? areaMusicOverrideBuffer->durationMs
		: 0;
	const long long sourceStartMs = ClampSourceStartOffsetMs(
		nextPlaybackStartOffsetMs.exchange(0),
		sourceDurationMs
	);
	const bool canPatchMetadata = sourceDurationMs > 0;
	if (canPatchMetadata)
	{
		AreaMusicMetadataPatch patch{};
		patch.enabled = true;
		patch.durationMs = sourceDurationMs;
		patch.sourceStartMs = sourceStartMs;
		patch.mediaId = areaMusicOverrideMediaId;
		patch.mediaSize = static_cast<uint32_t>(areaMusicOverrideBuffer->bytes.size());
		patch.sourcePluginId = areaMusicOverrideBuffer->sourcePluginId;
		{
			std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
			areaMusicMetadataPatch = patch;
		}

		areaMusicOverrideMetadataPatched = PatchLiveAreaMusicMetadata(
			data,
			sourceDurationMs,
			sourceStartMs
		);
	}
	else
	{
		ClearAreaMusicMetadataPatch();
		areaMusicOverrideMetadataPatched = false;
	}
	constexpr const char* metadataLogPrefix = "Area Music Metadata";
	Logging::Write(metadataLogPrefix,
		"Area00 live metadata patch %s for \"%s\" (media %zu bytes, source plugin 0x%08x, duration %lld ms, source start %lld ms)",
		canPatchMetadata
			? (areaMusicOverrideMetadataPatched ? "applied" : "not applied")
			: "skipped because source duration is unknown",
		data->name ? data->name : "",
		areaMusicOverrideBuffer->bytes.size(),
		areaMusicOverrideBuffer->sourcePluginId,
		sourceDurationMs,
		sourceStartMs
	);

	if (areaMusicOverrideRegistered)
	{
		return true;
	}

	AkSourceSettings media{};
	media.sourceId = mediaId;
	media.mediaMemory = areaMusicOverrideBuffer->bytes.data();
	media.mediaSize = static_cast<uint32_t>(areaMusicOverrideBuffer->bytes.size());

	uint32_t result = setMediaFunc(&media, 1);
	if (result != akSuccess)
	{
		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix,
			"AK::SoundEngine::SetMedia failed for source %u with result %u",
			mediaId,
			result
		);
		if (areaMusicOverrideMetadataPatched)
		{
			RestoreAreaMusicBankMetadata();
		}
		ClearAreaMusicMetadataPatch();
		areaMusicOverrideMetadataPatched = false;
		return false;
	}

	areaMusicOverrideRegistered = true;
	areaMusicOverrideRegisteredMediaId = mediaId;
	constexpr const char* logPrefix = "Area Music Override";
	Logging::Write(logPrefix,
		"Registered area music audio override for Wwise source %u from %s (source plugin 0x%08x)",
		mediaId,
		overridePath,
		areaMusicOverrideBuffer->sourcePluginId
	);
	return true;
}

void AreaMusicManager::Unset()
{
	if (!areaMusicOverrideRegistered)
	{
		if (areaMusicOverrideMetadataPatched)
		{
			RestoreAreaMusicBankMetadata();
		}
		areaMusicOverrideMetadataPatched = false;
		ClearAreaMusicMetadataPatch();
		return;
	}

	if (!ResolveWwiseMediaFunctions() || !unsetMediaFunc)
	{
		if (areaMusicOverrideMetadataPatched)
		{
			RestoreAreaMusicBankMetadata();
		}
		areaMusicOverrideRegistered = false;
		areaMusicOverrideRegisteredMediaId = 0;
		areaMusicOverrideMetadataPatched = false;
		ClearAreaMusicMetadataPatch();
		return;
	}

	AkSourceSettings media{};
	media.sourceId = areaMusicOverrideRegisteredMediaId
		? areaMusicOverrideRegisteredMediaId
		: areaMusicOverrideMediaId;
	if (areaMusicOverrideBuffer && !areaMusicOverrideBuffer->bytes.empty())
	{
		media.mediaMemory = areaMusicOverrideBuffer->bytes.data();
		media.mediaSize = static_cast<uint32_t>(areaMusicOverrideBuffer->bytes.size());
	}

	uint32_t result = unsetMediaFunc(&media, 1);
	if (result != akSuccess)
	{
		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix,
			"AK::SoundEngine::UnsetMedia failed for source %u with result %u",
			media.sourceId,
			result
		);
	}
	else
	{
		constexpr const char* logPrefix = "Area Music Override";
		Logging::Write(logPrefix, "Unregistered area music audio override for Wwise source %u", media.sourceId);
	}
	areaMusicOverrideRegistered = false;
	areaMusicOverrideRegisteredMediaId = 0;
	if (areaMusicOverrideMetadataPatched)
	{
		RestoreAreaMusicBankMetadata();
	}
	areaMusicOverrideMetadataPatched = false;
	ClearAreaMusicMetadataPatch();
}

void AreaMusicManager::RetireBuffer()
{
	if (!areaMusicOverrideBuffer || areaMusicOverrideBuffer->bytes.empty())
	{
		return;
	}

	constexpr const char* logPrefix = "Area Music Override";
	Logging::Write(logPrefix,
		"Keeping retired area music audio buffer alive for %s (%zu bytes)",
		areaMusicOverrideBuffer->path.c_str(),
		areaMusicOverrideBuffer->bytes.size()
	);
	retiredAreaMusicManagerBuffers.push_back(std::move(areaMusicOverrideBuffer));
}
