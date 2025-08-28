#include "acf.hh"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <windows.h>

namespace {

std::string DosDateTimeToString(uint32_t dosDateTime) {
    if (dosDateTime == 0) return "1980-01-01 00:00:00";
    WORD dosDate = static_cast<WORD>(dosDateTime >> 16);
    WORD dosTime = static_cast<WORD>(dosDateTime & 0xFFFF);

    int year = ((dosDate >> 9) & 0x7F) + 1980;
    int month = (dosDate >> 5) & 0x0F;
    int day = dosDate & 0x1F;
    int hour = (dosTime >> 11) & 0x1F;
    int minute = (dosTime >> 5) & 0x3F;
    int second = (dosTime & 0x1F) * 2;

    std::ostringstream oss;
    oss << std::setfill('0') 
        << std::setw(4) << year << "-"
        << std::setw(2) << month << "-"
        << std::setw(2) << day << " "
        << std::setw(2) << hour << ":"
        << std::setw(2) << minute << ":"
        << std::setw(2) << second;
    return oss.str();
}

std::string AttrToString(uint8_t attr) {
    std::string s;
    s += (attr & FILE_ATTRIBUTE_READONLY) ? 'R' : '-';
    s += (attr & FILE_ATTRIBUTE_HIDDEN)   ? 'H' : '-';
    s += (attr & FILE_ATTRIBUTE_SYSTEM)   ? 'S' : '-';
    s += (attr & FILE_ATTRIBUTE_DIRECTORY) ? 'D' : '-';
    s += (attr & FILE_ATTRIBUTE_ARCHIVE)  ? 'A' : '-';
    return s;
}

} // namespace

void displayProgress(const std::string& currentFile, float currentFileProgress, float generalProgress) {
    int barWidth = 50;
    
    std::string displayFile = currentFile;
    if (displayFile.length() > 35) {
        displayFile = "..." + displayFile.substr(displayFile.length() - 32);
    }

    std::cout << "[";
    int pos = barWidth * generalProgress;
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << (generalProgress * 100.0) << "%";
    std::cout << " " << std::left << std::setw(40) << displayFile << "\r";
    std::cout.flush();
}

void printUsage() {
    std::cout << "Usage: acfcli <command> [options]"<< std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  c <archive.acf> <file/dir1> [file/dir2] ... : Create an archive." << std::endl;
    std::cout << "  l <archive.acf>                            : List contents of an archive." << std::endl;
    std::cout << "  x <archive.acf> [output_path]              : Extract an archive." << std::endl;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printUsage();
        return 1;
    }

    std::string command = argv[1];
    std::string archivePath = argv[2];
    acf::ACFArchiver archiver;
    archiver.SetCallback(displayProgress);

    try {
        if (command == "l") {
            std::cout << "Listing contents of " << archivePath << ":\n" << std::endl;
            auto fileList = archiver.List(archivePath);
            
            std::cout << std::left << std::setw(22) << "DateTime"
                      << std::setw(10) << "Attr"
                      << std::setw(14) << "Size"
                      << std::setw(12) << "CRC32"
                      << "Path" << std::endl;
            std::cout << std::string(80, '-') << std::endl;

            for (const auto& pair : fileList) {
                const auto& entry = pair.first;
                const auto& path = pair.second;
                std::cout << std::left << std::setw(22) << DosDateTimeToString(entry.filedatetime)
                          << std::setw(10) << AttrToString(entry.fileattribute)
                          << std::setw(14) << entry.originalSize
                          << std::hex << std::setw(10) << entry.crc32 << std::dec
                          << " " << path << std::endl;
            }
        } else if (command == "c") {
            if (argc < 4) {
                std::cerr << "Error: No input files specified for creation." << std::endl;
                printUsage();
                return 1;
            }
            std::vector<std::string> inputPaths;
            for (int i = 3; i < argc; ++i) {
                inputPaths.push_back(argv[i]);
            }
            
            archiver.Create(archivePath, inputPaths, ".", "");
            std::cout << std::endl; // New line after progress bar
            std::cout << "Archive created successfully." << std::endl;

        } else if (command == "x") {
            std::string outputPath = ".";
            if (argc > 3) {
                outputPath = argv[3];
            }
            archiver.ExtractAll(archivePath, outputPath);
            std::cout << std::endl; // New line after progress bar
            std::cout << "Archive extracted successfully." << std::endl;
        } else {
            std::cerr << "Error: Unknown command '" << command << "'" << std::endl;
            printUsage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cout << std::endl; // New line after progress bar in case of error
        std::cerr << "An error occurred: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
