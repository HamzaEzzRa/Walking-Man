#include "AreaMusicOverride.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <Windows.h>

#include "AudioDecoder.h"
#include "GameData.h"
#include "Logger.h"
#include "MinHook.h"

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

	constexpr uintptr_t wwiseObjectRegistryRva = 0x7e9b5d8;
	constexpr uintptr_t wwiseLookupObjectRva = 0x3a37b60;
	constexpr size_t wwiseObjectReleaseVtableOffset = 0x10;
	constexpr size_t liveSegmentMarkerArrayOffset = 0x110;
	constexpr size_t liveSegmentMarkerCountOffset = 0x118;
	constexpr size_t liveSegmentDurationOffset = 0x120;
	constexpr size_t liveSegmentMarkerStride = 0x10;
	constexpr size_t liveSegmentMarkerPositionOffset = 0x04;
	constexpr size_t liveTrackSourceArrayOffset = 0xd8;
	constexpr size_t liveTrackSourceCountOffset = 0xe0;
	constexpr size_t liveTrackSourceStride = 0x10;
	constexpr size_t liveTrackSourceObjectOffset = 0x08;
	constexpr size_t liveTrackSourceObjectSize = 0x28;
	constexpr size_t liveTrackTimingArrayOffset = 0x100;
	constexpr size_t liveTrackTimingCountOffset = 0x108;
	constexpr size_t liveTrackSourceDurationOffset = 0x110;
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

	uintptr_t MainModuleAddress(uintptr_t rva)
	{
		HMODULE module = GetModuleHandleA(nullptr);
		if (!module)
		{
			return 0;
		}

		return reinterpret_cast<uintptr_t>(module) + rva;
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
				Logger logger("Area Music Metadata");
				logger.Log(
					"Wwise object release raised exception 0x%08x for object %p",
					exceptionCode,
					object
				);
			}
		}
	}

	void* LookupWwiseObjectInTable(uint32_t objectId, int tableIndex)
	{
		using LookupFn = void* (__cdecl*)(void*, uint32_t, int);

		const uintptr_t registryGlobalAddress = MainModuleAddress(wwiseObjectRegistryRva);
		const uintptr_t lookupAddress = MainModuleAddress(wwiseLookupObjectRva);
		if (
			!HasReadableMemory(
				registryGlobalAddress,
				sizeof(uintptr_t)
			)
			|| !HasExecutableMemory(
				lookupAddress,
				16
			)
		)
		{
			return nullptr;
		}

		const uintptr_t registryAddress = *reinterpret_cast<const uintptr_t*>(registryGlobalAddress);
		if (!HasWritableMemory(registryAddress, 0x80))
		{
			return nullptr;
		}

		auto lookup = reinterpret_cast<LookupFn>(lookupAddress);
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
			Logger logger("Area Music Metadata");
			logger.Log(
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

	void* LookupWwiseObject(uint32_t objectId)
	{
		void* object = LookupWwiseObjectInTable(objectId, 0);
		if (object)
		{
			return object;
		}
		return LookupWwiseObjectInTable(objectId, 1);
	}
}

bool AreaMusicOverride::UsesOverride(const MusicData* data)
{
	return data && data->customAreaTrack && data->customWemPath;
}

bool AreaMusicOverride::IsTemplateTrack(const MusicData* data)
{
	return UsesOverride(data)
		|| (
			data
			&& data->name
			&& std::strcmp(data->name, TargetTrackName) == 0
		);
}

bool AreaMusicOverride::DurationControlledByWwise()
{
	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	return areaMusicOverrideMetadataPatched;
}

uint32_t AreaMusicOverride::GetAreaMusicOverrideMediaId(const MusicData* data)
{
	(void)data;
	return areaMusicOverrideMediaId;
}

const char* AreaMusicOverride::GetAreaMusicOverridePath(const MusicData* data)
{
	if (data && data->customAreaTrack && data->customWemPath)
	{
		return data->customWemPath;
	}
	return nullptr;
}

void AreaMusicOverride::Initialize()
{
	if (areaMusicMetadataHooksInitialized)
	{
		return;
	}

	areaMusicMetadataHooksInitialized = true;

	HMODULE mainModule = GetModuleHandleA(nullptr);
	if (!mainModule)
	{
		Logger logger("Area Music Metadata");
		logger.Log("Failed to get main module handle for Wwise bank hooks");
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

	auto installHook = [](AkLoadBankMemoryFn target, void* hook, void** original, const char* name)
	{
		Logger logger("Area Music Metadata");
		if (!target)
		{
			logger.Log("Wwise %s export was not resolved", name);
			return;
		}

		MH_STATUS created = MH_CreateHook(reinterpret_cast<LPVOID>(target), hook, original);
		if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
		{
			logger.Log("Failed to create Wwise %s hook: %d", name, created);
			return;
		}

		MH_STATUS enabled = MH_EnableHook(reinterpret_cast<LPVOID>(target));
		if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
		{
			logger.Log("Failed to enable Wwise %s hook: %d", name, enabled);
			return;
		}

		logger.Log("Installed Wwise %s hook at %p", name, reinterpret_cast<void*>(target));
	};

	installHook(
		loadBankMemoryCopyFunc,
		reinterpret_cast<void*>(&AreaMusicOverride::LoadBankMemoryCopyHook),
		reinterpret_cast<void**>(&originalLoadBankMemoryCopyFunc),
		"LoadBankMemoryCopy"
	);
	installHook(
		loadBankMemoryViewFunc,
		reinterpret_cast<void*>(&AreaMusicOverride::LoadBankMemoryViewHook),
		reinterpret_cast<void**>(&originalLoadBankMemoryViewFunc),
		"LoadBankMemoryView"
	);
}

void AreaMusicOverride::ClearAreaMusicMetadataPatch()
{
	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	areaMusicMetadataPatch = {};
}

long long AreaMusicOverride::GetRegisteredSourceDurationMs(const MusicData* data)
{
	(void)data;
	if (areaMusicOverrideBuffer && areaMusicOverrideBuffer->durationMs > 0)
	{
		return areaMusicOverrideBuffer->durationMs;
	}

	return 0;
}

long long AreaMusicOverride::CalculateEffectiveDurationMs(
	long long sourceDurationMs,
	const MusicData* data
)
{
	if (sourceDurationMs <= 0)
	{
		return data && data->maxLength > 0 ? data->maxLength : 0;
	}

	return (std::max)(1LL, sourceDurationMs);
}

long long AreaMusicOverride::GetRegisteredEffectiveDurationMs(const MusicData* data)
{
	if (!UsesOverride(data))
	{
		return data ? data->maxLength : 0;
	}

	return CalculateEffectiveDurationMs(GetRegisteredSourceDurationMs(data), data);
}

void AreaMusicOverride::PrepareAreaMusicMetadataPatch(
	const MusicData* data,
	uint32_t mediaSize,
	uint32_t sourcePluginId,
	long long fallbackDurationMs
)
{
	AreaMusicMetadataPatch patch{};
	if (
		data
		&& data->customAreaTrack
		&& mediaSize > 0
	)
	{
		patch.enabled = true;
		patch.durationMs = fallbackDurationMs;
		patch.mediaId = GetAreaMusicOverrideMediaId(data);
		patch.mediaSize = mediaSize;
		patch.sourcePluginId = sourcePluginId;
	}

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	areaMusicMetadataPatch = patch;
}

bool AreaMusicOverride::PatchAreaMusicHircMetadata(
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
	// Wwise MusicClip defaults: PlayAt, BeginTrimOffset, and EndTrimOffset are all 0.
	constexpr double clipPlayAtMs = 0.0;
	constexpr double beginTrimMs = 0.0;
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
	effectiveDurationMs = (std::max)(1.0, effectiveDurationMs);

	WriteUnaligned<double>(segmentBody + musicSegmentDurationOffset, effectiveDurationMs);
	WriteUnaligned<double>(segmentBody + musicSegmentEndMarkerOffset, effectiveDurationMs);
	WriteUnaligned<double>(segmentBody + musicSegmentPostMarkerDurationOffset, effectiveDurationMs);

	return true;
}

bool AreaMusicOverride::FindAreaMusicBankMetadata(const uint8_t* bankData, size_t bankSize)
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

bool AreaMusicOverride::PatchAreaMusicBankMetadata(uint8_t* bankData, size_t bankSize)
{
	AreaMusicMetadataPatch patch{};
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		patch = areaMusicMetadataPatch;
	}

	if (!patch.enabled || !bankData || bankSize < wwiseChunkHeaderSize)
	{
		return false;
	}

	bool patched = false;
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
			uint8_t* hirc = bankData + offset + wwiseChunkHeaderSize;
			patched = PatchAreaMusicHircMetadata(hirc, chunkSize, &patch) || patched;
		}

		offset += wwiseChunkHeaderSize + chunkSize;
	}

	return patched;
}

