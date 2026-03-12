#include "acf.hh"
#include <stdexcept> 
#include <fstream>   
#include <filesystem>
#include <vector>
#include <string>
#include <utility>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <windows.h>
#include <memory>

// --- Utility Functions ---
std::wstring StringToWString(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), NULL, 0);
    std::wstring r(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), &r[0], len);
    return r;
}

std::string WStringToString(const std::wstring& s) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.length(), NULL, 0, NULL, NULL);
    std::string r(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.length(), &r[0], len, NULL, NULL);
    return r;
}

namespace acf
{
  // Basic CRC32 implementation
  uint32_t CalculateCRC32(uint32_t crc, const uint8_t* buf, size_t len) {
      static uint32_t table[256];
      static bool have_table = false;
      if (!have_table) {
          for (uint32_t i = 0; i < 256; i++) {
              uint32_t rem = i;
              for (int j = 0; j < 8; j++) {
                  if (rem & 1) { rem >>= 1; rem ^= 0xedb88320; }
                  else { rem >>= 1; }
              }
              table[i] = rem;
          }
          have_table = true;
      }
      crc = ~crc;
      const uint8_t* q = buf;
      for (size_t i = 0; i < len; i++) {
          crc = (crc >> 8) ^ table[(crc & 0xff) ^ *q++];
      }
      return ~crc;
  }

  // RAII Wrappers for ZSTD
  struct ZSTDCStreamDeleter { void operator()(ZSTD_CStream* s) { ZSTD_freeCStream(s); } };
  struct ZSTDDStreamDeleter { void operator()(ZSTD_DStream* s) { ZSTD_freeDStream(s); } };
  using CStreamPtr = std::unique_ptr<ZSTD_CStream, ZSTDCStreamDeleter>;
  using DStreamPtr = std::unique_ptr<ZSTD_DStream, ZSTDDStreamDeleter>;

  ACFArchiver::ACFArchiver() : m_CallbackFunc(nullptr) {}

  ACFArchiver::~ACFArchiver() {}

  void ACFArchiver::SetCallback(const CallbackFunc callbackf) {
    m_CallbackFunc = callbackf;
  }

