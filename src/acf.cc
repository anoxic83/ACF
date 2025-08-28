#include "acf.hh"
#include <stdexcept> 
#include <fstream>   
#include <filesystem>
#include <vector>
#include <string>
#include <utility>
#include <unordered_set>
#include <algorithm>
#include <windows.h>
#include <chrono>

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

namespace { // Anonymous namespace for internal helpers

// --- CRC32 Implementation ---
uint32_t crc32_tab[256];
void crc32_generate_table() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
        }
        crc32_tab[i] = crc;
    }
}
struct Crc32TableInitializer { Crc32TableInitializer() { crc32_generate_table(); } };
Crc32TableInitializer crc32_table_initializer;

uint32_t crc32_update(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t crc32(const void* data, size_t len) {
    return crc32_update(0, data, len);
}


// --- Date/Time Conversion ---
uint32_t FileTimeToDosDateTime(const FILETIME& ft) {
    WORD dosDate, dosTime;
    // Do not convert to local time; Total Commander likely expects UTC.
    FileTimeToDosDateTime(&ft, &dosDate, &dosTime);
    return (static_cast<uint32_t>(dosDate) << 16) | dosTime;
}

FILETIME DosDateTimeToFileTime(uint32_t dosDateTime) {
    FILETIME ft, lft;
    DosDateTimeToFileTime(static_cast<WORD>(dosDateTime >> 16), static_cast<WORD>(dosDateTime & 0xFFFF), &lft);
    LocalFileTimeToFileTime(&lft, &ft); // Convert to UTC
    return ft;
}

// --- ZSTD Stream Wrappers (RAII) ---
struct ZSTD_CStream_Deleter { void operator()(ZSTD_CStream* ptr) const { ZSTD_freeCStream(ptr); } };
using ZSTD_CStream_Ptr = std::unique_ptr<ZSTD_CStream, ZSTD_CStream_Deleter>;

struct ZSTD_DStream_Deleter { void operator()(ZSTD_DStream* ptr) const { ZSTD_freeDStream(ptr); } };
using ZSTD_DStream_Ptr = std::unique_ptr<ZSTD_DStream, ZSTD_DStream_Deleter>;

} // namespace

