# ACF Archiver

ACF Archiver is a file archiver based on the high-performance Zstandard (zstd) compression library. It provides a core library, a command-line tool, and a Total Commander plugin for managing custom `.acf` archives.

(c)2025 **AnoXic**. MIT License.

## Features

*   **Fast Compression:** Utilizes Zstandard for high-speed compression and decompression.
*   **File & Directory Archiving:** Supports recursive archiving of files and entire directory structures.
*   **Metadata Storage:** Preserves original file metadata, including timestamps and attributes.
*   **Integrity Checking:** Uses CRC32 checksums to verify the integrity of archived files and the archive's central directory.

## Components

The project consists of three main components:

### 1. `libacf` (Core Library)

A static C++ library that provides the core functionalities for handling `.acf` archives. It exposes a simple API for:
*   Creating archives from files and directories.
*   Extracting entire archives or specific files.
*   Listing the contents of an archive.
*   Handling raw data compression and decompression.

It is designed to be easily integrated into other C++ projects that require `.acf` archive support.

### 2. `acfcli` (Command-Line Tool)

A command-line interface for managing ACF archives. It provides a convenient way to perform archiving tasks directly from the terminal.

### 3. `acfwcx` (Total Commander Plugin)

A packer plugin (`.wcx`) for the popular Total Commander file manager. This plugin seamlessly integrates `.acf` archive handling into the Total Commander user interface, allowing users to:
*   Create `.acf` archives from selected files and folders.
*   Browse the contents of `.acf` archives as if they were regular folders.
*   Extract files and folders from archives.

## Building

The project is built using CMake. To compile all components:

1.  Create a build directory: `mkdir build`
2.  Navigate into the build directory: `cd build`
3.  Configure the project with CMake: `cmake ..`
4.  Run the build command for your system (e.g., `make` on msys2 or `MSBuild` with Visual Studio).

The compiled binaries will be placed in the `bin/` and `lib/` directories in the project's root.

## `acfcli` Usage

```
Usage: acfcli <command> [options]
Commands:
  c <archive.acf> <file/dir1> [file/dir2] ... : Create an archive.
  l <archive.acf>                            : List contents of an archive.
  x <archive.acf> [output_path]              : Extract an archive.
```

**Examples:**

*   **Create an archive:**
    ```sh
    acfcli c my_archive.acf file1.txt my_folder/
    ```

*   **List the contents of an archive:**
    ```sh
    acfcli l my_archive.acf
    ```

*   **Extract an archive:**
    ```sh
    acfcli x my_archive.acf extracted_files/
    ```

# Changes Log
**v0.9.1**
- First Initial Release