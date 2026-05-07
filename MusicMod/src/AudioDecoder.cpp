#include "AudioDecoder.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#include "Logger.h"
#include "Utils.h"

namespace
{
	constexpr uint32_t wwisePcmSourcePluginId = 0x00010001;
	constexpr uint32_t wwiseVorbisSourcePluginId = 0x00040001;

	const std::vector<std::string> supportedAudioExtensions = {
		".wem",
		".wav",
		".wave",
		".mp3",
		".flac",
		".ogg",
		".oga",
		".opus",
		".m4a",
		".m4b",
		".mp4",
		".webm",
		".aac",
		".wma",
		".aif",
		".aiff",
		".aifc",
		".alac",
		".ac3",
		".amr",
		".ape",
		".au",
		".caf",
		".mka",
		".mp2",
		".mpa",
		".m2a",
		".ra",
		".rm",
		".snd",
		".tta",
		".voc",
		".wv"
	};

	template<typename T>
	struct ComReleaser
	{
		void operator()(T* pointer) const
		{
			if (pointer)
			{
				pointer->Release();
			}
		}
	};

	template<typename T>
	using UniqueComPtr = std::unique_ptr<T, ComReleaser<T>>;

	struct MediaFoundationScope
	{
		bool comInitialized = false;
		bool mediaFoundationStarted = false;

		~MediaFoundationScope()
		{
			if (mediaFoundationStarted)
			{
				MFShutdown();
			}
			if (comInitialized)
			{
				CoUninitialize();
			}
		}

		bool Initialize(const char* logPrefix)
		{
			const HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (SUCCEEDED(coResult))
			{
				comInitialized = true;
			}
			else if (coResult != RPC_E_CHANGED_MODE)
			{
				Logging::Write(logPrefix, "CoInitializeEx failed while decoding audio: 0x%08x", coResult);
				return false;
			}

			const HRESULT mfResult = MFStartup(MF_VERSION, MFSTARTUP_LITE);
			if (FAILED(mfResult))
			{
				Logging::Write(logPrefix, "MFStartup failed while decoding audio: 0x%08x", mfResult);
				return false;
			}

			mediaFoundationStarted = true;
			return true;
		}
	};

	uint16_t DetectRiffFormatTag(const std::vector<uint8_t>& bytes)
	{
		if (
			bytes.size() < 20
			|| std::memcmp(bytes.data(), "RIFF", 4) != 0
			|| std::memcmp(bytes.data() + 8, "WAVE", 4) != 0
		)
		{
			return 0;
		}

		size_t offset = 12;
		while (offset + 8 <= bytes.size())
		{
			const uint32_t chunkSize = Utils::ReadLe32FromBytes(bytes, offset + 4);
			const size_t dataOffset = offset + 8;
			if (chunkSize > bytes.size() - dataOffset)
			{
				return 0;
			}

			if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkSize >= 2)
			{
				return Utils::ReadLe16FromBytes(bytes, dataOffset);
			}

			offset = dataOffset + chunkSize + (chunkSize & 1);
		}

