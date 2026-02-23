# ffind

A fast, multithreaded Windows-native file search utility written in C.

`ffind` recursively scans directories using the Win32 API and supports
parallel traversal across multiple CPU cores.

---

## Features

- Recursive directory traversal
- Multithreaded search (`-t`)
- Case-insensitive substring matching
- Extension filtering (`-e`)
- Full-path matching (`-f`)
- Built directly on WinAPI (`FindFirstFileW`)
- Optimized for large directory trees
- Zero external dependencies

---

## Usage
ffind <root> <needle> [options]

### Examples

Search for files containing "prime":
ffind C:\Users prime


Search only `.c` and `.h` files:
ffind C:\ source -f


Specify number of threads:
ffind C:\ prime -t 16


---

## Options

| Option | Description |
|--------|-------------|
| `-e`   | Comma-separated extension filter (e.g. `c,h,cpp`) |
| `-f`   | Match against full path instead of filename only |
| `-t N` | Number of worker threads |

---

## Build Instructions

### MSVC (Recommended)

Open:
x64 Native Tools Command Prompt for VS 2022
Then:
cl /O2 /W4 ffind.c


---

### MinGW-w64
gcc -O3 -municode -Wall -Wextra ffind.c -o ffind.exe



---

## Example Benchmark

On NVMe SSD (Windows 11, 24 threads):
Scanned 2,539,198 files
Time: ~2.99 seconds



---

## Why not just use PowerShell?

PowerShell uses .NET enumeration and is convenient,
but `ffind` is designed to be:

- Lightweight
- Fast
- Parallel
- Native
- Script-friendly

---

## License
MIT License – see LICENSE file.
