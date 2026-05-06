#include "AudioDecoder.h"

#include <algorithm>
#include <cctype>
#include <cstring>
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

namespace
{
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

		bool Initialize(Logger& logger)
		{
			const HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (SUCCEEDED(coResult))
			{
				comInitialized = true;
			}
			else if (coResult != RPC_E_CHANGED_MODE)
			{
				logger.Log("CoInitializeEx failed while decoding audio: 0x%08x", coResult);
				return false;
			}

			const HRESULT mfResult = MFStartup(MF_VERSION, MFSTARTUP_LITE);
			if (FAILED(mfResult))
			{
				logger.Log("MFStartup failed while decoding audio: 0x%08x", mfResult);
				return false;
			}

			mediaFoundationStarted = true;
			return true;
		}
	};

	std::string ToLowerAscii(std::string value)
	{
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); }
		);
		return value;
	}

	std::string GetLowerExtension(const std::string& path)
	{
		const size_t slash = path.find_last_of("\\/");
		const size_t dot = path.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		{
			return {};
		}

		return ToLowerAscii(path.substr(dot));
	}

	bool EndsWithExtension(const std::string& path, const char* extension)
	{
		if (!extension)
		{
			return false;
		}

		return GetLowerExtension(path) == ToLowerAscii(extension);
	}

	uint16_t ReadLe16FromBytes(const std::vector<uint8_t>& bytes, size_t offset)
	{
		if (offset + sizeof(uint16_t) > bytes.size())
		{
			return 0;
		}

		return static_cast<uint16_t>(bytes[offset])
			| static_cast<uint16_t>(bytes[offset + 1] << 8);
	}

	uint32_t ReadLe32FromBytes(const std::vector<uint8_t>& bytes, size_t offset)
	{
		if (offset + sizeof(uint32_t) > bytes.size())
		{
			return 0;
		}

		return static_cast<uint32_t>(bytes[offset])
			| (static_cast<uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<uint32_t>(bytes[offset + 3]) << 24);
	}

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
			const uint32_t chunkSize = ReadLe32FromBytes(bytes, offset + 4);
			const size_t dataOffset = offset + 8;
			if (chunkSize > bytes.size() - dataOffset)
			{
				return 0;
			}

			if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkSize >= 2)
			{
				return ReadLe16FromBytes(bytes, dataOffset);
			}

			offset = dataOffset + chunkSize + (chunkSize & 1);
		}

		return 0;
	}

	std::wstring MultiByteToWidePath(const std::string& path, UINT codePage, DWORD flags)
	{
		if (path.empty() || path.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
		{
			return {};
		}

		const int sourceLength = static_cast<int>(path.size());
		const int wideLength = MultiByteToWideChar(
			codePage,
			flags,
			path.data(),
			sourceLength,
			nullptr,
			0
		);
		if (wideLength <= 0)
		{
			return {};
		}

		std::wstring widePath(static_cast<size_t>(wideLength), L'\0');
		const int converted = MultiByteToWideChar(
			codePage,
			flags,
			path.data(),
			sourceLength,
			widePath.data(),
			wideLength
		);
		if (converted <= 0)
		{
			return {};
		}

		return widePath;
	}

	std::wstring ToWidePath(const std::string& path)
	{
		std::wstring widePath = MultiByteToWidePath(path, CP_UTF8, MB_ERR_INVALID_CHARS);
		if (!widePath.empty())
		{
			return widePath;
		}

		widePath = MultiByteToWidePath(path, CP_ACP, 0);
		if (!widePath.empty())
		{
			return widePath;
		}

		return std::wstring(path.begin(), path.end());
	}

	void AppendLe16(std::vector<uint8_t>& bytes, uint16_t value)
	{
		bytes.push_back(static_cast<uint8_t>(value & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
	}

	void AppendLe32(std::vector<uint8_t>& bytes, uint32_t value)
	{
		bytes.push_back(static_cast<uint8_t>(value & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
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
			const uint32_t chunkSize = ReadLe32FromBytes(bytes, offset + 4);
			const size_t dataOffset = offset + 8;
			if (chunkSize > bytes.size() - dataOffset)
			{
				return false;
			}

			if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkSize >= 16)
			{
				formatTag = ReadLe16FromBytes(bytes, dataOffset);
				channels = ReadLe16FromBytes(bytes, dataOffset + 2);
				sampleRate = ReadLe32FromBytes(bytes, dataOffset + 4);
				bitsPerSample = ReadLe16FromBytes(bytes, dataOffset + 14);
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
			const uint32_t chunkSize = ReadLe32FromBytes(bytes, offset + 4);
			const size_t dataOffset = offset + 8;
			if (chunkSize > bytes.size() - dataOffset)
			{
				return false;
			}

			if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkSize >= 0x1c)
			{
				const uint16_t formatTag = ReadLe16FromBytes(bytes, dataOffset);
				if (formatTag != 0xffff)
				{
					return false;
				}

				channels = ReadLe16FromBytes(bytes, dataOffset + 2);
				sampleRate = ReadLe32FromBytes(bytes, dataOffset + 4);
				const uint32_t sampleCount = ReadLe32FromBytes(bytes, dataOffset + 0x18);
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
		AppendLe32(wemBytes, riffSize);
		wemBytes.insert(wemBytes.end(), { 'W', 'A', 'V', 'E' });
		wemBytes.insert(wemBytes.end(), { 'f', 'm', 't', ' ' });
		AppendLe32(wemBytes, 40);
		AppendLe16(wemBytes, 0xfffe);
		AppendLe16(wemBytes, static_cast<uint16_t>(channels));
		AppendLe32(wemBytes, sampleRate);
		AppendLe32(wemBytes, static_cast<uint32_t>(byteRate));
		AppendLe16(wemBytes, static_cast<uint16_t>(blockAlign));
		AppendLe16(wemBytes, static_cast<uint16_t>(bitsPerSample));
		AppendLe16(wemBytes, 22);
		AppendLe16(wemBytes, static_cast<uint16_t>(bitsPerSample));
		AppendLe32(wemBytes, GetDefaultChannelMask(channels));
		wemBytes.insert(
			wemBytes.end(),
			pcmSubFormatGuid,
			pcmSubFormatGuid + sizeof(pcmSubFormatGuid)
		);
		wemBytes.insert(wemBytes.end(), { 'd', 'a', 't', 'a' });
		AppendLe32(wemBytes, dataSize);
		wemBytes.insert(wemBytes.end(), pcmBytes.begin(), pcmBytes.end());
		return true;
	}

	bool ReadFileBytesWide(
		const std::wstring& path,
		std::vector<uint8_t>& bytes,
		size_t maxByteCount = static_cast<size_t>((std::numeric_limits<uint32_t>::max)())
	)
	{
		HANDLE file = CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);
		if (file == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		LARGE_INTEGER fileSize{};
		if (
			!GetFileSizeEx(file, &fileSize)
			|| fileSize.QuadPart <= 0
			|| static_cast<unsigned long long>(fileSize.QuadPart) > maxByteCount
			|| fileSize.QuadPart > (std::numeric_limits<DWORD>::max)()
		)
		{
			CloseHandle(file);
			return false;
		}

		bytes.resize(static_cast<size_t>(fileSize.QuadPart));
		DWORD read = 0;
		const BOOL ok = ReadFile(
			file,
			bytes.data(),
			static_cast<DWORD>(bytes.size()),
			&read,
			nullptr
		);
		CloseHandle(file);
		if (!ok || static_cast<size_t>(read) != bytes.size())
		{
			bytes.clear();
			return false;
		}

		return true;
	}

	std::wstring QuoteCommandLineArgument(const std::wstring& argument)
	{
		std::wstring quoted = L"\"";
		for (wchar_t c : argument)
		{
			if (c == L'"')
			{
				quoted += L"\\\"";
			}
			else
			{
				quoted += c;
			}
		}
		quoted += L"\"";
		return quoted;
	}

	std::wstring GetDirectory(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos)
		{
			return {};
		}
		return path.substr(0, slash);
	}

	bool IsExistingFile(const std::wstring& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	std::wstring FindExecutableInDirectory(const std::wstring& directory, const wchar_t* executableName)
	{
		if (!executableName || !executableName[0])
		{
			return {};
		}

		if (directory.empty())
		{
			return {};
		}

		std::wstring candidate = directory;
		if (candidate.back() != L'\\' && candidate.back() != L'/')
		{
			candidate += L"\\";
		}
		candidate += executableName;
		return IsExistingFile(candidate)
			? candidate
			: std::wstring{};
	}

	std::wstring FindExecutableOnPath(const wchar_t* executableName)
	{
		if (!executableName || !executableName[0])
		{
			return {};
		}

		const DWORD requiredLength = GetEnvironmentVariableW(L"PATH", nullptr, 0);
		if (requiredLength == 0)
		{
			return {};
		}

		std::wstring pathValue(requiredLength, L'\0');
		const DWORD copiedLength = GetEnvironmentVariableW(
			L"PATH",
			pathValue.data(),
			requiredLength
		);
		if (copiedLength == 0 || copiedLength >= requiredLength)
		{
			return {};
		}
		pathValue.resize(copiedLength);

		size_t start = 0;
		while (start <= pathValue.size())
		{
			const size_t separator = pathValue.find(L';', start);
			std::wstring directory = pathValue.substr(
				start,
				separator == std::wstring::npos ? std::wstring::npos : separator - start
			);

			if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"')
			{
				directory = directory.substr(1, directory.size() - 2);
			}

			const std::wstring candidate = FindExecutableInDirectory(directory, executableName);
			if (!candidate.empty())
			{
				return candidate;
			}

			if (separator == std::wstring::npos)
			{
				break;
			}
			start = separator + 1;
		}
		return {};
	}

	std::wstring GetCurrentModuleDirectory()
	{
		HMODULE moduleHandle = nullptr;
		if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&GetCurrentModuleDirectory),
			&moduleHandle
		))
		{
			return {};
		}

		std::wstring modulePath(MAX_PATH, L'\0');
		const DWORD pathLength = GetModuleFileNameW(
			moduleHandle,
			modulePath.data(),
			static_cast<DWORD>(modulePath.size())
		);
		if (pathLength == 0 || pathLength >= modulePath.size())
		{
			return {};
		}
		modulePath.resize(pathLength);
		return GetDirectory(modulePath);
	}

	std::wstring FindFfmpeg()
	{
		std::wstring ffmpegPath = FindExecutableOnPath(L"ffmpeg.exe");
		if (!ffmpegPath.empty())
		{
			return ffmpegPath;
		}

		return FindExecutableInDirectory(GetCurrentModuleDirectory(), L"ffmpeg.exe");
	}

	void ShowMissingFfmpegPopup(const std::string& path)
	{
		const std::string extension = GetLowerExtension(path);
		const std::string format = extension.empty()
			? std::string("audio file")
			: extension + " file";

		std::string message = "Couldn't convert " + format
			+ ", ffmpeg needed and ffmpeg not found!\n\nFile: " + path
			+ "\n\nPut ffmpeg.exe beside dxgi.dll or add it to PATH.";

		Logger logger("Audio Decoder");
		logger.Log("Raised error: %s", message.c_str());
		MessageBoxA(nullptr, message.c_str(), "Walking Man", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
	}

	bool CreateTempFilePath(std::wstring& tempPath)
	{
		wchar_t tempDirectory[MAX_PATH]{};
		if (GetTempPathW(MAX_PATH, tempDirectory) == 0)
		{
			return false;
		}

		wchar_t tempFile[MAX_PATH]{};
		if (GetTempFileNameW(tempDirectory, L"wmn", 0, tempFile) == 0)
		{
			return false;
		}

		tempPath = tempFile;
		return true;
	}

	bool RunProcessAndWait(std::wstring commandLine, DWORD timeoutMs, DWORD& exitCode)
	{
		STARTUPINFOW startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		startupInfo.dwFlags = STARTF_USESHOWWINDOW;
		startupInfo.wShowWindow = SW_HIDE;

		PROCESS_INFORMATION processInfo{};
		if (!CreateProcessW(
			nullptr,
			commandLine.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_NO_WINDOW,
			nullptr,
			nullptr,
			&startupInfo,
			&processInfo
		))
		{
			return false;
		}

		const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, timeoutMs);
		if (waitResult == WAIT_TIMEOUT)
		{
			TerminateProcess(processInfo.hProcess, 1);
			WaitForSingleObject(processInfo.hProcess, 5000);
		}

		exitCode = 1;
		GetExitCodeProcess(processInfo.hProcess, &exitCode);
		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
		return waitResult != WAIT_TIMEOUT;
	}

	bool DecodeAudioFileWithMediaFoundation(const std::string& path, AudioDecoder::WwiseMediaBuffer& output)
	{
		Logger logger("Audio Decoder");
		MediaFoundationScope mediaFoundation;
		if (!mediaFoundation.Initialize(logger))
		{
			return false;
		}

		const std::wstring widePath = ToWidePath(path);
		IMFSourceReader* rawReader = nullptr;
		HRESULT result = MFCreateSourceReaderFromURL(widePath.c_str(), nullptr, &rawReader);
		UniqueComPtr<IMFSourceReader> reader(rawReader);
		if (FAILED(result) || !reader)
		{
			logger.Log("Failed to open audio file with Media Foundation: %s (0x%08x)", path.c_str(), result);
			return false;
		}

		reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
		result = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
		if (FAILED(result))
		{
			logger.Log("Failed to select first audio stream for %s (0x%08x)", path.c_str(), result);
			return false;
		}

		IMFMediaType* rawTargetType = nullptr;
		result = MFCreateMediaType(&rawTargetType);
		UniqueComPtr<IMFMediaType> targetType(rawTargetType);
		if (FAILED(result) || !targetType)
		{
			logger.Log("Failed to create target PCM media type for %s (0x%08x)", path.c_str(), result);
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
			logger.Log("Failed to request PCM decode for %s (0x%08x)", path.c_str(), result);
			return false;
		}

		IMFMediaType* rawCurrentType = nullptr;
		result = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &rawCurrentType);
		UniqueComPtr<IMFMediaType> currentType(rawCurrentType);
		if (FAILED(result) || !currentType)
		{
			logger.Log("Failed to read decoded media type for %s (0x%08x)", path.c_str(), result);
			return false;
		}

		GUID subtype{};
		UINT32 channels = 0;
		UINT32 sampleRate = 0;
		UINT32 bitsPerSample = 0;
		result = currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
		if (FAILED(result) || subtype != MFAudioFormat_PCM)
		{
			logger.Log("Media Foundation did not provide PCM output for %s", path.c_str());
			return false;
		}
		if (
			FAILED(currentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))
			|| FAILED(currentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate))
			|| FAILED(currentType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample))
		)
		{
			logger.Log("Decoded PCM media type is missing format fields for %s", path.c_str());
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
				logger.Log("Failed while decoding audio file %s (0x%08x)", path.c_str(), result);
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
				logger.Log("Failed to collect decoded audio sample for %s (0x%08x)", path.c_str(), result);
				return false;
			}

			BYTE* sampleBytes = nullptr;
			DWORD maxLength = 0;
			DWORD currentLength = 0;
			result = buffer->Lock(&sampleBytes, &maxLength, &currentLength);
			if (FAILED(result))
			{
				logger.Log("Failed to lock decoded audio sample for %s (0x%08x)", path.c_str(), result);
				return false;
			}
			(void)maxLength;

			if (sampleBytes && currentLength > 0)
			{
				if (currentLength > maxPcmByteCount || pcmBytes.size() > maxPcmByteCount - currentLength)
				{
					buffer->Unlock();
					logger.Log("Decoded audio is too large for Wwise media memory: %s", path.c_str());
					return false;
				}
				pcmBytes.insert(pcmBytes.end(), sampleBytes, sampleBytes + currentLength);
			}
			buffer->Unlock();
		}

		if (!BuildPcmWemBytes(pcmBytes, channels, sampleRate, bitsPerSample, output.bytes))
		{
			logger.Log("Failed to build PCM WEM media bytes for %s", path.c_str());
			return false;
		}

		output.path = path;
		output.sourcePluginId = AudioDecoder::WwisePcmSourcePluginId;
		output.durationMs = CalculatePcmDurationMs(pcmBytes.size(), channels, sampleRate, bitsPerSample);
		output.channels = channels;
		output.sampleRate = sampleRate;
		output.bitsPerSample = bitsPerSample;
		output.decodedToPcm = true;

		logger.Log(
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
		Logger logger("Audio Decoder");
		const std::wstring widePath = ToWidePath(path);
		const std::wstring ffmpegPath = FindFfmpeg();
		if (ffmpegPath.empty())
		{
			logger.Log("No ffmpeg.exe found for fallback decode of %s", path.c_str());
			ShowMissingFfmpegPopup(path);
			return false;
		}

		std::wstring rawPcmPath;
		if (!CreateTempFilePath(rawPcmPath))
		{
			logger.Log("Failed to create temporary PCM output path for %s", path.c_str());
			return false;
		}

		std::wstring commandLine =
			QuoteCommandLineArgument(ffmpegPath)
			+ L" -hide_banner -loglevel error -i "
			+ QuoteCommandLineArgument(widePath)
			+ L" -vn -f s16le -acodec pcm_s16le -ac 2 -ar 48000 -y "
			+ QuoteCommandLineArgument(rawPcmPath);

		DWORD exitCode = 1;
		if (!RunProcessAndWait(commandLine, 300000, exitCode) || exitCode != 0)
		{
			DeleteFileW(rawPcmPath.c_str());
			logger.Log("ffmpeg failed while decoding %s (exit=%lu)", path.c_str(), exitCode);
			return false;
		}

		std::vector<uint8_t> pcmBytes;
		const size_t maxPcmByteCount =
			static_cast<size_t>((std::numeric_limits<uint32_t>::max)() - 60);
		if (!ReadFileBytesWide(rawPcmPath, pcmBytes, maxPcmByteCount))
		{
			DeleteFileW(rawPcmPath.c_str());
			logger.Log("Failed to read ffmpeg PCM output for %s", path.c_str());
			return false;
		}
		DeleteFileW(rawPcmPath.c_str());

		constexpr uint32_t channels = 2;
		constexpr uint32_t sampleRate = 48000;
		constexpr uint32_t bitsPerSample = 16;
		if (!BuildPcmWemBytes(pcmBytes, channels, sampleRate, bitsPerSample, output.bytes))
		{
			logger.Log("Failed to build ffmpeg PCM WEM media bytes for %s", path.c_str());
			return false;
		}

		output.path = path;
		output.sourcePluginId = AudioDecoder::WwisePcmSourcePluginId;
		output.durationMs = CalculatePcmDurationMs(pcmBytes.size(), channels, sampleRate, bitsPerSample);
		output.channels = channels;
		output.sampleRate = sampleRate;
		output.bitsPerSample = bitsPerSample;
		output.decodedToPcm = true;

		logger.Log(
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
	const std::vector<std::string>& SupportedCustomAudioExtensions()
	{
		static const std::vector<std::string> extensions =
		{
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
		return extensions;
	}

	bool IsSupportedCustomAudioPath(const std::string& path)
	{
		const std::string extension = GetLowerExtension(path);
		const std::vector<std::string>& supportedExtensions = SupportedCustomAudioExtensions();
		return std::find(
			supportedExtensions.begin(),
			supportedExtensions.end(),
			extension
		) != supportedExtensions.end();
	}

	bool IsWemPath(const std::string& path)
	{
		return EndsWithExtension(path, ".wem");
	}

	bool ShouldDecodeToPcmWem(const std::string& path)
	{
		return IsSupportedCustomAudioPath(path) && !IsWemPath(path);
	}

	uint32_t DetectWemSourcePluginId(const std::vector<uint8_t>& bytes)
	{
		const uint16_t formatTag = DetectRiffFormatTag(bytes);
		if (formatTag == 1 || formatTag == 0xfffe)
		{
			return WwisePcmSourcePluginId;
		}

		return WwiseVorbisSourcePluginId;
	}

	bool DecodeFileToPcmWem(const std::string& path, WwiseMediaBuffer& output)
	{
		output = {};
		if (!ShouldDecodeToPcmWem(path))
		{
			return false;
		}

		if (DecodeAudioFileWithMediaFoundation(path, output))
		{
			return true;
		}

		return DecodeAudioFileWithFfmpeg(path, output);
	}

	bool LoadWwiseMedia(const std::string& path, WwiseMediaBuffer& output)
	{
		output = {};
		if (path.empty())
		{
			return false;
		}

		if (ShouldDecodeToPcmWem(path))
		{
			return DecodeFileToPcmWem(path, output);
		}

		if (!IsWemPath(path))
		{
			return false;
		}

		std::vector<uint8_t> bytes;
		if (!ReadFileBytesWide(ToWidePath(path), bytes))
		{
			Logger logger("Audio Decoder");
			logger.Log("Failed to read Wwise media file: %s", path.c_str());
			return false;
		}

		output.path = path;
		output.bytes = std::move(bytes);
		output.sourcePluginId = DetectWemSourcePluginId(output.bytes);
		TryReadPcmWaveDuration(
			output.bytes,
			output.durationMs,
			output.channels,
			output.sampleRate,
			output.bitsPerSample
		);
		if (output.durationMs == 0 && output.sourcePluginId == WwiseVorbisSourcePluginId)
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