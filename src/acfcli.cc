#include "acf.hh"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

void displayProgress(const std::string& currentFile, float currentFileProgress, float generalProgress) {
    int barWidth = 50;
    
    // Truncate the filename if it's too long
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
    std::cout << "] " << std::fixed << std::setprecision(1) << (generalProgress * 100.0) << " ";
    std::cout << std::left << std::setw(40) << displayFile << "\r";
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
            std::cout << "Listing contents of " << archivePath << ":" << std::endl;
            std::vector<std::string> fileList = archiver.List(archivePath);
            for (const auto& path : fileList) {
                std::cout << path << std::endl;
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