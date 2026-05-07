#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <filesystem>

#include <sstream>
#include <string>
#include <string_view>

#include <Windows.h>

namespace Utils {
	static bool ReadFileBytesWide(
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

	static std::wstring QuoteCommandLineArgument(const std::wstring& argument)
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

	static std::wstring GetDirectory(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos)
		{
			return {};
		}
		return path.substr(0, slash);
	}

	static std::wstring GetCurrentModuleDirectory()
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

	static bool CreateTempFilePath(std::wstring& tempPath)
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

	static bool RunProcessAndWait(std::wstring commandLine, DWORD timeoutMs, DWORD& exitCode)
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

	static bool IsExistingFile(const std::wstring& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	static std::wstring FindExecutableInDirectory(const std::wstring& directory, const wchar_t* executableName)
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

	static std::wstring FindExecutableOnPath(const wchar_t* executableName)
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

	static std::string ToLowerAscii(std::string value)
	{
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); }
		);
		return value;
	}

	static std::string GetLowerExtension(const std::string& path)
	{
		const size_t slash = path.find_last_of("\\/");
		const size_t dot = path.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		{
			return {};
		}

		return ToLowerAscii(path.substr(dot));
	}

	static bool EndsWithExtension(const std::string& path, const char* extension)
	{
		if (!extension)
		{
			return false;
		}

		return GetLowerExtension(path) == ToLowerAscii(extension);
	}

	static bool TryPathToUtf8String(const std::filesystem::path& path, std::string& value)
	{
		try
		{
			value = path.u8string();
			return true;
		}
		catch (...) {}

		try
		{
			value = path.string();
			return true;
		}
		catch (...)
		{
			value.clear();
			return false;
		}
	}

	static std::string PathToLogString(const std::filesystem::path& path)
	{
		std::string value;
		return TryPathToUtf8String(path, value) ? value : "<unprintable path>";
	}

	static std::string Trim(const std::string& str)
	{
		const char* whitespace = " \t\r\n";
		size_t start = str.find_first_not_of(whitespace);
		size_t end = str.find_last_not_of(whitespace);
		return (start == std::string::npos) ? "" : str.substr(start, end - start + 1);
	}

	static bool IsCommentOrEmpty(const std::string& line)
	{
		std::string trimmed = Trim(line);
		return trimmed.empty() || trimmed.rfind("//", 0) == 0;
	}

	static std::string PointerToString(void* ptr)
	{
		std::stringstream ss;
		ss << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << (uintptr_t)ptr;
		return ss.str();
	}

	static void* StringToPointer(const std::string& hexStr)
	{
		std::stringstream ss(hexStr);
		uintptr_t addr = 0;
		ss >> std::hex >> addr;
		return reinterpret_cast<void*>(addr);
	}

	static uint32_t StringToUint32(const std::string& str)
	{
		return static_cast<uint32_t>(std::stoul(str, nullptr, 0));
	}

	static std::wstring Utf8ToWstring(const std::string& utf8Str)
	{
		if (utf8Str.empty())
		{
			return std::wstring();
		}

		int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
		if (wideLen == 0)
		{
			return std::wstring();  // handle error if needed
		}

		std::wstring wideStr(wideLen - 1, L'\0'); // exclude null terminator
		MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wideStr[0], wideLen - 1);

		return wideStr;
	}

	static std::string WstringToUtf8(const std::wstring& wideStr)
	{
		if (wideStr.empty())
		{
			return std::string();
		}

		int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (utf8Len == 0)
		{
			return std::string(); // handle error if needed
		}

		std::string utf8Str(utf8Len - 1, '\0'); // exclude null terminator
		WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, &utf8Str[0], utf8Len - 1, nullptr, nullptr);

		return utf8Str;
	}

	static std::string WstringViewToUtf8(const std::wstring_view& wideView)
	{
		if (wideView.empty())
		{
			return std::string();
		}

		int utf8Len = WideCharToMultiByte(
			CP_UTF8,
			0,
			wideView.data(),
			static_cast<int>(wideView.size()),
			nullptr,
			0,
			nullptr,
			nullptr
		);
		if (utf8Len == 0)
		{
			return std::string();
		}

		std::string utf8Str(utf8Len, '\0');
		WideCharToMultiByte(
			CP_UTF8,
			0,
			wideView.data(),
			static_cast<int>(wideView.size()),
			&utf8Str[0],
			utf8Len,
			nullptr,
			nullptr
		);

		return utf8Str;
	}

	static std::wstring Utf8ToUtf16(const std::string& utf8Str)
	{
		if (utf8Str.empty())
		{
			return L"";
		}

		int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
		if (wideLen <= 0)
		{
			return L"";
		}

		std::wstring wide(wideLen, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wide[0], wideLen);

		return wide;
	}

	static std::string EncodeGameText(const std::u16string& input)
	{
		std::string out;
		for (char16_t ch : input)
		{
			if (ch <= 0xFF)
			{
				// ASCII + Extended Latin-1, pass through
				out.push_back(static_cast<char>(ch));
			}
			else
			{
				// Non-ASCII → escaped as 01 + UTF-16LE
				out.push_back(0x01);
				out.push_back(ch & 0xFF);
				out.push_back((ch >> 8) & 0xFF);
			}
		}

		// Null-terminate the string (01 + 00 + 00)
		out.push_back(0x01);
		out.push_back(0x00);
		out.push_back(0x00);

		return out;
	}

	static std::wstring DecodeGameText(const std::string& encoded)
	{
		std::wstring result;
		size_t i = 0;

		while (i < encoded.size())
		{
			uint8_t b = static_cast<uint8_t>(encoded[i]);
			if (b == 0x01 && i + 2 < encoded.size())
			{
				// 0x01 + UTF-16LE (2 bytes)
				uint16_t ch = static_cast<uint8_t>(encoded[i + 1]) |
					(static_cast<uint8_t>(encoded[i + 2]) << 8);
				result.push_back(static_cast<wchar_t>(ch));
				i += 3;
			}
			else
			{
				// ASCII char
				result.push_back(static_cast<wchar_t>(b));
				i += 1;
			}
		}

		return result;
	}

	static std::string GameTextToUtf8(const std::string& encoded)
	{
		std::wstring decoded = DecodeGameText(encoded);
		if (!decoded.empty() && decoded.back() == L'\0')
		{
			decoded.pop_back();
		}

		return WstringToUtf8(decoded);
	}

	static const char* DecodeGameText(const std::wstring& wide)
	{
		static std::string utf8Buffer;
		std::wstring decoded;

		for (size_t i = 0; i < wide.size();)
		{
			wchar_t ch = wide[i];
			if (ch == 0x01 && i + 2 < wide.size())
			{
				// Decode escaped UTF-16LE character (stored as 3 UTF-16 code units)
				wchar_t low = wide[i + 1];
				wchar_t high = wide[i + 2];
				wchar_t combined = low | (high << 8);
				decoded.push_back(combined);
				i += 3;
			}
			else
			{
				// Regular wchar_t character (ASCII or already decoded)
				decoded.push_back(ch);
				i += 1;
			}
		}

		utf8Buffer = Utils::WstringToUtf8(decoded);
		return utf8Buffer.c_str();
	}

	static std::wstring MultiByteToWidePath(const std::string& path, UINT codePage, DWORD flags)
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

	static std::wstring ToWidePath(const std::string& path)
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

	static uint16_t ReadLe16FromBytes(const std::vector<uint8_t>& bytes, size_t offset)
	{
		if (offset + sizeof(uint16_t) > bytes.size())
		{
			return 0;
		}

		return static_cast<uint16_t>(bytes[offset])
			| static_cast<uint16_t>(bytes[offset + 1] << 8);
	}

	static uint32_t ReadLe32FromBytes(const std::vector<uint8_t>& bytes, size_t offset)
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

	static void AppendLe16(std::vector<uint8_t>& bytes, uint16_t value)
	{
		bytes.push_back(static_cast<uint8_t>(value & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
	}

	static void AppendLe32(std::vector<uint8_t>& bytes, uint32_t value)
	{
		bytes.push_back(static_cast<uint8_t>(value & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
		bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
	}

	static uintptr_t KeepTopHex(uintptr_t addr, int hexDigitsToKeep)
	{
		if (addr == 0 || hexDigitsToKeep <= 0)
			return 0;

		// Count how many hex digits are used (excluding leading 0s)
		int totalHexDigits = 0;
		uintptr_t tmp = addr;
		while (tmp)
		{
			totalHexDigits++;
			tmp >>= 4; // move 1 hex digit (4 bits) to the right
		}

		if (hexDigitsToKeep >= totalHexDigits)
			return addr;

		int digitsToTruncate = totalHexDigits - hexDigitsToKeep;
		uintptr_t mask = ~((uintptr_t{ 1 } << (digitsToTruncate * 4)) - 1);
		return addr & mask;
	}
};