  void ACFArchiver::Create(const std::string& archivePath, 
              const std::vector<std::string>& inputPaths,
              const std::string& basePath,
              const std::string& internalBasePath)
  {
    namespace fs = std::filesystem;

    std::ofstream archiveFile(archivePath, std::ios::binary | std::ios::trunc);
    if (!archiveFile) {
        throw std::runtime_error("Could not create archive file: " + archivePath);
    }

    ACFHeader header;
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader));

    std::vector<ACFEntryData> centralDirectory;
    std::vector<std::string> pathStrings;
    fs::path fsBasePath(basePath);

    // Using std::set with lexically_normal absolute paths eliminates duplicate files!
    std::set<fs::path> filesToProcess;
    std::set<fs::path> dirsToProcess;

    for (const auto& inputPathStr : inputPaths) {
        fs::path inputPath = fs::absolute(inputPathStr).lexically_normal();
        if (!fs::exists(inputPath)) continue;

        if (fs::is_directory(inputPath)) {
            dirsToProcess.insert(inputPath);
            for (const auto& dir_entry : fs::recursive_directory_iterator(inputPath)) {
                fs::path normPath = fs::absolute(dir_entry.path()).lexically_normal();
                if (dir_entry.is_directory()) {
                    dirsToProcess.insert(normPath);
                } else if (dir_entry.is_regular_file()) {
                    filesToProcess.insert(normPath);
                }
            }
        } else if (fs::is_regular_file(inputPath)) {
            filesToProcess.insert(inputPath);
        }
    }

    // Process Directories
    for (const auto& dirPath : dirsToProcess) {
        fs::path relativePath = fs::relative(dirPath, fs::absolute(fsBasePath));
        if (relativePath.empty() || relativePath == ".") continue;

        fs::path internalPath_fs = fs::path(internalBasePath) / relativePath;
        std::string internalPath = WStringToString(internalPath_fs.make_preferred().wstring());
        if (!internalPath.empty() && internalPath.back() != '\\') {
            internalPath += '\\';
        }

        ACFEntryData dirEntry = {EntryType::Directory, 0, 0, 0, 0, static_cast<uint16_t>(internalPath.length())};
        centralDirectory.push_back(dirEntry);
        pathStrings.push_back(internalPath);
    }

    // Optimize: Create Context and buffers ONLY ONCE for the whole archive
    CStreamPtr cstream(ZSTD_createCStream());
    if (!cstream) throw std::runtime_error("ZSTD_createCStream error");
    std::vector<char> inBuff(ZSTD_CStreamInSize());
    std::vector<char> outBuff(ZSTD_CStreamOutSize());

    float totalFiles = filesToProcess.size();
    float filesProcessed = 0;

    // Process Files
    for (const auto& filePath : filesToProcess) {
        fs::path relativePath = fs::relative(filePath, fs::absolute(fsBasePath));
        fs::path internalPath_fs = fs::path(internalBasePath) / relativePath;
        std::string internalPath = WStringToString(internalPath_fs.make_preferred().wstring());

        if (m_CallbackFunc) {
            if (!m_CallbackFunc(internalPath, 0, 0, 0, filesProcessed / totalFiles)) {
                throw std::runtime_error("Operation aborted by user.");
            }
        }

        ACFEntryData fileEntry;
        fileEntry.type = EntryType::File;
        fileEntry.originalSize = fs::file_size(filePath);
        fileEntry.dataOffset = archiveFile.tellp();
        fileEntry.pathLength = static_cast<uint16_t>(internalPath.length());
        
        uint32_t currentCrc32 = 0;

        std::ifstream inputFile(filePath, std::ios::binary);
        if (inputFile) {
            ZSTD_initCStream(cstream.get(), 19);
            
            uint64_t totalRead = 0;
            uint64_t totalCompressedSize = 0;
            for (;;) {
                inputFile.read(inBuff.data(), inBuff.size());
                size_t readCount = inputFile.gcount();
                if (readCount == 0) break;

                totalRead += readCount;
                currentCrc32 = CalculateCRC32(currentCrc32, reinterpret_cast<const uint8_t*>(inBuff.data()), readCount);

                ZSTD_inBuffer inBuffer = { inBuff.data(), readCount, 0 };
                while (inBuffer.pos < inBuffer.size) {
                    ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
                    ZSTD_compressStream(cstream.get(), &outBuffer, &inBuffer);
                    archiveFile.write(outBuff.data(), outBuffer.pos);
                    totalCompressedSize += outBuffer.pos;
                }

                // Chunk-based Callback Progress update
                if (m_CallbackFunc) {
                    float fileProg = fileEntry.originalSize > 0 ? static_cast<float>(totalRead) / fileEntry.originalSize : 1.0f;
                    float genProg = (filesProcessed + fileProg) / totalFiles;
                    if (!m_CallbackFunc(internalPath, totalRead, fileEntry.originalSize, readCount, genProg)) {
                        throw std::runtime_error("Operation aborted by user.");
                    }
                }
            }

            ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
            ZSTD_endStream(cstream.get(), &outBuffer);
            archiveFile.write(outBuff.data(), outBuffer.pos);
            totalCompressedSize += outBuffer.pos;

            fileEntry.compressedSize = totalCompressedSize;
            fileEntry.crc32 = currentCrc32;
            
            centralDirectory.push_back(fileEntry);
            pathStrings.push_back(internalPath);
        }

        filesProcessed++;
    }

    const uint64_t centralDirStartOffset = archiveFile.tellp();
    for (size_t i = 0; i < centralDirectory.size(); ++i) {
        archiveFile.write(reinterpret_cast<const char*>(&centralDirectory[i]), sizeof(ACFEntryData));
        archiveFile.write(pathStrings[i].c_str(), pathStrings[i].length());
    }

    header.centralDirOffset = centralDirStartOffset;
    header.entryCount = centralDirectory.size();
    archiveFile.seekp(0);
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader));

    if (m_CallbackFunc) m_CallbackFunc("Done.", 0, 0, 0, 1.0f);
  }

  void ACFArchiver::CreateData(const std::string& archivePath, 
              const std::string& internalPath,
              const std::vector<uint8_t>& data)
  {
    std::ofstream archiveFile(archivePath, std::ios::binary | std::ios::trunc);
    if (!archiveFile) throw std::runtime_error("Could not create archive file: " + archivePath);

    ACFHeader header;
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader));
    const uint64_t dataOffset = archiveFile.tellp();

    CStreamPtr cstream(ZSTD_createCStream());
    ZSTD_initCStream(cstream.get(), 19);

    std::vector<char> cBuff(ZSTD_CStreamOutSize());
    ZSTD_inBuffer inBuff = { data.data(), data.size(), 0 };
    
    uint64_t compressedSize = 0;
    while (inBuff.pos < inBuff.size) {
        ZSTD_outBuffer outBuff = { cBuff.data(), cBuff.size(), 0 };
        ZSTD_compressStream(cstream.get(), &outBuff, &inBuff);
        archiveFile.write(cBuff.data(), outBuff.pos);
        compressedSize += outBuff.pos;
    }

    ZSTD_outBuffer outBuff = { cBuff.data(), cBuff.size(), 0 };
    ZSTD_endStream(cstream.get(), &outBuff);
    archiveFile.write(cBuff.data(), outBuff.pos);
    compressedSize += outBuff.pos;
    
    const uint64_t centralDirOffset = archiveFile.tellp();

    ACFEntryData entryData;
    entryData.type = EntryType::File;
    entryData.originalSize = data.size();
    entryData.compressedSize = compressedSize;
    entryData.dataOffset = dataOffset;
    entryData.crc32 = CalculateCRC32(0, data.data(), data.size());
    entryData.pathLength = static_cast<uint16_t>(internalPath.length());

    archiveFile.write(reinterpret_cast<const char*>(&entryData), sizeof(ACFEntryData));
    archiveFile.write(internalPath.c_str(), internalPath.length());

    header.entryCount = 1;
    header.centralDirOffset = centralDirOffset;
    archiveFile.seekp(0);
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader));
  }

  void ACFArchiver::ExtractAll(const std::string& archivePath, const std::string& outputPath)
  {
    Extract(archivePath, {}, outputPath);
  }

  void ACFArchiver::Extract(const std::string& archivePath,
              const std::vector<std::string>& archFileNames,
              const std::string& outputPath)
  {
    namespace fs = std::filesystem;

    std::ifstream archiveFile(archivePath, std::ios::binary);
    if (!archiveFile) throw std::runtime_error("Could not open archive file: " + archivePath);

    ACFHeader header;
    archiveFile.read(reinterpret_cast<char*>(&header), sizeof(ACFHeader));
    if (header.magic != ACF_MAGIC) throw std::runtime_error("Not a valid ACF archive.");

    archiveFile.seekg(header.centralDirOffset);
    std::vector<std::pair<ACFEntryData, std::string>> allEntries;
    allEntries.reserve(header.entryCount);

    for (uint64_t i = 0; i < header.entryCount; ++i) {
        ACFEntryData entry;
        archiveFile.read(reinterpret_cast<char*>(&entry), sizeof(ACFEntryData));
        std::string path(entry.pathLength, '\0');
        archiveFile.read(&path[0], entry.pathLength);
        allEntries.emplace_back(entry, path);
    }

    bool extractAll = archFileNames.empty();
    std::unordered_set<std::string> filesToExtractSet(archFileNames.begin(), archFileNames.end());
    fs::path outputDir(outputPath);

    DStreamPtr dstream(ZSTD_createDStream());
    std::vector<char> inBuff(ZSTD_DStreamInSize());
    std::vector<char> outBuff(ZSTD_DStreamOutSize());

    float totalEntries = allEntries.size();
    float entriesProcessed = 0;

    for (const auto& pair : allEntries) {
        if (!extractAll && !filesToExtractSet.count(pair.second)) continue;

        const auto& entry = pair.first;
        const auto& path = pair.second;
        fs::path fullPath = outputDir / fs::path(path);

        if (entry.type == EntryType::Directory) {
            fs::create_directories(fullPath);
        } else if (entry.type == EntryType::File) {
            fs::create_directories(fullPath.parent_path());
            
            std::ofstream outputFile(fullPath, std::ios::binary | std::ios::trunc);
            if (!outputFile) continue;

            archiveFile.seekg(entry.dataOffset);
            ZSTD_initDStream(dstream.get());

            uint64_t totalRead = 0;
            uint64_t totalWritten = 0;
            while (totalRead < entry.compressedSize) {
                size_t toRead = std::min(static_cast<uint64_t>(inBuff.size()), entry.compressedSize - totalRead);
                archiveFile.read(inBuff.data(), toRead);
                totalRead += toRead;

                ZSTD_inBuffer inBuffer = { inBuff.data(), toRead, 0 };
                while (inBuffer.pos < inBuffer.size) {
                    ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
                    ZSTD_decompressStream(dstream.get(), &outBuffer, &inBuffer);
                    outputFile.write(outBuff.data(), outBuffer.pos);
                    totalWritten += outBuffer.pos;

                    if (m_CallbackFunc) {
                        float fileProg = entry.originalSize > 0 ? static_cast<float>(totalWritten) / entry.originalSize : 1.0f;
                        float genProg = (entriesProcessed + fileProg) / totalEntries;
                        if (!m_CallbackFunc(path, totalWritten, entry.originalSize, outBuffer.pos, genProg)) {
                            throw std::runtime_error("Operation aborted by user.");
                        }
                    }
                }
            }
        }
        entriesProcessed++;
    }
    if (m_CallbackFunc) m_CallbackFunc("Done.", 0, 0, 0, 1.0f);
  }

  void ACFArchiver::ExtractSingleFile(const std::string& archivePath, const std::string& archFileName, const std::string& destFilePath)
  {
    std::ifstream archiveFile(archivePath, std::ios::binary);
    if (!archiveFile) throw std::runtime_error("Could not open archive: " + archivePath);

    ACFHeader header;
    archiveFile.read(reinterpret_cast<char*>(&header), sizeof(ACFHeader));
    if (header.magic != ACF_MAGIC) throw std::runtime_error("Not a valid ACF archive.");

    archiveFile.seekg(header.centralDirOffset);
    ACFEntryData targetEntry;
    bool found = false;

    for (uint64_t i = 0; i < header.entryCount; ++i) {
        archiveFile.read(reinterpret_cast<char*>(&targetEntry), sizeof(ACFEntryData));
        std::string path(targetEntry.pathLength, '\0');
        archiveFile.read(&path[0], targetEntry.pathLength);
        if (path == archFileName) {
            found = true;
            break;
        }
    }

    if (!found) throw std::runtime_error("File not found in archive: " + archFileName);
    if (targetEntry.type != EntryType::File) return;

    bool isTestOperation = destFilePath.empty();
    std::ofstream outputFile;
    
    if (!isTestOperation) {
        outputFile.open(destFilePath, std::ios::binary | std::ios::trunc);
        if (!outputFile) throw std::runtime_error("Cannot create output file.");
    }

    archiveFile.seekg(targetEntry.dataOffset);
    DStreamPtr dstream(ZSTD_createDStream());
    ZSTD_initDStream(dstream.get());

    std::vector<char> inBuff(ZSTD_DStreamInSize());
    std::vector<char> outBuff(ZSTD_DStreamOutSize());

    uint64_t totalRead = 0;
    uint64_t totalWritten = 0;

    while (totalRead < targetEntry.compressedSize) {
        size_t toRead = std::min(static_cast<uint64_t>(inBuff.size()), targetEntry.compressedSize - totalRead);
        archiveFile.read(inBuff.data(), toRead);
        totalRead += toRead;

        ZSTD_inBuffer inBuffer = { inBuff.data(), toRead, 0 };
        while (inBuffer.pos < inBuffer.size) {
            ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
            ZSTD_decompressStream(dstream.get(), &outBuffer, &inBuffer);
            
            if (!isTestOperation) {
                outputFile.write(outBuff.data(), outBuffer.pos);
            }
            totalWritten += outBuffer.pos;

            if (m_CallbackFunc) {
                float fileProg = targetEntry.originalSize > 0 ? static_cast<float>(totalWritten) / targetEntry.originalSize : 1.0f;
                if (!m_CallbackFunc(archFileName, totalWritten, targetEntry.originalSize, outBuffer.pos, fileProg)) {
                    throw std::runtime_error("Operation aborted by user.");
                }
            }
        }
    }
  }

  std::vector<uint8_t> ACFArchiver::ExtractData(const std::string& archivePath, const std::string& archFileName)
  {
    std::ifstream archiveFile(archivePath, std::ios::binary);
    if (!archiveFile) throw std::runtime_error("Could not open archive file: " + archivePath);

    ACFHeader header;
    archiveFile.read(reinterpret_cast<char*>(&header), sizeof(ACFHeader));
    if (header.magic != ACF_MAGIC) throw std::runtime_error("Not a valid ACF archive.");

    archiveFile.seekg(header.centralDirOffset);
    
    ACFEntryData targetEntry;
    bool found = false;
    for (uint64_t i = 0; i < header.entryCount; ++i) {
        ACFEntryData currentEntry;
        archiveFile.read(reinterpret_cast<char*>(&currentEntry), sizeof(ACFEntryData));
        std::string path(currentEntry.pathLength, '\0');
        archiveFile.read(&path[0], currentEntry.pathLength);

        if (path == archFileName) {
            targetEntry = currentEntry;
            found = true;
            break;
        }
    }

    if (!found) throw std::runtime_error("File not found in archive: " + archFileName);
    if (targetEntry.type != EntryType::File) throw std::runtime_error("Cannot extract data from a directory entry.");

    archiveFile.seekg(targetEntry.dataOffset);

    DStreamPtr dstream(ZSTD_createDStream());
    ZSTD_initDStream(dstream.get());
    std::vector<char> inBuff(ZSTD_DStreamInSize());
    std::vector<char> outBuff(ZSTD_DStreamOutSize());

    std::vector<uint8_t> decompressedData;
    decompressedData.reserve(targetEntry.originalSize);

    uint64_t totalRead = 0;
    while (totalRead < targetEntry.compressedSize) {
        size_t toRead = std::min(static_cast<uint64_t>(inBuff.size()), targetEntry.compressedSize - totalRead);
        archiveFile.read(inBuff.data(), toRead);
        totalRead += toRead;

        ZSTD_inBuffer inBuffer = { inBuff.data(), toRead, 0 };
        while (inBuffer.pos < inBuffer.size) {
            ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
            ZSTD_decompressStream(dstream.get(), &outBuffer, &inBuffer);
            decompressedData.insert(decompressedData.end(), reinterpret_cast<uint8_t*>(outBuffer.dst), reinterpret_cast<uint8_t*>(outBuffer.dst) + outBuffer.pos);
        }
    }
    return decompressedData;
  }
                                  
  std::vector<std::string> ACFArchiver::List(const std::string& archivePath)
  {
    std::ifstream archiveFile(archivePath, std::ios::binary);
    if (!archiveFile) throw std::runtime_error("Could not open archive file: " + archivePath);

    ACFHeader header;
    archiveFile.read(reinterpret_cast<char*>(&header), sizeof(ACFHeader));
    if (header.magic != ACF_MAGIC) throw std::runtime_error("Not a valid ACF archive: " + archivePath);

    archiveFile.seekg(header.centralDirOffset);
    std::vector<std::string> fileList;
    fileList.reserve(header.entryCount);

    for (uint64_t i = 0; i < header.entryCount; ++i) {
      ACFEntryData entryData;
      archiveFile.read(reinterpret_cast<char*>(&entryData), sizeof(ACFEntryData));
      std::string path(entryData.pathLength, '\0');
      archiveFile.read(&path[0], entryData.pathLength);
      fileList.push_back(path);
    }
    return fileList;
  }
} // namespace acf