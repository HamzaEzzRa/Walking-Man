#pragma once

#include <cstdint>
#include <iomanip>

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