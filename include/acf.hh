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

  // Callback function for progress reporting.
  // Parameters: current file path, progress for the current file (0-1), overall progress (0-1).
  using CallbackFunc = std::function<void(const std::string& currentFile, float currentFileProgress, float generalProgress)>;

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
    uint32_t centralDirCRC32 = 0;
    uint32_t reserved = 0;
  };

  struct ACFEntryData
  {
    EntryType type;
    uint64_t originalSize;
    uint64_t compressedSize;
    uint64_t dataOffset;
    uint32_t crc32;
    uint32_t filedatetime;
    uint8_t fileattribute;
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
    
    void Create(const std::string& archivePath, 
                const std::vector<std::string>& inputPaths,
                const std::string& basePath,
                const std::string& internalBasePath);

    void CreateData(const std::string& archivePath, 
                const std::string& internalPath,
                const std::vector<uint8_t>& data);

    void ExtractAll(const std::string& archivePath,
                    const std::string& outputPath);

    void Extract(const std::string& archivePath,
                const std::vector<std::string>& archFileNames,
                const std::string& outputPath);

    std::vector<uint8_t> ExtractData(const std::string& archivePath,
                                    const std::string& archFileName);
                                    
    std::vector<std::pair<ACFEntryData, std::string>> List(const std::string& archivePath);
  };


} // namespace acf