		return 0;
	}

	uint32_t GetDefaultChannelMask(uint32_t channels)
	{
		switch (channels)
		{
		case 1:
			return 0x4;
		case 2:
			return 0x3;
		case 4:
			return 0x33;
		case 6:
			return 0x3f;
		case 8:
			return 0x63f;
		default:
			return channels < 32 ? ((1u << channels) - 1u) : 0;
		}
	}

	long long CalculatePcmDurationMs(
		size_t pcmByteCount,
		uint32_t channels,
		uint32_t sampleRate,
		uint32_t bitsPerSample
	)
	{
		if (
			pcmByteCount == 0
			|| channels == 0
			|| sampleRate == 0
			|| bitsPerSample == 0
			|| bitsPerSample % 8 != 0
		)
		{
			return 0;
		}

		const uint64_t bytesPerFrame = static_cast<uint64_t>(channels) * (bitsPerSample / 8);
		if (bytesPerFrame == 0)
		{
			return 0;
		}

		return static_cast<long long>(
			(static_cast<unsigned long long>(pcmByteCount) * 1000ULL)
			/ (bytesPerFrame * sampleRate)
		);
	}

	bool TryReadPcmWaveDuration(
		const std::vector<uint8_t>& bytes,
		long long& durationMs,
		uint32_t& channels,
		uint32_t& sampleRate,
		uint32_t& bitsPerSample
	)
	{
		durationMs = 0;
		channels = 0;
		sampleRate = 0;
		bitsPerSample = 0;

		if (
			bytes.size() < 20
			|| std::memcmp(bytes.data(), "RIFF", 4) != 0
			|| std::memcmp(bytes.data() + 8, "WAVE", 4) != 0
		)
		{
			return false;
		}

		uint16_t formatTag = 0;
		uint32_t dataSize = 0;
		size_t offset = 12;
		while (offset + 8 <= bytes.size())
		{
			const uint32_t chunkSize = Utils::ReadLe32FromBytes(bytes, offset + 4);
			const size_t dataOffset = offset + 8;
			if (chunkSize > bytes.size() - dataOffset)
			{
				return false;
			}

			if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkSize >= 16)
			{
				formatTag = Utils::ReadLe16FromBytes(bytes, dataOffset);
				channels = Utils::ReadLe16FromBytes(bytes, dataOffset + 2);
				sampleRate = Utils::ReadLe32FromBytes(bytes, dataOffset + 4);
				bitsPerSample = Utils::ReadLe16FromBytes(bytes, dataOffset + 14);
			}
			else if (std::memcmp(bytes.data() + offset, "data", 4) == 0)
			{
				dataSize = chunkSize;
			}

			offset = dataOffset + chunkSize + (chunkSize & 1);
		}

		if (
			(formatTag != 1 && formatTag != 0xfffe)
			|| dataSize == 0
			|| channels == 0
			|| sampleRate == 0
			|| bitsPerSample == 0
		)
		{
			return false;
		}

		durationMs = CalculatePcmDurationMs(dataSize, channels, sampleRate, bitsPerSample);
		return durationMs > 0;
	}

	bool TryReadWwiseVorbisDuration(
		const std::vector<uint8_t>& bytes,
		long long& durationMs,
		uint32_t& channels,
		uint32_t& sampleRate
	)
	{
		durationMs = 0;
		channels = 0;
		sampleRate = 0;

		if (
			bytes.size() < 20
			|| std::memcmp(bytes.data(), "RIFF", 4) != 0
			|| std::memcmp(bytes.data() + 8, "WAVE", 4) != 0
		)
		{
			return false;
		}

		size_t offset = 12;
		while (offset + 8 <= bytes.size())
		{
			const uint32_t chunkSize = Utils::ReadLe32FromBytes(bytes, offset + 4);
			const size_t dataOffset = offset + 8;
			if (chunkSize > bytes.size() - dataOffset)
			{
				return false;
			}

			if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkSize >= 0x1c)
			{
				const uint16_t formatTag = Utils::ReadLe16FromBytes(bytes, dataOffset);
				if (formatTag != 0xffff)
				{
					return false;
				}

				channels = Utils::ReadLe16FromBytes(bytes, dataOffset + 2);
				sampleRate = Utils::ReadLe32FromBytes(bytes, dataOffset + 4);
				const uint32_t sampleCount = Utils::ReadLe32FromBytes(bytes, dataOffset + 0x18);
				if (channels == 0 || sampleRate == 0 || sampleCount == 0)
				{
					return false;
				}

				durationMs = static_cast<long long>(
					(static_cast<unsigned long long>(sampleCount) * 1000ULL)
					/ sampleRate
				);
				return durationMs > 0;
			}

			offset = dataOffset + chunkSize + (chunkSize & 1);
		}

		return false;
	}

	bool BuildPcmWemBytes(
		const std::vector<uint8_t>& pcmBytes,
		uint32_t channels,
		uint32_t sampleRate,
		uint32_t bitsPerSample,
		std::vector<uint8_t>& wemBytes
	)
	{
		if (
			pcmBytes.empty()
			|| channels == 0
			|| sampleRate == 0
			|| bitsPerSample == 0
			|| bitsPerSample % 8 != 0
			|| pcmBytes.size() > static_cast<size_t>((std::numeric_limits<uint32_t>::max)() - 60)
		)
		{
			return false;
		}

		const uint64_t blockAlign = static_cast<uint64_t>(channels) * (bitsPerSample / 8);
		const uint64_t byteRate = blockAlign * sampleRate;
		if (
			blockAlign == 0
			|| blockAlign > (std::numeric_limits<uint16_t>::max)()
			|| byteRate > (std::numeric_limits<uint32_t>::max)()
		)
		{
			return false;
		}

		const uint32_t dataSize = static_cast<uint32_t>(pcmBytes.size());
		const uint32_t riffSize = 60 + dataSize;
		const uint8_t pcmSubFormatGuid[16] = {
			0x01, 0x00, 0x00, 0x00,
			0x00, 0x00,
			0x10, 0x00,
			0x80, 0x00,
			0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
		};

		wemBytes.clear();
		wemBytes.reserve(static_cast<size_t>(68) + pcmBytes.size());
		wemBytes.insert(wemBytes.end(), { 'R', 'I', 'F', 'F' });
		Utils::AppendLe32(wemBytes, riffSize);
		wemBytes.insert(wemBytes.end(), { 'W', 'A', 'V', 'E' });
		wemBytes.insert(wemBytes.end(), { 'f', 'm', 't', ' ' });
		Utils::AppendLe32(wemBytes, 40);
		Utils::AppendLe16(wemBytes, 0xfffe);
		Utils::AppendLe16(wemBytes, static_cast<uint16_t>(channels));
		Utils::AppendLe32(wemBytes, sampleRate);
		Utils::AppendLe32(wemBytes, static_cast<uint32_t>(byteRate));
		Utils::AppendLe16(wemBytes, static_cast<uint16_t>(blockAlign));
		Utils::AppendLe16(wemBytes, static_cast<uint16_t>(bitsPerSample));
		Utils::AppendLe16(wemBytes, 22);
		Utils::AppendLe16(wemBytes, static_cast<uint16_t>(bitsPerSample));
		Utils::AppendLe32(wemBytes, GetDefaultChannelMask(channels));
		wemBytes.insert(
			wemBytes.end(),
			pcmSubFormatGuid,
			pcmSubFormatGuid + sizeof(pcmSubFormatGuid)
		);
		wemBytes.insert(wemBytes.end(), { 'd', 'a', 't', 'a' });
		Utils::AppendLe32(wemBytes, dataSize);
		wemBytes.insert(wemBytes.end(), pcmBytes.begin(), pcmBytes.end());
		return true;
	}

	bool DecodeAudioFileWithMediaFoundation(const std::string& path, AudioDecoder::WwiseMediaBuffer& output)
	{
		constexpr const char* logPrefix = "Audio Decoder";
		MediaFoundationScope mediaFoundation;
		if (!mediaFoundation.Initialize(logPrefix))
		{
			return false;
		}

		const std::wstring widePath = Utils::ToWidePath(path);
		IMFSourceReader* rawReader = nullptr;
		HRESULT result = MFCreateSourceReaderFromURL(widePath.c_str(), nullptr, &rawReader);
		UniqueComPtr<IMFSourceReader> reader(rawReader);
		if (FAILED(result) || !reader)
		{
			Logging::Write(logPrefix, "Failed to open audio file with Media Foundation: %s (0x%08x)", path.c_str(), result);
			return false;
		}

		reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
		result = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
		if (FAILED(result))
		{
			Logging::Write(logPrefix, "Failed to select first audio stream for %s (0x%08x)", path.c_str(), result);
			return false;
		}

		IMFMediaType* rawTargetType = nullptr;
		result = MFCreateMediaType(&rawTargetType);
		UniqueComPtr<IMFMediaType> targetType(rawTargetType);
		if (FAILED(result) || !targetType)
		{
			Logging::Write(logPrefix, "Failed to create target PCM media type for %s (0x%08x)", path.c_str(), result);
			return false;
		}

		targetType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		targetType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		targetType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

		result = reader->SetCurrentMediaType(
			MF_SOURCE_READER_FIRST_AUDIO_STREAM,
			nullptr,
			targetType.get()
		);
		if (FAILED(result))
		{
			Logging::Write(logPrefix, "Failed to request PCM decode for %s (0x%08x)", path.c_str(), result);
			return false;
		}

		IMFMediaType* rawCurrentType = nullptr;
		result = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &rawCurrentType);
		UniqueComPtr<IMFMediaType> currentType(rawCurrentType);
		if (FAILED(result) || !currentType)
		{
			Logging::Write(logPrefix, "Failed to read decoded media type for %s (0x%08x)", path.c_str(), result);
			return false;
		}

		GUID subtype{};
		UINT32 channels = 0;
		UINT32 sampleRate = 0;
		UINT32 bitsPerSample = 0;
		result = currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
		if (FAILED(result) || subtype != MFAudioFormat_PCM)
		{
			Logging::Write(logPrefix, "Media Foundation did not provide PCM output for %s", path.c_str());
			return false;
		}
		if (
			FAILED(currentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))
			|| FAILED(currentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate))
			|| FAILED(currentType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample))
		)
		{
			Logging::Write(logPrefix, "Decoded PCM media type is missing format fields for %s", path.c_str());
			return false;
		}

		std::vector<uint8_t> pcmBytes;
		const size_t maxPcmByteCount =
			static_cast<size_t>((std::numeric_limits<uint32_t>::max)() - 60);
		for (;;)
		{
			DWORD streamIndex = 0;
			DWORD flags = 0;
			LONGLONG timestamp = 0;
			IMFSample* rawSample = nullptr;
			result = reader->ReadSample(
				MF_SOURCE_READER_FIRST_AUDIO_STREAM,
				0,
				&streamIndex,
				&flags,
				&timestamp,
				&rawSample
			);
			UniqueComPtr<IMFSample> sample(rawSample);
			if (FAILED(result))
			{
				Logging::Write(logPrefix, "Failed while decoding audio file %s (0x%08x)", path.c_str(), result);
				return false;
			}
			(void)streamIndex;
			(void)timestamp;
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
			{
				break;
			}
			if (!sample)
			{
				continue;
			}

			IMFMediaBuffer* rawBuffer = nullptr;
			result = sample->ConvertToContiguousBuffer(&rawBuffer);
			UniqueComPtr<IMFMediaBuffer> buffer(rawBuffer);
			if (FAILED(result) || !buffer)
			{
				Logging::Write(logPrefix, "Failed to collect decoded audio sample for %s (0x%08x)", path.c_str(), result);
				return false;
			}

			BYTE* sampleBytes = nullptr;
			DWORD maxLength = 0;
			DWORD currentLength = 0;
			result = buffer->Lock(&sampleBytes, &maxLength, &currentLength);
			if (FAILED(result))
			{
				Logging::Write(logPrefix, "Failed to lock decoded audio sample for %s (0x%08x)", path.c_str(), result);
				return false;
			}
			(void)maxLength;

			if (sampleBytes && currentLength > 0)
			{
				if (currentLength > maxPcmByteCount || pcmBytes.size() > maxPcmByteCount - currentLength)
				{
					buffer->Unlock();
					Logging::Write(logPrefix, "Decoded audio is too large for Wwise media memory: %s", path.c_str());
					return false;
				}
				pcmBytes.insert(pcmBytes.end(), sampleBytes, sampleBytes + currentLength);
			}
			buffer->Unlock();
		}

		if (!BuildPcmWemBytes(pcmBytes, channels, sampleRate, bitsPerSample, output.bytes))
		{
			Logging::Write(logPrefix, "Failed to build PCM WEM media bytes for %s", path.c_str());
			return false;
		}

		output.path = path;
		output.sourcePluginId = wwisePcmSourcePluginId;
		output.durationMs = CalculatePcmDurationMs(pcmBytes.size(), channels, sampleRate, bitsPerSample);
		output.channels = channels;
		output.sampleRate = sampleRate;
		output.bitsPerSample = bitsPerSample;
		output.decodedToPcm = true;

		Logging::Write(logPrefix,
			"Decoded %s to PCM WEM with Media Foundation (%u Hz, %u channel(s), %u-bit, %lld ms, %zu bytes)",
			path.c_str(),
			sampleRate,
			channels,
			bitsPerSample,
			output.durationMs,
			output.bytes.size()
		);
		return true;
	}

	bool DecodeAudioFileWithFfmpeg(const std::string& path, AudioDecoder::WwiseMediaBuffer& output)
	{
		constexpr const char* logPrefix = "Audio Decoder";
		const std::wstring widePath = Utils::ToWidePath(path);
		std::wstring ffmpegPath = Utils::FindExecutableOnPath(L"ffmpeg.exe");
		if (ffmpegPath.empty())
		{
			ffmpegPath = Utils::FindExecutableInDirectory(Utils::GetCurrentModuleDirectory(), L"ffmpeg.exe");
		}
		if (ffmpegPath.empty())
		{
			Logging::Write(logPrefix, "No ffmpeg.exe found for fallback decode of %s", path.c_str());

			const std::string extension = Utils::GetLowerExtension(path);
			const std::string format = extension.empty()
				? std::string("audio file")
				: extension + " file";
			const std::string message = "Couldn't convert " + format
				+ ", ffmpeg needed and ffmpeg not found!\n\nFile: " + path
				+ "\n\nGet and put ffmpeg.exe in game's directory.";
			Logging::Write(logPrefix, "Raised error: %s", message.c_str());
			MessageBoxA(nullptr, message.c_str(), "Walking Man", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
			return false;
		}

		std::wstring rawPcmPath;
		if (!Utils::CreateTempFilePath(rawPcmPath))
		{
			Logging::Write(logPrefix, "Failed to create temporary PCM output path for %s", path.c_str());
			return false;
		}

		std::wstring commandLine =
			Utils::QuoteCommandLineArgument(ffmpegPath)
			+ L" -hide_banner -loglevel error -i "
			+ Utils::QuoteCommandLineArgument(widePath)
			+ L" -vn -f s16le -acodec pcm_s16le -ac 2 -ar 48000 -y "
			+ Utils::QuoteCommandLineArgument(rawPcmPath);

		DWORD exitCode = 1;
		if (!Utils::RunProcessAndWait(commandLine, 300000, exitCode) || exitCode != 0)
		{
			DeleteFileW(rawPcmPath.c_str());
			Logging::Write(logPrefix, "ffmpeg failed while decoding %s (exit=%lu)", path.c_str(), exitCode);
			return false;
		}

		std::vector<uint8_t> pcmBytes;
		const size_t maxPcmByteCount =
			static_cast<size_t>((std::numeric_limits<uint32_t>::max)() - 60);
		if (!Utils::ReadFileBytesWide(rawPcmPath, pcmBytes, maxPcmByteCount))
		{
			DeleteFileW(rawPcmPath.c_str());
			Logging::Write(logPrefix, "Failed to read ffmpeg PCM output for %s", path.c_str());
			return false;
		}
		DeleteFileW(rawPcmPath.c_str());

		constexpr uint32_t channels = 2;
		constexpr uint32_t sampleRate = 48000;
		constexpr uint32_t bitsPerSample = 16;
		if (!BuildPcmWemBytes(pcmBytes, channels, sampleRate, bitsPerSample, output.bytes))
		{
			Logging::Write(logPrefix, "Failed to build ffmpeg PCM WEM media bytes for %s", path.c_str());
			return false;
		}

		output.path = path;
		output.sourcePluginId = wwisePcmSourcePluginId;
		output.durationMs = CalculatePcmDurationMs(pcmBytes.size(), channels, sampleRate, bitsPerSample);
		output.channels = channels;
		output.sampleRate = sampleRate;
		output.bitsPerSample = bitsPerSample;
		output.decodedToPcm = true;

		Logging::Write(logPrefix,
			"Decoded %s to PCM WEM with ffmpeg (%u Hz, %u channel(s), %u-bit, %lld ms, %zu bytes)",
			path.c_str(),
			sampleRate,
			channels,
			bitsPerSample,
			output.durationMs,
			output.bytes.size()
		);
		return true;
	}
}

