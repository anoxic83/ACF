#include "wcxhead.h"
#include "acf.hh"
#include <windows.h>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <filesystem>
#include <fstream>

// --- Global State Management ---
struct ArchiveState {
    std::unique_ptr<acf::ACFArchiver> archiver;
    std::vector<std::pair<acf::ACFEntryData, std::string>> entries;
    int currentEntryIndex = -1;
    tProcessDataProcW processDataProc = nullptr;
    tChangeVolProcW changeVolProc = nullptr;
    std::wstring archivePath;
};

std::map<HANDLE, std::unique_ptr<ArchiveState>> g_OpenArchives;
long g_NextHandle = 0;

namespace {
    // Helper to convert DOS date/time format to FILETIME
    FILETIME DosDateTimeToFileTime(uint32_t dosDateTime) {
        FILETIME ft, lft;
        DosDateTimeToFileTime(static_cast<WORD>(dosDateTime >> 16), static_cast<WORD>(dosDateTime & 0xFFFF), &lft);
        LocalFileTimeToFileTime(&lft, &ft); // Convert to UTC for setting
        return ft;
    }
} // namespace

// --- DLL Entry Point ---
BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}

// --- Packer Implementation ---

extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveData) {
    if (!ArchiveData) return (HANDLE)-1;

    auto state = std::make_unique<ArchiveState>();
    state->archiver = std::make_unique<acf::ACFArchiver>();
    state->archivePath = ArchiveData->ArcName;

    try {
        // List() will throw on error (bad format, bad crc etc.)
        state->entries = state->archiver->List(WStringToString(state->archivePath));
    } catch (const std::exception& e) {
        // Map exception to Total Commander error codes
        std::string what = e.what();
        if (what.find("Not a valid") != std::string::npos) {
            ArchiveData->OpenResult = E_UNKNOWN_FORMAT;
        } else if (what.find("corrupted") != std::string::npos) {
            ArchiveData->OpenResult = E_BAD_ARCHIVE;
        } else {
            ArchiveData->OpenResult = E_EOPEN;
        }
        return (HANDLE)-1;
    }

    HANDLE newHandle = (HANDLE)InterlockedIncrement(&g_NextHandle);
    g_OpenArchives[newHandle] = std::move(state);
    ArchiveData->OpenResult = 0;
    return newHandle;
}

