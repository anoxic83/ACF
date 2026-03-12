#include "wcxhead.h"
#include "acf.hh"
#include <windows.h>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <filesystem>
#include <fstream>

struct ArchiveState {
    std::unique_ptr<acf::ACFArchiver> archiver;
    std::vector<std::pair<acf::ACFEntryData, std::string>> entries;
    int currentEntryIndex = -1;
    tProcessDataProcW processDataProc = nullptr;
    tChangeVolProcW changeVolProc = nullptr;
    std::wstring archivePath;
};

tProcessDataProcW g_PackProgressProc = nullptr;

std::map<HANDLE, std::unique_ptr<ArchiveState>> g_OpenArchives;
long g_NextHandle = 0;

BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}

extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveData) {
    if (!ArchiveData) return (HANDLE)-1;

    auto state = std::make_unique<ArchiveState>();
    state->archiver = std::make_unique<acf::ACFArchiver>();
    state->archivePath = ArchiveData->ArcName;

    try {
        std::ifstream archiveFile(std::filesystem::path(state->archivePath), std::ios::binary);
        if (!archiveFile) {
            ArchiveData->OpenResult = E_EOPEN;
            return (HANDLE)-1;
        }
        acf::ACFHeader header;
        archiveFile.read(reinterpret_cast<char*>(&header), sizeof(acf::ACFHeader));
        if (header.magic != acf::ACF_MAGIC) {
            ArchiveData->OpenResult = E_UNKNOWN_FORMAT;
            return (HANDLE)-1;
        }
        archiveFile.seekg(header.centralDirOffset);
        state->entries.reserve(header.entryCount);
        for (uint64_t i = 0; i < header.entryCount; ++i) {
            acf::ACFEntryData entry;
            archiveFile.read(reinterpret_cast<char*>(&entry), sizeof(acf::ACFEntryData));
            std::string path(entry.pathLength, '\0');
            archiveFile.read(&path[0], entry.pathLength);
            state->entries.emplace_back(entry, path);
        }

    } catch (...) {
        ArchiveData->OpenResult = E_BAD_ARCHIVE;
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

    // Support for files > 4GB 
    HeaderData->UnpSize = static_cast<unsigned int>(entry.originalSize & 0xFFFFFFFF);
    HeaderData->UnpSizeHigh = static_cast<unsigned int>(entry.originalSize >> 32);
    HeaderData->PackSize = static_cast<unsigned int>(entry.compressedSize & 0xFFFFFFFF);
    HeaderData->PackSizeHigh = static_cast<unsigned int>(entry.compressedSize >> 32);
    
    HeaderData->FileCRC = entry.crc32;
    HeaderData->FileAttr = (entry.type == acf::EntryType::Directory) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall ProcessFileW(HANDLE hArcData, int Operation, WCHAR* DestPath, WCHAR* DestName) {
    if (g_OpenArchives.find(hArcData) == g_OpenArchives.end()) return E_BAD_ARCHIVE;
    auto& state = g_OpenArchives[hArcData];

    if (state->currentEntryIndex < 0 || state->currentEntryIndex >= (int)state->entries.size()) return E_BAD_ARCHIVE;

    if (Operation == PK_SKIP) return 0;
    
    // Support extracting and testing
    if (Operation != PK_EXTRACT && Operation != PK_TEST) return E_NOT_SUPPORTED;

    try {
        const auto& current = state->entries[state->currentEntryIndex];
        const auto& entry = current.first;
        const auto& path = current.second;

        // Create target directory
        if (entry.type == acf::EntryType::Directory) {
            if (Operation == PK_EXTRACT && DestName) {
                std::filesystem::create_directories(DestName);
            }
            return 0;
        }

        std::string targetPath = "";
        if (Operation == PK_EXTRACT) {
            if (!DestName || !*DestName) return E_ECREATE;
            std::filesystem::path finalDestPath(DestName);
            std::filesystem::create_directories(finalDestPath.parent_path());
            targetPath = WStringToString(DestName);
        }

        // --- ZMIANA: Poprawa Callbacku rozpakowywania ---
        // TC oczekuje w DestName pełnej ścieżki pliku na dysku. Przekazujemy DestName, jeśli istnieje.
        std::wstring wDestName = DestName ? DestName : StringToWString(path);
        
        state->archiver->SetCallback([&state, wDestName](const std::string& /*name*/, uint64_t /*proc*/, uint64_t /*total*/, uint64_t chunk, float /*gen*/) -> bool {
            if (state->processDataProc && chunk > 0) {
                // TC dla funkcji ProcessDataProc oczekuje liczby zapisanych bajtów, a nie postępu w ułamku.
                int res = state->processDataProc((WCHAR*)wDestName.c_str(), static_cast<int>(chunk));
                // Total Commander zwraca 0 jako "Kontynuuj", a inne wartości (często 1) jako "Anuluj" (Abort)
                if (res == 0) return false; // UWAGA! W API TC zwrócenie 0 oznacza PRZERWANIE z winy użytkownika.
            }
            return true; // Wszelkie inne wartości i brak processDataProc oznaczają: kontynuuj.
        });

        // Use Streaming to not saturate RAM with large files
        state->archiver->ExtractSingleFile(WStringToString(state->archivePath), path, targetPath);

    } catch (const std::exception& e) {
        std::string err(e.what());
        if (err.find("aborted") != std::string::npos) {
            return E_EABORTED; // Total Commander wyświetli "Przerwano na życzenie użytkownika"
        }
        return E_EWRITE;
    } catch (...) {
        return E_EWRITE;
    }
    return 0;
}

extern "C" __declspec(dllexport) void __stdcall SetProcessDataProcW(HANDLE hArcData, tProcessDataProcW pProcessDataW) {
    if (hArcData != (HANDLE)-1 && g_OpenArchives.count(hArcData)) {
        g_OpenArchives[hArcData]->processDataProc = pProcessDataW;
    }
    g_PackProgressProc = pProcessDataW; // Cache-ujemy go również do globalnej akcji pakowania
}

extern "C" __declspec(dllexport) int __stdcall PackFilesW(WCHAR* PackedFile, WCHAR* SubPath, WCHAR* SrcPath, WCHAR* AddList, int Flags) {
    try {
        acf::ACFArchiver archiver;
        
        // --- ZMIANA: Dodanie obsługi paska podczas tworzenia (o ile TC załadował g_PackProgressProc przed wywołaniem) ---
        archiver.SetCallback([PackedFile](const std::string& /*name*/, uint64_t /*proc*/, uint64_t /*total*/, uint64_t chunk, float /*gen*/) -> bool {
            if (g_PackProgressProc && chunk > 0) {
                // TC oczekuje, żeby dla postępu zgłaszać plik, do którego pakujemy, a rozmiar chunku to rozmiar "skompensowanych" danych
                int res = g_PackProgressProc(PackedFile, static_cast<int>(chunk));
                if (res == 0) return false; // TC anuluje akcję
            }
            return true;
        });

        std::vector<std::string> files_to_add;
        for (const WCHAR* p = AddList; *p; p += wcslen(p) + 1) {
            std::filesystem::path full_path(SrcPath);
            full_path /= p;
            files_to_add.push_back(WStringToString(full_path.wstring()));
        }

        std::string internal_path = SubPath ? WStringToString(SubPath) : "";
        archiver.Create(WStringToString(PackedFile), files_to_add, WStringToString(SrcPath), internal_path);

    } catch (const std::exception& e) {
        std::string err(e.what());
        if (err.find("aborted") != std::string::npos) {
            return E_EABORTED;
        }
        return E_ECREATE;
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

extern "C" __declspec(dllexport) int __stdcall GetPackerCaps() {
    return PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE | PK_CAPS_DELETE | PK_CAPS_BY_CONTENT;
}

extern "C" __declspec(dllexport) void __stdcall ConfigurePacker(HWND Parent, DWORD DllInstance) {}

// --- ANSI Wrappers ---

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