#include "DecimaArchiveReader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <Windows.h>
#include <wincrypt.h>

#include "Utils.h"

#pragma comment(lib, "advapi32.lib")

namespace
{
	constexpr uint32_t decimaPackMagicPlain = 0x20304050;
	constexpr uint32_t decimaPackMagicEncrypted = 0x21304050;
	constexpr uint32_t decimaPackChunkSize = 0x40000;
	constexpr const wchar_t* decimaInitialArchiveFilename =
		L"7017f9bb9d52fc1c4433599203cc51b1.bin";
	constexpr std::array<const wchar_t*, 2> decimaInitialArchiveDirectories{
		L"data",
		L"packed_GDK"
	};
	constexpr uint64_t decimaHeaderKey0 = 0xf41cab62fa3a9443ull;
	constexpr uint64_t decimaHeaderKey1 = 0xd2a89e3ef376811cull;
	constexpr uint64_t decimaDataKey0 = 0x7e159d956c084a37ull;
	constexpr uint64_t decimaDataKey1 = 0x18aa7d3f3d5af7e8ull;

	struct DecimaPackSpan
	{
		uint64_t offset = 0;
		uint32_t size = 0;
		uint32_t key = 0;
	};

	struct DecimaPackFileEntry
	{
		uint32_t index = 0;
		uint32_t key = 0;
		uint64_t hash = 0;
		DecimaPackSpan span{};
	};

	struct DecimaPackChunkEntry
	{
		DecimaPackSpan decompressed{};
		DecimaPackSpan compressed{};
	};

	struct UniqueHandle
	{
		HANDLE handle = INVALID_HANDLE_VALUE;

		explicit UniqueHandle(HANDLE value)
			: handle(value)
		{}

		UniqueHandle(const UniqueHandle&) = delete;
		UniqueHandle& operator=(const UniqueHandle&) = delete;

		~UniqueHandle()
		{
			if (handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(handle);
			}
		}

		bool IsValid() const
		{
			return handle != INVALID_HANDLE_VALUE;
		}
	};

	using OodleLZDecompressFn = long long(__cdecl*)(
		const void*,
		long long,
		void*,
		long long,
		int,
		int,
		int,
		void*,
		long long,
		void*,
		void*,
		void*,
		long long,
		int
	);

	DecimaArchiveReader::ReadFileResult Fail(
		std::vector<uint8_t>& output,
		const std::string& error,
		size_t copiedBytes = 0
	)
	{
		output.clear();
		return { false, error, copiedBytes };
	}

	uint64_t Rotl64(uint64_t value, int bits)
	{
		return (value << bits) | (value >> (64 - bits));
	}

	uint64_t Fmix64(uint64_t value)
	{
		value ^= value >> 33;
		value *= 0xff51afd7ed558ccdull;
		value ^= value >> 33;
		value *= 0xc4ceb9fe1a85ec53ull;
		value ^= value >> 33;
		return value;
	}