extern "C" __declspec(dllexport) int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW* HeaderData) {
    if (g_OpenArchives.find(hArcData) == g_OpenArchives.end()) return E_BAD_ARCHIVE;
    auto& state = g_OpenArchives[hArcData];
    state->currentEntryIndex++;

    if (state->currentEntryIndex >= (int)state->entries.size()) return E_END_ARCHIVE;

    const auto& current = state->entries[state->currentEntryIndex];
    const auto& entry = current.first;
    const auto& path = current.second;

    memset(HeaderData, 0, sizeof(tHeaderDataExW));
    std::wstring wpath = StringToWString(path);
    wcsncpy_s(HeaderData->FileName, wpath.c_str(), _TRUNCATE);

    HeaderData->UnpSize = entry.originalSize;
    HeaderData->PackSize = entry.compressedSize;
    HeaderData->FileCRC = entry.crc32;
    HeaderData->FileTime = entry.filedatetime;
    HeaderData->FileAttr = entry.fileattribute;
    
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall ProcessFileW(HANDLE hArcData, int Operation, WCHAR* DestPath, WCHAR* DestName) {
    if (g_OpenArchives.find(hArcData) == g_OpenArchives.end()) return E_BAD_ARCHIVE;
    auto& state = g_OpenArchives[hArcData];

    if (state->currentEntryIndex < 0 || state->currentEntryIndex >= (int)state->entries.size()) return E_BAD_ARCHIVE;

    const auto& current = state->entries[state->currentEntryIndex];
    const auto& entry = current.first;
    const auto& path = current.second;

    if (Operation == PK_SKIP) return 0;
    if (Operation != PK_EXTRACT) return E_NOT_SUPPORTED;

    try {
        std::filesystem::path finalDestPath = DestName ? DestName : (std::filesystem::path(DestPath) / StringToWString(path));

        if (entry.type == acf::EntryType::Directory) {
            std::filesystem::create_directories(finalDestPath);
        } else {
            std::filesystem::create_directories(finalDestPath.parent_path());
            std::vector<uint8_t> data = state->archiver->ExtractData(WStringToString(state->archivePath), path);
            
            std::ofstream outFile(finalDestPath, std::ios::binary | std::ios::trunc);
            if (!outFile) return E_ECREATE;
            outFile.write(reinterpret_cast<const char*>(data.data()), data.size());
            outFile.close();
        }

        // Set file time and attributes
        HANDLE hFile = CreateFileW(finalDestPath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            FILETIME ft = DosDateTimeToFileTime(entry.filedatetime);
            SetFileTime(hFile, NULL, NULL, &ft);
            CloseHandle(hFile);
        }
        SetFileAttributesW(finalDestPath.c_str(), entry.fileattribute);

        if (state->processDataProc) {
            state->processDataProc((WCHAR*)finalDestPath.c_str(), entry.originalSize);
        }
    } catch (...) {
        return E_EWRITE;
    }
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall PackFilesW(WCHAR* PackedFile, WCHAR* SubPath, WCHAR* SrcPath, WCHAR* AddList, int Flags) {
    try {
        acf::ACFArchiver archiver;
        std::vector<std::string> files_to_add;
        for (const WCHAR* p = AddList; *p; p += wcslen(p) + 1) {
            std::filesystem::path full_path(SrcPath);
            full_path /= p;
            files_to_add.push_back(WStringToString(full_path.wstring()));
        }

        std::string internal_path = SubPath ? WStringToString(SubPath) : "";
        archiver.Create(WStringToString(PackedFile), files_to_add, WStringToString(SrcPath), internal_path);
    } catch (...) {
        return E_ECREATE;
    }
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall DeleteFilesW(WCHAR* PackedFile, WCHAR* DeleteList) {
    return E_NOT_SUPPORTED;
}

extern "C" __declspec(dllexport) int __stdcall CloseArchive(HANDLE hArcData) {
    if (g_OpenArchives.find(hArcData) == g_OpenArchives.end()) return E_BAD_ARCHIVE;
    g_OpenArchives.erase(hArcData);
    return 0;
}

extern "C" __declspec(dllexport) BOOL __stdcall CanYouHandleThisFileW(WCHAR* FileName) {
    const WCHAR* ext = wcsrchr(FileName, L'.');
    return (ext && _wcsicmp(ext, L".acf") == 0);
}

extern "C" __declspec(dllexport) void __stdcall SetChangeVolProcW(HANDLE hArcData, tChangeVolProcW pChangeVolW) {
     if (g_OpenArchives.count(hArcData)) g_OpenArchives[hArcData]->changeVolProc = pChangeVolW;
}

extern "C" __declspec(dllexport) void __stdcall SetProcessDataProcW(HANDLE hArcData, tProcessDataProcW pProcessDataW) {
    if (g_OpenArchives.count(hArcData)) g_OpenArchives[hArcData]->processDataProc = pProcessDataW;
}

extern "C" __declspec(dllexport) int __stdcall GetPackerCaps() {
    return PK_CAPS_NEW | PK_CAPS_MULTIPLE | PK_CAPS_BY_CONTENT;
}

extern "C" __declspec(dllexport) void __stdcall ConfigurePacker(HWND Parent, DWORD DllInstance) {}

// --- ANSI Wrappers (not supported) ---

extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchive(tOpenArchiveData* ArchiveData) {
    if (ArchiveData) ArchiveData->OpenResult = E_NOT_SUPPORTED;
    return NULL;
}

extern "C" __declspec(dllexport) int __stdcall ReadHeader(HANDLE hArcData, tHeaderData* HeaderData) {
    return E_NOT_SUPPORTED;
}

extern "C" __declspec(dllexport) int __stdcall ProcessFile(HANDLE hArcData, int Operation, char* DestPath, char* DestName) {
    return E_NOT_SUPPORTED;
}

extern "C" __declspec(dllexport) int __stdcall PackFiles(char* PackedFile, char* SubPath, char* SrcPath, char* AddList, int Flags) {
    return E_NOT_SUPPORTED;
}

extern "C" __declspec(dllexport) int __stdcall DeleteFiles(char* PackedFile, char* DeleteList) {
    return E_NOT_SUPPORTED;
}

extern "C" __declspec(dllexport) BOOL __stdcall CanYouHandleThisFile(char* FileName) {
    const char* ext = strrchr(FileName, '.');
    return (ext && _stricmp(ext, ".acf") == 0);
}

extern "C" __declspec(dllexport) void __stdcall SetChangeVolProc(HANDLE hArcData, tChangeVolProc pChangeVol) {}
extern "C" __declspec(dllexport) void __stdcall SetProcessDataProc(HANDLE hArcData, tProcessDataProc pProcessData) {}