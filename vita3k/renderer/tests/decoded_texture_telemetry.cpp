// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <renderer/decoded_texture_telemetry.h>
#include <sstream>

int main() {
    using namespace std::chrono_literals;
    const auto directory = std::filesystem::temp_directory_path() / ("vita3k-telemetry-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(std::filesystem::create_directory(directory));
    const auto output = directory / "sample.csv";
    const auto path = output.string();
    renderer::DecodedTextureCache cache(1024);
    {
        renderer::DecodedTextureTelemetry disabled(cache, nullptr);
        renderer::DecodedTextureTelemetry empty(cache, "");
        renderer::DecodedTextureTelemetry bad_path(cache, (directory / "absent" / "file.csv").string().c_str(), 1ms);
    }
    {
        renderer::DecodedTextureTelemetry telemetry(cache, path.c_str(), 10ms);
        for (unsigned n = 0; n < 100; ++n)
            cache.decoded(3000000, 16, 16);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!std::filesystem::exists(output) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        assert(std::filesystem::exists(output));
        // Updates after the first published row must survive normal shutdown.
        for (unsigned n = 0; n < 100; ++n)
            cache.decoded(3000000, 16, 16);
    } // Final snapshot is flushed and writer joined before reading.
    std::ifstream file(output);
    std::string header, row, last;
    std::getline(file, header);
    assert(header.starts_with("Schema,UnixNs,SteadyNs,Qpc,QpcFrequency,Requests"));
    while (std::getline(file, row))
        last = row;
    assert(!last.empty());
    std::istringstream fields(last);
    std::vector<std::string> values;
    while (std::getline(fields, row, ','))
        values.push_back(row);
    assert(values.size() == 26);
    assert(values[0] == "1" && std::stoull(values[1]) > 0 && std::stoull(values[2]) > 0);
#ifdef _WIN32
    assert(std::stoull(values[3]) > 0 && std::stoull(values[4]) > 0);
#endif
    assert(values[14] == "200" && values[15] == "600000000" && values[16] == "200");
    assert(values[17] == "0" && values[18] == "0" && values[19] == "3000000");
    file.close();
    const auto size = std::filesystem::file_size(output);
    { // Refuse existing output rather than truncate an earlier session.
        renderer::DecodedTextureTelemetry existing(cache, path.c_str(), 1ms);
    }
    assert(std::filesystem::file_size(output) == size);
    std::filesystem::remove_all(directory);
}
