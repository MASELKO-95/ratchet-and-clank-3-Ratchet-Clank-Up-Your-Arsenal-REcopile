// Uses Wrench's GPL-3.0-or-later decompressor; see third_party/wrench/COPYING.
#include "compression.h"
#include <fstream>
#include <iterator>
#include <iostream>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 3;
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    const uint8_t magic[] = {'W', 'A', 'D'};
    auto start = std::search(bytes.begin(), bytes.end(), std::begin(magic), std::end(magic));
    if (start == bytes.end()) return 4;
    std::vector<uint8_t> output;
    if (!decompress_wad(output, WadBuffer(bytes.data() + (start - bytes.begin()), bytes.data() + bytes.size()))) return 5;
    std::ofstream dest(argv[2], std::ios::binary);
    dest.write(reinterpret_cast<const char*>(output.data()), output.size());
    if (!dest) return 6;
    std::cout << "WAD offset: " << start - bytes.begin() << "; unpacked bytes: " << output.size() << '\n';
}
