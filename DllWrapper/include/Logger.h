#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "ModConfiguration.h"

class Logger
{
public:
	Logger(const char* prefix)
	{
		printPrefix = prefix;
		GetLaunchTime();

		FILE* logFile = GetLogFile();
		if (logFile == nullptr)
		{
			fopen_s(&logFile, ModConfiguration::modLogFilename.c_str(), "w");
			if (logFile != nullptr)
			{
				const unsigned char utf8Bom[] = { 0xEF, 0xBB, 0xBF };
				fwrite(utf8Bom, sizeof(unsigned char), sizeof(utf8Bom), logFile);
			}
			GetLogFile(logFile);
		}
	}

	void Log(std::string msg, ...)
	{
		const std::string formattedMessage =
			"[" + std::to_string(GetElapsedSeconds()) + "s] " + printPrefix + " > " + msg + "\n";

		va_list args;
		va_start(args, msg);

		va_list fileArgs;
		va_copy(fileArgs, args);

		vprintf(formattedMessage.c_str(), args);
		if (GetLogFile() != nullptr)
		{
			vfprintf(GetLogFile(), formattedMessage.c_str(), fileArgs);
			fflush(GetLogFile());
		}

		va_end(fileArgs);
		va_end(args);
	}

private:
	std::string printPrefix = "";

	static const std::chrono::steady_clock::time_point& GetLaunchTime()
	{
		static const auto launchTime = std::chrono::steady_clock::now();
		return launchTime;
	}

	static long long GetElapsedSeconds()
	{
		return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - GetLaunchTime()
		).count();
	}

	static FILE* GetLogFile(FILE* newLogFile = nullptr)
	{
		static FILE* logFile = nullptr;
		if (newLogFile != nullptr)
		{
			logFile = newLogFile;
		}
		return logFile;
	}
};
