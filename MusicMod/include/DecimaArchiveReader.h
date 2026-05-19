#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DecimaArchiveReader
{
	inline constexpr uint32_t maxExtractedFileSize = 128u * 1024u * 1024u;

	struct ReadFileResult
	{
		bool success = false;
		std::string error{};
		size_t copiedBytes = 0;
	};

	std::string BuildStreamedWemPath(uint32_t sourceId);
	std::wstring GetInitialArchivePath();

	ReadFileResult ReadInitialArchiveFile(
		const std::string& normalizedPath,
		uint32_t expectedSize,
		std::vector<uint8_t>& output
	);
}