	std::array<uint64_t, 2> MurmurHash3X64_128(const uint8_t* data, size_t length, uint32_t seed = 0x2a)
	{
		constexpr uint64_t c1 = 0x87c37b91114253d5ull;
		constexpr uint64_t c2 = 0x4cf5ad432745937full;
		uint64_t h1 = seed;
		uint64_t h2 = seed;

		const size_t blockCount = length / 16;
		for (size_t i = 0; i < blockCount; i++)
		{
			uint64_t k1 = Utils::ReadLe64(data + i * 16);
			uint64_t k2 = Utils::ReadLe64(data + i * 16 + 8);

			k1 *= c1;
			k1 = Rotl64(k1, 31);
			k1 *= c2;
			h1 ^= k1;

			h1 = Rotl64(h1, 27);
			h1 += h2;
			h1 = h1 * 5 + 0x52dce729;

			k2 *= c2;
			k2 = Rotl64(k2, 33);
			k2 *= c1;
			h2 ^= k2;

			h2 = Rotl64(h2, 31);
			h2 += h1;
			h2 = h2 * 5 + 0x38495ab5;
		}

		const uint8_t* tail = data + blockCount * 16;
		uint64_t k1 = 0;
		uint64_t k2 = 0;

		switch (length & 15)
		{
		case 15: k2 ^= static_cast<uint64_t>(tail[14]) << 48; [[fallthrough]];
		case 14: k2 ^= static_cast<uint64_t>(tail[13]) << 40; [[fallthrough]];
		case 13: k2 ^= static_cast<uint64_t>(tail[12]) << 32; [[fallthrough]];
		case 12: k2 ^= static_cast<uint64_t>(tail[11]) << 24; [[fallthrough]];
		case 11: k2 ^= static_cast<uint64_t>(tail[10]) << 16; [[fallthrough]];
		case 10: k2 ^= static_cast<uint64_t>(tail[9]) << 8; [[fallthrough]];
		case 9:
			k2 ^= static_cast<uint64_t>(tail[8]);
			k2 *= c2;
			k2 = Rotl64(k2, 33);
			k2 *= c1;
			h2 ^= k2;
			[[fallthrough]];
		case 8: k1 ^= static_cast<uint64_t>(tail[7]) << 56; [[fallthrough]];
		case 7: k1 ^= static_cast<uint64_t>(tail[6]) << 48; [[fallthrough]];
		case 6: k1 ^= static_cast<uint64_t>(tail[5]) << 40; [[fallthrough]];
		case 5: k1 ^= static_cast<uint64_t>(tail[4]) << 32; [[fallthrough]];
		case 4: k1 ^= static_cast<uint64_t>(tail[3]) << 24; [[fallthrough]];
		case 3: k1 ^= static_cast<uint64_t>(tail[2]) << 16; [[fallthrough]];
		case 2: k1 ^= static_cast<uint64_t>(tail[1]) << 8; [[fallthrough]];
		case 1:
			k1 ^= static_cast<uint64_t>(tail[0]);
			k1 *= c1;
			k1 = Rotl64(k1, 31);
			k1 *= c2;
			h1 ^= k1;
			break;
		default:
			break;
		}

		h1 ^= length;
		h2 ^= length;

		h1 += h2;
		h2 += h1;

		h1 = Fmix64(h1);
		h2 = Fmix64(h2);

		h1 += h2;
		h2 += h1;

		return { h1, h2 };
	}

