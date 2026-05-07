#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace Logging
{
	inline const std::chrono::steady_clock::time_point& GetLaunchTime()
	{
		static const auto launchTime = std::chrono::steady_clock::now();
		return launchTime;
	}

	inline FILE*& GetLogFile()
	{
		static FILE* logFile = nullptr;
		return logFile;
	}

	inline std::mutex& GetMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	inline long long GetElapsedSeconds()
	{
		return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - GetLaunchTime()
		).count();
	}

	inline void Initialize(const char* logFilename)
	{
		std::lock_guard<std::mutex> lock(GetMutex());
		GetLaunchTime();
		FILE*& logFile = GetLogFile();
		if (logFile || !logFilename || logFilename[0] == '\0')
		{
			return;
		}

		fopen_s(&logFile, logFilename, "w");
		if (logFile)
		{
			const unsigned char utf8Bom[] = { 0xEF, 0xBB, 0xBF };
			fwrite(utf8Bom, sizeof(unsigned char), sizeof(utf8Bom), logFile);
			fflush(logFile);
		}
	}

	inline void WriteV(const char* prefix, const char* format, va_list args)
	{
		const char* safePrefix = prefix ? prefix : "Log";
		const char* safeFormat = format ? format : "";
		const std::string formattedMessage =
			"[" + std::to_string(GetElapsedSeconds()) + "s] " + safePrefix + " > " + safeFormat + "\n";

		std::lock_guard<std::mutex> lock(GetMutex());

		va_list consoleArgs;
		va_copy(consoleArgs, args);
		vprintf(formattedMessage.c_str(), consoleArgs);
		va_end(consoleArgs);

		FILE* logFile = GetLogFile();
		if (logFile)
		{
			va_list fileArgs;
			va_copy(fileArgs, args);
			vfprintf(logFile, formattedMessage.c_str(), fileArgs);
			va_end(fileArgs);
			fflush(logFile);
		}
	}

	inline void Write(const char* prefix, const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		WriteV(prefix, format, args);
		va_end(args);
	}
}