bool AreaMusicOverride::PatchLiveAreaMusicMetadata(const MusicData* data, long long fallbackDurationMs)
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
		Logger logger("Area Music Metadata");
		logger.Log("Cannot patch live Wwise metadata for \"%s\": no duration is known", data->name ? data->name : "");
		return false;
	}

	const long long effectiveDurationMs = (std::max)(
		1LL,
		sourceDurationMs
	);
	const uint32_t sourceDurationTicks = MillisecondsToWwiseTicks(sourceDurationMs);
	const uint32_t effectiveDurationTicks = MillisecondsToWwiseTicks(effectiveDurationMs);
	constexpr uint32_t sourceStartTicks = 0;
	const uint32_t mediaId = GetAreaMusicOverrideMediaId(data);
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
		Logger logger("Area Music Metadata");
		logger.Log(
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
	if (
		!HasWritableMemory(reinterpret_cast<uintptr_t>(segmentBytes), liveSegmentDurationOffset + sizeof(uint32_t))
		|| !HasWritableMemory(reinterpret_cast<uintptr_t>(trackBytes), liveTrackSourceDurationOffset + sizeof(uint32_t))
	)
	{
		Logger logger("Area Music Metadata");
		logger.Log("Refusing live Wwise metadata patch: segment or track object memory is not writable");
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + liveSegmentMarkerArrayOffset
	);
	const uint32_t markerCount = ReadUnaligned<uint32_t>(
		segmentBytes + liveSegmentMarkerCountOffset
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
		Logger logger("Area Music Metadata");
		logger.Log("Refusing live Wwise segment patch: marker count %u is unreasonable", markerCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* sourceArray = *reinterpret_cast<uint8_t**>(
		trackBytes + liveTrackSourceArrayOffset
	);
	const uint32_t sourceCount = ReadUnaligned<uint32_t>(
		trackBytes + liveTrackSourceCountOffset
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
		Logger logger("Area Music Metadata");
		logger.Log("Refusing live Wwise track patch: source entry count %u is unreasonable", sourceCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + liveTrackTimingArrayOffset
	);
	const uint32_t timingCount = ReadUnaligned<uint32_t>(
		trackBytes + liveTrackTimingCountOffset
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
		Logger logger("Area Music Metadata");
		logger.Log("Refusing live Wwise track patch: timing entry count %u is unreasonable", timingCount);
		ReleaseWwiseObject(segment);
		ReleaseWwiseObject(track);
		return false;
	}

	backup.valid = true;
	backup.segmentDurationTicks = ReadUnaligned<uint32_t>(
		segmentBytes + liveSegmentDurationOffset
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
		trackBytes + liveTrackSourceDurationOffset
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
		segmentBytes + liveSegmentDurationOffset,
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
		trackBytes + liveTrackSourceDurationOffset,
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
			segmentBytes + liveSegmentDurationOffset,
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
			trackBytes + liveTrackSourceDurationOffset,
			backup.trackSourceDurationTicks
		);
		if (timingArray && !backup.trackTimingBytes.empty())
		{
			std::memcpy(timingArray, backup.trackTimingBytes.data(), backup.trackTimingBytes.size());
		}

		Logger logger("Area Music Metadata");
		logger.Log(
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

	Logger logger("Area Music Metadata");
	logger.Log(
		"Patched live Area00 Wwise music metadata for \"%s\" (media id %u, source %lld ms/%u ticks, effective %lld ms/%u ticks, source start %u ticks, neutral trim fields, sources %u/%u, markers %u/%u, timing entries %u/%u)",
		data->name ? data->name : "",
		mediaId,
		sourceDurationMs,
		sourceDurationTicks,
		effectiveDurationMs,
		effectiveDurationTicks,
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

void AreaMusicOverride::RestoreLiveAreaMusicMetadata()
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
		Logger logger("Area Music Metadata");
		logger.Log(
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
	if (
		!HasWritableMemory(reinterpret_cast<uintptr_t>(segmentBytes), liveSegmentDurationOffset + sizeof(uint32_t))
		|| !HasWritableMemory(reinterpret_cast<uintptr_t>(trackBytes), liveTrackSourceDurationOffset + sizeof(uint32_t))
	)
	{
		Logger logger("Area Music Metadata");
		logger.Log("Refusing live Wwise metadata restore: segment or track object memory is not writable");
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
		segmentBytes + liveSegmentDurationOffset,
		backup.segmentDurationTicks
	);

	uint8_t* markerArray = *reinterpret_cast<uint8_t**>(
		segmentBytes + liveSegmentMarkerArrayOffset
	);
	const uint32_t markerCount = ReadUnaligned<uint32_t>(
		segmentBytes + liveSegmentMarkerCountOffset
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
		trackBytes + liveTrackSourceArrayOffset
	);
	const uint32_t sourceCount = ReadUnaligned<uint32_t>(
		trackBytes + liveTrackSourceCountOffset
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
		trackBytes + liveTrackSourceDurationOffset,
		backup.trackSourceDurationTicks
	);

	uint8_t* timingArray = *reinterpret_cast<uint8_t**>(
		trackBytes + liveTrackTimingArrayOffset
	);
	const uint32_t timingCount = ReadUnaligned<uint32_t>(
		trackBytes + liveTrackTimingCountOffset
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

	Logger logger("Area Music Metadata");
	logger.Log("Restored live Area00 Wwise music metadata");
	ReleaseWwiseObject(segment);
	ReleaseWwiseObject(track);
	{
		std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
		liveAreaMusicMetadataBackup = {};
		areaMusicOverrideMetadataPatched = false;
	}
}

void AreaMusicOverride::RememberAreaMusicBankMemory(
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
	Logger logger("Area Music Metadata");
	logger.Log(
		"Remembered Area00 Wwise bank memory from %s at %p (%zu bytes, bank id %u)",
		source ? source : "unknown",
		writableBankData,
		bankSize,
		bankId
	);
}

void AreaMusicOverride::BackupAreaMusicBankMetadataBytes(
	uint8_t* bankData,
	size_t bankSize,
	const std::vector<uint8_t>& bytes
)
{
	if (
		!bankData
		|| bankSize < wwiseChunkHeaderSize
		|| bytes.size() != bankSize
	)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	for (const AreaMusicBankMetadataBackup& backup : areaMusicBankMetadataBackups)
	{
		if (backup.data == bankData && backup.size == bankSize)
		{
			return;
		}
	}

	AreaMusicBankMetadataBackup backup{};
	backup.data = bankData;
	backup.size = bankSize;
	backup.bytes = bytes;
	areaMusicBankMetadataBackups.push_back(std::move(backup));
}

void AreaMusicOverride::BackupAreaMusicBankMetadata(uint8_t* bankData, size_t bankSize)
{
	if (!bankData || bankSize < wwiseChunkHeaderSize)
	{
		return;
	}

	BackupAreaMusicBankMetadataBytes(
		bankData,
		bankSize,
		std::vector<uint8_t>(bankData, bankData + bankSize)
	);
}

void AreaMusicOverride::RestoreAreaMusicBankMetadata()
{
	RestoreLiveAreaMusicMetadata();

	std::lock_guard<std::mutex> lock(areaMusicMetadataMutex);
	if (areaMusicBankMetadataBackups.empty())
	{
		return;
	}

	std::vector<AreaMusicBankMetadataBackup> remainingBackups{};
	size_t restored = 0;
	for (const AreaMusicBankMetadataBackup& backup : areaMusicBankMetadataBackups)
	{
		if (!backup.data || backup.bytes.empty())
		{
			continue;
		}

		DWORD oldProtect = 0;
		const bool protectionChanged = VirtualProtect(
			backup.data,
			backup.size,
			PAGE_READWRITE,
			&oldProtect
		) != FALSE;
		if (!protectionChanged)
		{
			remainingBackups.push_back(backup);
			continue;
		}

		std::memcpy(backup.data, backup.bytes.data(), backup.bytes.size());
		++restored;

		DWORD ignored = 0;
		VirtualProtect(backup.data, backup.size, oldProtect, &ignored);
	}

	areaMusicBankMetadataBackups = std::move(remainingBackups);

	if (restored > 0)
	{
		Logger logger("Area Music Metadata");
		logger.Log("Restored original Area00 Wwise metadata in %zu region(s)", restored);
	}
}

bool AreaMusicOverride::PatchKnownAreaMusicBankMetadata()
{
	return ReloadAreaMusicBankMetadata(true);
}

bool AreaMusicOverride::ReloadAreaMusicBankMetadata(bool patched)
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
		Logger logger("Area Music Metadata");
		logger.Log(
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
				Logger logger("Area Music Metadata");
				logger.Log(
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
			Logger logger("Area Music Metadata");
			logger.Log(
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
					logger.Log(
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

		Logger logger("Area Music Metadata");
		logger.Log(
			"Reloaded Area00 Wwise bank with %s metadata at %p (%zu bytes, bank id %u)",
			patched ? "patched" : "original",
			view.data,
			view.size,
			view.bankId
		);
		return true;
	}

	Logger logger("Area Music Metadata");
	logger.Log(
		"Could not find remembered Area00 Wwise bank memory to reload %s metadata",
		patched ? "patched" : "original"
	);
	return false;
}

bool AreaMusicOverride::PatchProcessAreaMusicBankMetadata()
{
	Logger logger("Area Music Metadata");
	logger.Log("Live process metadata patch is disabled; use bank reload instead");
	return false;
}

uint32_t __cdecl AreaMusicOverride::LoadBankMemoryCopyHook(
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
		Logger logger("Area Music Metadata");
		logger.Log("Observed Area00 Wwise bank during LoadBankMemoryCopy");
	}

	return originalLoadBankMemoryCopyFunc(bankData, bankSize, outBankId);
}

uint32_t __cdecl AreaMusicOverride::LoadBankMemoryViewHook(
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

bool AreaMusicOverride::ResolveWwiseMediaFunctions()
{
	if (wwiseMediaFunctionsResolved)
	{
		return setMediaFunc != nullptr;
	}

	wwiseMediaFunctionsResolved = true;
	HMODULE mainModule = GetModuleHandleA(nullptr);
	if (!mainModule)
	{
		Logger logger("Area Music Override");
		logger.Log("Failed to get main module handle for Wwise media exports");
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
		Logger logger("Area Music Override");
		logger.Log("Failed to resolve AK::SoundEngine::SetMedia export");
		return false;
	}
	if (!unsetMediaFunc)
	{
		Logger logger("Area Music Override");
		logger.Log("AK::SoundEngine::UnsetMedia export was not resolved; cleanup will be skipped");
	}

	Logger logger("Area Music Override");
	logger.Log(
		"Resolved Wwise SetMedia at %p and UnsetMedia at %p",
		reinterpret_cast<void*>(setMediaFunc),
		reinterpret_cast<void*>(unsetMediaFunc)
	);
	return true;
}

bool AreaMusicOverride::LoadAreaMusicOverride(const char* overridePath)
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
		retiredAreaMusicOverrideBuffers.begin(),
		retiredAreaMusicOverrideBuffers.end(),
		[&path](const std::unique_ptr<AreaMusicOverrideBuffer>& buffer)
		{
			return buffer
				&& !buffer->bytes.empty()
				&& buffer->path == path;
		}
	);
	if (retiredIt != retiredAreaMusicOverrideBuffers.end())
	{
		areaMusicOverrideBuffer = std::move(*retiredIt);
		retiredAreaMusicOverrideBuffers.erase(retiredIt);

		Logger logger("Area Music Override");
		logger.Log(
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
		Logger logger("Area Music Override");
		logger.Log("Failed to load custom audio override as Wwise media: %s", path.c_str());
		return false;
	}

	areaMusicOverrideBuffer = std::make_unique<AreaMusicOverrideBuffer>();
	areaMusicOverrideBuffer->path = path;
	areaMusicOverrideBuffer->bytes = std::move(media.bytes);
	areaMusicOverrideBuffer->sourcePluginId = media.sourcePluginId;
	areaMusicOverrideBuffer->durationMs = media.durationMs;
	Logger logger("Area Music Override");
	logger.Log(
		"Loaded area music audio override from %s (%zu bytes, source plugin 0x%08x, duration %lld ms)",
		path.c_str(),
		areaMusicOverrideBuffer->bytes.size(),
		areaMusicOverrideBuffer->sourcePluginId,
		areaMusicOverrideBuffer->durationMs
	);
	return true;
}

bool AreaMusicOverride::Register(const MusicData* data)
{
	if (!UsesOverride(data))
	{
		return true;
	}

	const char* overridePath = GetAreaMusicOverridePath(data);
	const uint32_t mediaId = GetAreaMusicOverrideMediaId(data);
	if (!ResolveWwiseMediaFunctions() || !LoadAreaMusicOverride(overridePath))
	{
		return false;
	}

	if (!areaMusicOverrideBuffer || areaMusicOverrideBuffer->bytes.empty())
	{
		return false;
	}

	const long long sourceDurationMs = GetRegisteredSourceDurationMs(data);
	const bool canPatchMetadata = sourceDurationMs > 0;
	if (canPatchMetadata)
	{
		PrepareAreaMusicMetadataPatch(
			data,
			static_cast<uint32_t>(areaMusicOverrideBuffer->bytes.size()),
			areaMusicOverrideBuffer->sourcePluginId,
			sourceDurationMs
		);
		areaMusicOverrideMetadataPatched = PatchLiveAreaMusicMetadata(
			data,
			sourceDurationMs
		);
	}
	else
	{
		ClearAreaMusicMetadataPatch();
		areaMusicOverrideMetadataPatched = false;
	}
	Logger metadataLogger("Area Music Metadata");
	metadataLogger.Log(
		"Area00 live metadata patch %s for \"%s\" (media %zu bytes, source plugin 0x%08x, duration %lld ms, neutral trim fields)",
		canPatchMetadata
			? (areaMusicOverrideMetadataPatched ? "applied" : "not applied")
			: "skipped because source duration is unknown",
		data->name ? data->name : "",
		areaMusicOverrideBuffer->bytes.size(),
		areaMusicOverrideBuffer->sourcePluginId,
		sourceDurationMs
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
		Logger logger("Area Music Override");
		logger.Log(
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
	Logger logger("Area Music Override");
	logger.Log(
		"Registered area music audio override for Wwise source %u from %s (source plugin 0x%08x)",
		mediaId,
		overridePath,
		areaMusicOverrideBuffer->sourcePluginId
	);
	return true;
}

void AreaMusicOverride::Unset()
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
		Logger logger("Area Music Override");
		logger.Log(
			"AK::SoundEngine::UnsetMedia failed for source %u with result %u",
			media.sourceId,
			result
		);
	}
	else
	{
		Logger logger("Area Music Override");
		logger.Log("Unregistered area music audio override for Wwise source %u", media.sourceId);
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

void AreaMusicOverride::RetireBuffer()
{
	if (!areaMusicOverrideBuffer || areaMusicOverrideBuffer->bytes.empty())
	{
		return;
	}

	Logger logger("Area Music Override");
	logger.Log(
		"Keeping retired area music audio buffer alive for %s (%zu bytes)",
		areaMusicOverrideBuffer->path.c_str(),
		areaMusicOverrideBuffer->bytes.size()
	);
	retiredAreaMusicOverrideBuffers.push_back(std::move(areaMusicOverrideBuffer));
}