	bool ComputeMd5(const uint8_t* bytes, size_t size, uint8_t digest[16])
	{
		HCRYPTPROV provider = 0;
		HCRYPTHASH hash = 0;
		DWORD digestSize = 16;
		bool success = false;

		if (
			CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)
			&& CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)
			&& CryptHashData(hash, bytes, static_cast<DWORD>(size), 0)
			&& CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0)
			&& digestSize == 16
		)
		{
			success = true;
		}

		if (hash)
		{
			CryptDestroyHash(hash);
		}
		if (provider)
		{
			CryptReleaseContext(provider, 0);
		}
		return success;
	}

	void XorLe64(uint8_t* bytes, uint64_t value)
	{
		Utils::WriteLe64(bytes, Utils::ReadLe64(bytes) ^ value);
	}

	void SwizzleHeaderBlock(uint8_t* bytes, uint32_t key1, uint32_t key2)
	{
		uint8_t hashInput[16]{};
		Utils::WriteLe64(hashInput, decimaHeaderKey0);
		Utils::WriteLe64(hashInput + 8, decimaHeaderKey1);
		Utils::WriteLe32(hashInput, key1);
		const auto hash1 = MurmurHash3X64_128(hashInput, sizeof(hashInput));
		XorLe64(bytes, hash1[0]);
		XorLe64(bytes + 8, hash1[1]);

		Utils::WriteLe64(hashInput, decimaHeaderKey0);
		Utils::WriteLe64(hashInput + 8, decimaHeaderKey1);
		Utils::WriteLe32(hashInput, key2);
		const auto hash2 = MurmurHash3X64_128(hashInput, sizeof(hashInput));
		XorLe64(bytes + 16, hash2[0]);
		XorLe64(bytes + 24, hash2[1]);
	}

	bool SwizzleDataBlock(uint8_t* bytes, size_t size, const DecimaPackSpan& decompressed)
	{
		uint8_t hashInput[16]{};
		Utils::WriteLe64(hashInput, decompressed.offset);
		Utils::WriteLe32(hashInput + 8, decompressed.size);
		Utils::WriteLe32(hashInput + 12, decompressed.key);

		const auto hash = MurmurHash3X64_128(hashInput, sizeof(hashInput));
		Utils::WriteLe64(hashInput, hash[0] ^ decimaDataKey0);
		Utils::WriteLe64(hashInput + 8, hash[1] ^ decimaDataKey1);

		uint8_t xorKey[16]{};
		if (!ComputeMd5(hashInput, sizeof(hashInput), xorKey))
		{
			return false;
		}

		for (size_t i = 0; i < size; i++)
		{
			bytes[i] ^= xorKey[i & 15];
		}
		return true;
	}

	uint64_t ComputeDecimaPathHash(const std::string& normalizedPath)
	{
		std::vector<uint8_t> bytes(normalizedPath.begin(), normalizedPath.end());
		bytes.push_back(0);
		return MurmurHash3X64_128(bytes.data(), bytes.size())[0];
	}

	DecimaPackSpan ReadPackSpan(const uint8_t* bytes)
	{
		return {
			Utils::ReadLe64(bytes),
			Utils::ReadLe32(bytes + 8),
			Utils::ReadLe32(bytes + 12)
		};
	}

	DecimaPackFileEntry ReadPackFileEntry(uint8_t* bytes, bool encrypted)
	{
		if (encrypted)
		{
			const uint32_t key1 = Utils::ReadLe32(bytes + 4);
			const uint32_t key2 = Utils::ReadLe32(bytes + 28);
			SwizzleHeaderBlock(bytes, key1, key2);
			Utils::WriteLe32(bytes + 4, key1);
			Utils::WriteLe32(bytes + 28, key2);
		}

		return {
			Utils::ReadLe32(bytes),
			Utils::ReadLe32(bytes + 4),
			Utils::ReadLe64(bytes + 8),
			ReadPackSpan(bytes + 16)
		};
	}

	DecimaPackChunkEntry ReadPackChunkEntry(uint8_t* bytes, bool encrypted)
	{
		if (encrypted)
		{
			const uint32_t key1 = Utils::ReadLe32(bytes + 12);
			const uint32_t key2 = Utils::ReadLe32(bytes + 28);
			SwizzleHeaderBlock(bytes, key1, key2);
			Utils::WriteLe32(bytes + 12, key1);
			Utils::WriteLe32(bytes + 28, key2);
		}

		return {
			ReadPackSpan(bytes),
			ReadPackSpan(bytes + 16)
		};
	}

	OodleLZDecompressFn ResolveOodleDecompress()
	{
		static OodleLZDecompressFn decompress = nullptr;
		static bool attempted = false;
		if (attempted)
		{
			return decompress;
		}
		attempted = true;

		HMODULE module = GetModuleHandleW(L"oo2core_7_win64.dll");
		if (!module)
		{
			const std::wstring moduleDirectory = Utils::GetCurrentModuleDirectory();
			if (!moduleDirectory.empty())
			{
				module = LoadLibraryW((moduleDirectory + L"\\oo2core_7_win64.dll").c_str());
			}
		}
		if (!module)
		{
			module = LoadLibraryW(L"oo2core_7_win64.dll");
		}
		if (!module)
		{
			return nullptr;
		}

		decompress = reinterpret_cast<OodleLZDecompressFn>(
			GetProcAddress(module, "OodleLZ_Decompress")
		);
		return decompress;
	}

	bool OodleDecompressChunk(
		const std::vector<uint8_t>& compressed,
		std::vector<uint8_t>& decompressed,
		uint32_t expectedSize
	)
	{
		OodleLZDecompressFn decompress = ResolveOodleDecompress();
		if (!decompress)
		{
			return false;
		}

		decompressed.assign(expectedSize, 0);
		const long long result = decompress(
			compressed.data(),
			static_cast<long long>(compressed.size()),
			decompressed.data(),
			static_cast<long long>(decompressed.size()),
			1,
			1,
			0,
			nullptr,
			0,
			nullptr,
			nullptr,
			nullptr,
			0,
			3
		);
		return result == static_cast<long long>(expectedSize);
	}

	std::wstring BuildInitialArchivePath(const std::wstring& moduleDirectory, const wchar_t* directory)
	{
		return moduleDirectory + L"\\" + directory + L"\\" + decimaInitialArchiveFilename;
	}
}