namespace AudioDecoder
{
	bool IsSupportedCustomAudioPath(const std::string& path)
	{
		const std::string extension = Utils::GetLowerExtension(path);
		return std::find(
			supportedAudioExtensions.begin(),
			supportedAudioExtensions.end(),
			extension
		) != supportedAudioExtensions.end();
	}

	bool IsSupportedCustomAudioPath(const std::filesystem::path& path)
	{
		std::string extension;
		if (!Utils::TryPathToUtf8String(path.extension(), extension))
		{
			return false;
		}

		std::string formattedExt = Utils::ToLowerAscii(extension);
		return std::find(
			supportedAudioExtensions.begin(),
			supportedAudioExtensions.end(),
			formattedExt
		) != supportedAudioExtensions.end();
	}

	bool LoadWwiseMedia(const std::string& path, WwiseMediaBuffer& output)
	{
		output = {};
		if (path.empty())
		{
			return false;
		}

		const bool isWem = Utils::EndsWithExtension(path, ".wem");
		if (!isWem)
		{
			if (!IsSupportedCustomAudioPath(path))
			{
				return false;
			}

			if (DecodeAudioFileWithMediaFoundation(path, output))
			{
				return true;
			}

			return DecodeAudioFileWithFfmpeg(path, output);
		}

		std::vector<uint8_t> bytes;
		if (!Utils::ReadFileBytesWide(Utils::ToWidePath(path), bytes))
		{
			constexpr const char* logPrefix = "Audio Decoder";
			Logging::Write(logPrefix, "Failed to read Wwise media file: %s", path.c_str());
			return false;
		}

		output.path = path;
		output.bytes = std::move(bytes);
		const uint16_t formatTag = DetectRiffFormatTag(output.bytes);
		output.sourcePluginId = (formatTag == 1 || formatTag == 0xfffe)
			? wwisePcmSourcePluginId
			: wwiseVorbisSourcePluginId;
		TryReadPcmWaveDuration(
			output.bytes,
			output.durationMs,
			output.channels,
			output.sampleRate,
			output.bitsPerSample
		);
		if (output.durationMs == 0 && output.sourcePluginId == wwiseVorbisSourcePluginId)
		{
			uint32_t channels = 0;
			uint32_t sampleRate = 0;
			if (TryReadWwiseVorbisDuration(output.bytes, output.durationMs, channels, sampleRate))
			{
				output.channels = channels;
				output.sampleRate = sampleRate;
			}
		}
		output.decodedToPcm = false;
		return true;
	}
}
