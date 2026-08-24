#include <fstream>
#include <iostream>
#include <string>
#include <cassert>

int main() {
    const std::string path = "test_roundtrip.bin";
    const std::string original = "hello wisckey, this is a byte round-trip test";

    // --- Write ---
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "Failed to open file for writing\n";
            return 1;
        }
        out.write(original.data(), original.size());
        out.close();
    }

    // --- Read back ---
    std::string result;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "Failed to open file for reading\n";
            return 1;
        }
        in.seekg(0, std::ios::end);
        std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);

        result.resize(size);
        in.read(&result[0], size);
    }

    // --- Verify ---
    assert(result == original);
    std::cout << "Round-trip OK: \"" << result << "\"\n";

    return 0;
}