namespace acf
{
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
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader)); // Placeholder

    std::vector<ACFEntryData> centralDirectory;
    std::vector<std::string> pathStrings;
    
    fs::path fsBasePath(basePath);
    std::unordered_set<fs::path> processedPaths;

    std::vector<fs::path> filesToProcess;
    std::vector<fs::path> dirsToProcess;

    for (const auto& inputPathStr : inputPaths) {
        fs::path inputPath(inputPathStr);
        if (!fs::exists(inputPath) || processedPaths.count(inputPath)) continue;

        if (fs::is_directory(inputPath)) {
            if (processedPaths.find(inputPath) == processedPaths.end()) {
                dirsToProcess.push_back(inputPath);
                processedPaths.insert(inputPath);
            }
            for (const auto& dir_entry : fs::recursive_directory_iterator(inputPath)) {
                 if (processedPaths.count(dir_entry.path())) continue;
                if (dir_entry.is_directory()) {
                    dirsToProcess.push_back(dir_entry.path());
                } else if (dir_entry.is_regular_file()) {
                    filesToProcess.push_back(dir_entry.path());
                }
                processedPaths.insert(dir_entry.path());
            }
        } else if (fs::is_regular_file(inputPath)) {
            filesToProcess.push_back(inputPath);
            processedPaths.insert(inputPath);
        }
    }
    
    std::sort(dirsToProcess.begin(), dirsToProcess.end());
    std::sort(filesToProcess.begin(), filesToProcess.end());

    for (const auto& dirPath : dirsToProcess) {
        fs::path relativePath = fs::relative(dirPath, fsBasePath);
        fs::path internalPath_fs = fs::path(internalBasePath) / relativePath;
        std::string internalPath = WStringToString(internalPath_fs.make_preferred().wstring());
        if (!internalPath.empty() && internalPath.back() != '\\') {
            internalPath += '\\';
        }

        ACFEntryData dirEntry{};
        dirEntry.type = EntryType::Directory;
        
        auto ftime = fs::last_write_time(dirPath);
        ULARGE_INTEGER uli;
        uli.QuadPart = ftime.time_since_epoch().count();
        FILETIME ft;
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        dirEntry.filedatetime = FileTimeToDosDateTime(ft);
        
        dirEntry.fileattribute = GetFileAttributesW(dirPath.c_str());
        dirEntry.pathLength = static_cast<uint16_t>(internalPath.length());
        
        centralDirectory.push_back(dirEntry);
        pathStrings.push_back(internalPath);
    }

    float totalFiles = filesToProcess.size();
    float filesProcessed = 0;

    for (const auto& filePath : filesToProcess) {
        fs::path relativePath = fs::relative(filePath, fsBasePath);
        fs::path internalPath_fs = fs::path(internalBasePath) / relativePath;
        std::string internalPath = WStringToString(internalPath_fs.make_preferred().wstring());

        if (m_CallbackFunc) {
            m_CallbackFunc(internalPath, 0.0f, filesProcessed / totalFiles);
        }

        ACFEntryData fileEntry{};
        fileEntry.type = EntryType::File;
        fileEntry.originalSize = fs::file_size(filePath);
        fileEntry.dataOffset = archiveFile.tellp();
        
        FILETIME ft;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(filePath.generic_wstring().c_str(), GetFileExInfoStandard, &fad))
        {
            ft = fad.ftLastWriteTime;
        }
        else
        {
            ft.dwLowDateTime = ft.dwHighDateTime = 0;
        }
        fileEntry.filedatetime = FileTimeToDosDateTime(ft);
        fileEntry.fileattribute = GetFileAttributesW(filePath.c_str());
        fileEntry.pathLength = static_cast<uint16_t>(internalPath.length());

        std::ifstream inputFile(filePath, std::ios::binary);
        if (!inputFile) continue;

        ZSTD_CStream_Ptr cstream(ZSTD_createCStream());
        ZSTD_initCStream(cstream.get(), 9);
        
        size_t const inBuffSize = ZSTD_CStreamInSize();
        std::vector<char> inBuff(inBuffSize);
        size_t const outBuffSize = ZSTD_CStreamOutSize();
        std::vector<char> outBuff(outBuffSize);

        uint64_t totalCompressedSize = 0;
        uint32_t crc = 0;
        for (;;) {
            inputFile.read(inBuff.data(), inBuff.size());
            size_t readCount = inputFile.gcount();
            if (readCount == 0) break;

            crc = crc32_update(crc, inBuff.data(), readCount);

            ZSTD_inBuffer inBuffer = { inBuff.data(), readCount, 0 };
            while (inBuffer.pos < inBuffer.size) {
                ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
                ZSTD_compressStream(cstream.get(), &outBuffer, &inBuffer);
                archiveFile.write(outBuff.data(), outBuffer.pos);
                totalCompressedSize += outBuffer.pos;
            }
        }

        ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
        size_t const remaining = ZSTD_endStream(cstream.get(), &outBuffer);
        if (ZSTD_isError(remaining)) {
            throw std::runtime_error("ZSTD_endStream error");
        }
        archiveFile.write(outBuff.data(), outBuffer.pos);
        totalCompressedSize += outBuffer.pos;
        
        fileEntry.crc32 = crc;
        fileEntry.compressedSize = totalCompressedSize;
        centralDirectory.push_back(fileEntry);
        pathStrings.push_back(internalPath);

        filesProcessed++;
        if (m_CallbackFunc) {
            m_CallbackFunc(internalPath, 1.0f, filesProcessed / totalFiles);
        }
    }

    header.centralDirOffset = archiveFile.tellp();
    header.entryCount = centralDirectory.size();
    
    std::vector<char> centralDirBuffer;
    for (size_t i = 0; i < centralDirectory.size(); ++i) {
        const char* entry_ptr = reinterpret_cast<const char*>(&centralDirectory[i]);
        centralDirBuffer.insert(centralDirBuffer.end(), entry_ptr, entry_ptr + sizeof(ACFEntryData));
        centralDirBuffer.insert(centralDirBuffer.end(), pathStrings[i].begin(), pathStrings[i].end());
    }
    archiveFile.write(centralDirBuffer.data(), centralDirBuffer.size());

    header.centralDirCRC32 = crc32(centralDirBuffer.data(), centralDirBuffer.size());

    archiveFile.seekp(0);
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader));

    if (m_CallbackFunc) {
        m_CallbackFunc("Done.", 1.0f, 1.0f);
    }
  }

  void ACFArchiver::CreateData(const std::string& archivePath, 
              const std::string& internalPath,
              const std::vector<uint8_t>& data)
  {
    std::ofstream archiveFile(archivePath, std::ios::binary | std::ios::trunc);
    if (!archiveFile) {
      throw std::runtime_error("Could not create archive file: " + archivePath);
    }

    ACFHeader header;
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader));

    const uint64_t dataOffset = archiveFile.tellp();

    ZSTD_CStream_Ptr cstream(ZSTD_createCStream());
    if (!cstream) { throw std::runtime_error("ZSTD_createCStream() error"); }
    if (ZSTD_isError(ZSTD_initCStream(cstream.get(), 9))) {
        throw std::runtime_error("ZSTD_initCStream() error");
    }

    size_t const cBuffSize = ZSTD_CStreamOutSize();
    std::vector<char> cBuff(cBuffSize);
    ZSTD_inBuffer inBuff = { data.data(), data.size(), 0 };
    
    uint64_t compressedSize = 0;
    while (inBuff.pos < inBuff.size) {
        ZSTD_outBuffer outBuff = { cBuff.data(), cBuff.size(), 0 };
        if (ZSTD_isError(ZSTD_compressStream(cstream.get(), &outBuff, &inBuff))) {
            throw std::runtime_error("ZSTD_compressStream() error");
        }
        archiveFile.write(cBuff.data(), outBuff.pos);
        compressedSize += outBuff.pos;
    }

    ZSTD_outBuffer outBuff = { cBuff.data(), cBuff.size(), 0 };
    if (ZSTD_isError(ZSTD_endStream(cstream.get(), &outBuff))) {
        throw std::runtime_error("ZSTD_endStream() error");
    }
    archiveFile.write(cBuff.data(), outBuff.pos);
    compressedSize += outBuff.pos;
    
    ACFEntryData entryData{};
    entryData.type = EntryType::File;
    entryData.originalSize = data.size();
    entryData.compressedSize = compressedSize;
    entryData.dataOffset = dataOffset;
    entryData.crc32 = crc32(data.data(), data.size());
    
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    entryData.filedatetime = FileTimeToDosDateTime(ft);
    entryData.fileattribute = FILE_ATTRIBUTE_ARCHIVE;
    entryData.pathLength = static_cast<uint16_t>(internalPath.length());

    header.centralDirOffset = archiveFile.tellp();
    header.entryCount = 1;

    std::vector<char> centralDirBuffer;
    const char* entry_ptr = reinterpret_cast<const char*>(&entryData);
    centralDirBuffer.insert(centralDirBuffer.end(), entry_ptr, entry_ptr + sizeof(ACFEntryData));
    centralDirBuffer.insert(centralDirBuffer.end(), internalPath.begin(), internalPath.end());
    
    archiveFile.write(centralDirBuffer.data(), centralDirBuffer.size());
    header.centralDirCRC32 = crc32(centralDirBuffer.data(), centralDirBuffer.size());

    archiveFile.seekp(0);
    archiveFile.write(reinterpret_cast<const char*>(&header), sizeof(ACFHeader));
  }

  void ACFArchiver::ExtractAll(const std::string& archivePath,
                  const std::string& outputPath)
  {
    auto entries = List(archivePath); // List() also validates the archive
    
    namespace fs = std::filesystem;
    fs::path outputDir(outputPath);
    float totalEntries = entries.size();
    float entriesProcessed = 0;

    for (const auto& pair : entries) {
        const auto& entry = pair.first;
        const auto& path = pair.second;
        fs::path fullPath = outputDir / fs::path(path);

        if (m_CallbackFunc) {
            m_CallbackFunc(path, 0.0f, entriesProcessed / totalEntries);
        }

        if (entry.type == EntryType::Directory) {
            fs::create_directories(fullPath);
        } else if (entry.type == EntryType::File) {
            fs::create_directories(fullPath.parent_path());
            
            std::vector<uint8_t> data = ExtractData(archivePath, path); // CRC is checked inside
            std::ofstream outputFile(fullPath, std::ios::binary | std::ios::trunc);
            if (outputFile) {
                outputFile.write(reinterpret_cast<const char*>(data.data()), data.size());
            }
        }

        // Set attributes and time
        FILETIME ft = DosDateTimeToFileTime(entry.filedatetime);
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        auto ftime = std::filesystem::file_time_type(std::filesystem::file_time_type::duration(uli.QuadPart));
        std::error_code ec;
        fs::last_write_time(fullPath, ftime, ec); // Ignore error
        SetFileAttributesW(fullPath.c_str(), entry.fileattribute);

        entriesProcessed++;
        if (m_CallbackFunc) {
            m_CallbackFunc(path, 1.0f, entriesProcessed / totalEntries);
        }
    }
    if (m_CallbackFunc) {
        m_CallbackFunc("Done.", 1.0f, 1.0f);
    }
  }

  void ACFArchiver::Extract(const std::string& archivePath,
              const std::vector<std::string>& archFileNames,
              const std::string& outputPath)
  {
    auto allEntries = List(archivePath);
    std::unordered_set<std::string> filesToExtractSet(archFileNames.begin(), archFileNames.end());
    namespace fs = std::filesystem;
    fs::path outputDir(outputPath);

    std::vector<std::pair<ACFEntryData, std::string>> entriesToExtract;
    for(const auto& pair : allEntries) {
        if (filesToExtractSet.count(pair.second)) {
            entriesToExtract.push_back(pair);
        }
    }

    float totalEntries = entriesToExtract.size();
    float entriesProcessed = 0;

    for (const auto& pair : entriesToExtract) {
        const auto& entry = pair.first;
        const auto& path = pair.second;
        fs::path fullPath = outputDir / fs::path(path);

        if (m_CallbackFunc) {
            m_CallbackFunc(path, 0.0f, entriesProcessed / totalEntries);
        }

        if (entry.type == EntryType::Directory) {
            fs::create_directories(fullPath);
        } else if (entry.type == EntryType::File) {
            fs::create_directories(fullPath.parent_path());
            
            std::vector<uint8_t> data = ExtractData(archivePath, path);
            std::ofstream outputFile(fullPath, std::ios::binary | std::ios::trunc);
            if (outputFile) {
                outputFile.write(reinterpret_cast<const char*>(data.data()), data.size());
            }
        }

        FILETIME ft = DosDateTimeToFileTime(entry.filedatetime);
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        auto ftime = std::filesystem::file_time_type(std::filesystem::file_time_type::duration(uli.QuadPart));
        std::error_code ec;
        fs::last_write_time(fullPath, ftime, ec);
        SetFileAttributesW(fullPath.c_str(), entry.fileattribute);

        entriesProcessed++;
        if (m_CallbackFunc) {
            m_CallbackFunc(path, 1.0f, entriesProcessed / totalEntries);
        }
    }
    if (m_CallbackFunc) {
        m_CallbackFunc("Done.", 1.0f, 1.0f);
    }
  }

  std::vector<uint8_t> ACFArchiver::ExtractData(const std::string& archivePath,
                                  const std::string& archFileName)
  {
    std::ifstream archiveFile(archivePath, std::ios::binary);
    if (!archiveFile) {
        throw std::runtime_error("Could not open archive file: " + archivePath);
    }

    ACFHeader header;
    archiveFile.read(reinterpret_cast<char*>(&header), sizeof(ACFHeader));
    if (header.magic != ACF_MAGIC) {
        throw std::runtime_error("Not a valid ACF archive.");
    }

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

    if (!found) {
        throw std::runtime_error("File not found in archive: " + archFileName);
    }
    if (targetEntry.type != EntryType::File) {
        throw std::runtime_error("Cannot extract data from a directory entry: " + archFileName);
    }

    archiveFile.seekg(targetEntry.dataOffset);

    ZSTD_DStream_Ptr dstream(ZSTD_createDStream());
    if (!dstream) { throw std::runtime_error("ZSTD_createDStream() error"); }
    ZSTD_initDStream(dstream.get());

    std::vector<uint8_t> decompressedData;
    decompressedData.reserve(targetEntry.originalSize);
    
    size_t const inBuffSize = ZSTD_DStreamInSize();
    std::vector<char> inBuff(inBuffSize);
    size_t const outBuffSize = ZSTD_DStreamOutSize();
    std::vector<char> outBuff(outBuffSize);

    uint64_t totalRead = 0;
    while (totalRead < targetEntry.compressedSize) {
        size_t toRead = std::min(static_cast<uint64_t>(inBuff.size()), targetEntry.compressedSize - totalRead);
        archiveFile.read(inBuff.data(), toRead);
        totalRead += toRead;

        ZSTD_inBuffer inBuffer = { inBuff.data(), toRead, 0 };
        while (inBuffer.pos < inBuffer.size) {
            ZSTD_outBuffer outBuffer = { outBuff.data(), outBuff.size(), 0 };
            size_t const ret = ZSTD_decompressStream(dstream.get(), &outBuffer, &inBuffer);
            if (ZSTD_isError(ret)) {
                throw std::runtime_error("ZSTD_decompressStream error");
            }
            decompressedData.insert(decompressedData.end(), reinterpret_cast<uint8_t*>(outBuffer.dst), reinterpret_cast<uint8_t*>(outBuffer.dst) + outBuffer.pos);
        }
    }

    if (crc32(decompressedData.data(), decompressedData.size()) != targetEntry.crc32) {
        throw std::runtime_error("CRC32 mismatch for file: " + archFileName);
    }

    return decompressedData;
  }
                                  
  std::vector<std::pair<ACFEntryData, std::string>> ACFArchiver::List(const std::string& archivePath)
  {
    std::ifstream archiveFile(archivePath, std::ios::binary);
    if (!archiveFile) {
      throw std::runtime_error("Could not open archive file: " + archivePath);
    }

    ACFHeader header;
    archiveFile.read(reinterpret_cast<char*>(&header), sizeof(ACFHeader));
    if (header.magic != ACF_MAGIC) {
      throw std::runtime_error("Not a valid ACF archive: " + archivePath);
    }

    archiveFile.seekg(header.centralDirOffset);
    
    size_t cdSize = 0;
    if (archiveFile.seekg(0, std::ios::end)) {
        cdSize = static_cast<size_t>(archiveFile.tellg()) - header.centralDirOffset;
    }
    archiveFile.seekg(header.centralDirOffset);

    std::vector<char> centralDirBuffer(cdSize);
    archiveFile.read(centralDirBuffer.data(), cdSize);
    if (crc32(centralDirBuffer.data(), cdSize) != header.centralDirCRC32) {
        throw std::runtime_error("Central directory CRC32 mismatch. Archive is likely corrupted.");
    }

    std::vector<std::pair<ACFEntryData, std::string>> fileList;
    fileList.reserve(header.entryCount);

    const char* buffer_ptr = centralDirBuffer.data();
    const char* buffer_end = centralDirBuffer.data() + cdSize;

    for (uint64_t i = 0; i < header.entryCount; ++i)
    {
      if (buffer_ptr + sizeof(ACFEntryData) > buffer_end) break;
      ACFEntryData entryData;
      memcpy(&entryData, buffer_ptr, sizeof(ACFEntryData));
      buffer_ptr += sizeof(ACFEntryData);
      
      if (buffer_ptr + entryData.pathLength > buffer_end) break;
      std::string path(buffer_ptr, entryData.pathLength);
      buffer_ptr += entryData.pathLength;
      
      fileList.emplace_back(entryData, path);
    }

    return fileList;
  }

} // namespace acf
