#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <zstd.h>

std::wstring StringToWString(const std::string& s);
std::string WStringToString(const std::wstring& s);

namespace acf
{
  constexpr uint32_t ACF_MAGIC = 0x39464341;
  constexpr uint32_t ACF_VERSION = 0x10000900;

  // Callback function for progress reporting and abortion.
  // Returns false if the operation should be aborted (e.g. user canceled).
  using CallbackFunc = std::function<bool(const std::string& currentFile, uint64_t fileBytesProcessed, uint64_t fileTotalBytes, uint64_t chunkBytes, float generalProgress)>;

  enum class EntryType: uint8_t
  {
    File = 0,
    Directory = 1
  };

  #pragma pack(push, 1)
  struct ACFHeader
  {
    uint32_t magic = ACF_MAGIC;
    uint32_t version = ACF_VERSION;
    uint64_t centralDirOffset = 0;
    uint64_t entryCount = 0;
    uint64_t reserved = static_cast<uint64_t>(-1);
  };

  struct ACFEntryData
  {
    EntryType type;
    uint64_t originalSize;
    uint64_t compressedSize;
    uint64_t dataOffset;
    uint32_t crc32;
    uint16_t pathLength;
  };
  #pragma pack(pop)

  class ACFArchiver
  {
  private:
    CallbackFunc m_CallbackFunc;
  public:
    ACFArchiver();
    virtual ~ACFArchiver();
    void SetCallback(const CallbackFunc callbackf);
    
    // Creates an archive from a list of files and directories.
    void Create(const std::string& archivePath, 
                const std::vector<std::string>& inputPaths,
                const std::string& basePath,
                const std::string& internalBasePath);

    // Creates a new archive containing a single entry from raw data.
    void CreateData(const std::string& archivePath, 
                const std::string& internalPath,
                const std::vector<uint8_t>& data);

    void ExtractAll(const std::string& archivePath,
                    const std::string& outputPath);

    void Extract(const std::string& archivePath,
                const std::vector<std::string>& archFileNames,
                const std::string& outputPath);

    // Extracts a single file, streaming it directly to disk to prevent RAM exhaustion.
    // If destFilePath is empty, it acts as a test (decodes but writes nowhere).
    void ExtractSingleFile(const std::string& archivePath, 
                           const std::string& archFileName, 
                           const std::string& destFilePath);

    // Kept for backward compatibility or very small files.
    std::vector<uint8_t> ExtractData(const std::string& archivePath,
                                    const std::string& archFileName);
                                    
    std::vector<std::string> List(const std::string& archivePath);
  };

} // namespace acf