namespace DecimaArchiveReader
{
	std::string BuildStreamedWemPath(uint32_t sourceId)
	{
		return "ds/sounds/streamed_wem_in_bank/generated/windows/"
			+ std::to_string(sourceId)
			+ ".core.stream";
	}

	std::wstring GetInitialArchivePath()
	{
		const std::wstring moduleDirectory = Utils::GetCurrentModuleDirectory();
		if (moduleDirectory.empty())
		{
			return {};
		}
		for (const wchar_t* directory : decimaInitialArchiveDirectories)
		{
			const std::wstring path = BuildInitialArchivePath(moduleDirectory, directory);
			if (Utils::IsExistingFile(path))
			{
				return path;
			}
		}
		return {};
	}

	ReadFileResult ReadInitialArchiveFile(
		const std::string& normalizedPath,
		uint32_t expectedSize,
		std::vector<uint8_t>& output
	)
	{
		output.clear();
		if (normalizedPath.empty())
		{
			return Fail(output, "Decima archive path is empty");
		}
		if (expectedSize < 16 || expectedSize > maxExtractedFileSize)
		{
			return Fail(output, "expected Decima archive entry size is not valid");
		}

		const uint64_t streamHash = ComputeDecimaPathHash(normalizedPath);
		const std::wstring archivePath = GetInitialArchivePath();
		if (archivePath.empty())
		{
			return Fail(output, "Decima Initial archive was not found");
		}

		UniqueHandle archive(CreateFileW(
			archivePath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		));
		if (!archive.IsValid())
		{
			return Fail(output,
				"cannot open Decima Initial archive: " + Utils::WstringToUtf8(archivePath)
			);
		}

		std::array<uint8_t, 40> headerBytes{};
		if (!Utils::ReadFileBytesAt(archive.handle, 0, headerBytes.data(), headerBytes.size()))
		{
			return Fail(output, "failed to read Decima Initial archive header");
		}

		const uint32_t magic = Utils::ReadLe32(headerBytes.data());
		const bool encrypted = magic == decimaPackMagicEncrypted;
		if (magic != decimaPackMagicPlain && magic != decimaPackMagicEncrypted)
		{
			return Fail(output,
				"Decima Initial archive has unexpected magic 0x" + std::to_string(magic)
			);
		}

		const uint32_t headerKey = Utils::ReadLe32(headerBytes.data() + 4);
		if (encrypted)
		{
			SwizzleHeaderBlock(headerBytes.data() + 8, headerKey, headerKey + 1);
		}

		const uint64_t fileSize = Utils::ReadLe64(headerBytes.data() + 8);
		const uint64_t dataSize = Utils::ReadLe64(headerBytes.data() + 16);
		const uint64_t fileEntryCount = Utils::ReadLe64(headerBytes.data() + 24);
		const uint32_t chunkEntryCount = Utils::ReadLe32(headerBytes.data() + 32);
		const uint32_t chunkEntrySize = Utils::ReadLe32(headerBytes.data() + 36);

		LARGE_INTEGER physicalFileSize{};
		if (
			fileEntryCount > 1000000ull
			|| chunkEntryCount > 1000000u
			|| chunkEntrySize != decimaPackChunkSize
			|| !GetFileSizeEx(archive.handle, &physicalFileSize)
			|| static_cast<uint64_t>(physicalFileSize.QuadPart) != fileSize
			|| dataSize == 0
		)
		{
			return Fail(output,
				"Decima Initial archive header is not valid (files "
				+ std::to_string(fileEntryCount)
				+ ", chunks " + std::to_string(chunkEntryCount)
				+ ", chunk size " + std::to_string(chunkEntrySize)
				+ ")"
			);
		}

		const uint64_t fileTableBytes = fileEntryCount * 32ull;
		const uint64_t chunkTableBytes = static_cast<uint64_t>(chunkEntryCount) * 32ull;
		const uint64_t tableBytes64 = fileTableBytes + chunkTableBytes;
		if (tableBytes64 > maxExtractedFileSize)
		{
			return Fail(output,
				"Decima Initial archive table is too large ("
				+ std::to_string(tableBytes64)
				+ " bytes)"
			);
		}

		std::vector<uint8_t> tableBytes(static_cast<size_t>(tableBytes64));
		if (!Utils::ReadFileBytesAt(archive.handle, headerBytes.size(), tableBytes.data(), tableBytes.size()))
		{
			return Fail(output, "failed to read Decima Initial archive tables");
		}

		DecimaPackFileEntry streamEntry{};
		bool foundStreamEntry = false;
		for (uint64_t i = 0; i < fileEntryCount; i++)
		{
			DecimaPackFileEntry entry = ReadPackFileEntry(
				tableBytes.data() + static_cast<size_t>(i * 32ull),
				encrypted
			);
			if (entry.hash == streamHash)
			{
				streamEntry = entry;
				foundStreamEntry = true;
				break;
			}
		}

		if (!foundStreamEntry)
		{
			return Fail(output,
				"could not find Decima stream \"" + normalizedPath
				+ "\" (hash " + std::to_string(streamHash) + ")"
			);
		}

		if (streamEntry.span.size != expectedSize)
		{
			return Fail(output,
				"Decima stream \"" + normalizedPath
				+ "\" size mismatch (archive "
				+ std::to_string(streamEntry.span.size)
				+ ", expected " + std::to_string(expectedSize)
				+ ")"
			);
		}

		uint8_t* chunkTable = tableBytes.data() + static_cast<size_t>(fileTableBytes);
		std::vector<DecimaPackChunkEntry> chunks;
		chunks.reserve(chunkEntryCount);
		for (uint32_t i = 0; i < chunkEntryCount; i++)
		{
			chunks.push_back(ReadPackChunkEntry(
				chunkTable + static_cast<size_t>(i) * 32ull,
				encrypted
			));
		}

		output.assign(streamEntry.span.size, 0);
		const uint64_t fileStart = streamEntry.span.offset;
		const uint64_t fileEnd = fileStart + streamEntry.span.size;
		size_t copiedBytes = 0;

		for (const DecimaPackChunkEntry& chunk : chunks)
		{
			const uint64_t chunkStart = chunk.decompressed.offset;
			const uint64_t chunkEnd = chunkStart + chunk.decompressed.size;
			if (chunkEnd <= fileStart || chunkStart >= fileEnd)
			{
				continue;
			}

			if (
				chunk.decompressed.size == 0
				|| chunk.decompressed.size > decimaPackChunkSize
				|| chunk.compressed.size == 0
				|| chunk.compressed.size > decimaPackChunkSize * 2u
			)
			{
				return Fail(output,
					"Decima chunk metadata is not valid (compressed "
					+ std::to_string(chunk.compressed.size)
					+ ", decompressed " + std::to_string(chunk.decompressed.size)
					+ ")",
					copiedBytes
				);
			}

			std::vector<uint8_t> compressed(chunk.compressed.size);
			if (!Utils::ReadFileBytesAt(archive.handle, chunk.compressed.offset, compressed.data(), compressed.size()))
			{
				return Fail(output, "failed to read Decima archive chunk", copiedBytes);
			}

			if (encrypted && !SwizzleDataBlock(compressed.data(), compressed.size(), chunk.decompressed))
			{
				return Fail(output, "failed to decrypt Decima archive chunk", copiedBytes);
			}

			std::vector<uint8_t> decompressed;
			if (!OodleDecompressChunk(compressed, decompressed, chunk.decompressed.size))
			{
				return Fail(output, "failed to decompress Decima archive chunk", copiedBytes);
			}

			const uint64_t copyStart = (std::max)(fileStart, chunkStart);
			const uint64_t copyEnd = (std::min)(fileEnd, chunkEnd);
			const size_t sourceOffset = static_cast<size_t>(copyStart - chunkStart);
			const size_t targetOffset = static_cast<size_t>(copyStart - fileStart);
			const size_t copySize = static_cast<size_t>(copyEnd - copyStart);
			std::memcpy(output.data() + targetOffset, decompressed.data() + sourceOffset, copySize);
			copiedBytes += copySize;
		}

		if (copiedBytes != output.size())
		{
			return Fail(output,
				"Decima archive extraction copied "
				+ std::to_string(copiedBytes)
				+ " of " + std::to_string(output.size())
				+ " bytes",
				copiedBytes
			);
		}

		return { true, {}, copiedBytes };
	}
}
