#pragma once

#include <cstdint>
#include <intrin.h>

#include <sstream>
#include <string>

#include <Windows.h>

static class Utils {
public:
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
		if (utf8Str.empty()) return std::wstring();

		int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
		if (wideLen == 0) return std::wstring();  // handle error if needed

		std::wstring wideStr(wideLen - 1, L'\0'); // exclude null terminator
		MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wideStr[0], wideLen - 1);

		return wideStr;
	}

	static std::string WstringToUtf8(const std::wstring& wideStr)
	{
		if (wideStr.empty()) return std::string();

		int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (utf8Len == 0) return std::string(); // handle error if needed

		std::string utf8Str(utf8Len - 1, '\0'); // exclude null terminator
		WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, &utf8Str[0], utf8Len - 1, nullptr, nullptr);

		return utf8Str;
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