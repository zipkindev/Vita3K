// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <chrono>
#include <config/settings.h>
#include <fstream>

int main() {
    Config cfg;
    assert(cfg.decoded_texture_cache_mb == 0);
    Config::CurrentConfig settings;
    assert(settings.decoded_texture_cache_mb == 0);
    const auto dir = fs::temp_directory_path() / ("vita3k-cache-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    for (int mib : { 0, 128, 256, 512 }) {
        settings.decoded_texture_cache_mb = mib;
        assert(config::save_custom_config(settings, dir, "TEST"));
        Config::CurrentConfig loaded;
        assert(config::load_custom_config(loaded, dir, "TEST"));
        assert(loaded.decoded_texture_cache_mb == mib);
    }
    // Legacy profiles omit the new field and should inherit the global setting.
    const auto path = dir / "config/config_TEST.xml";
    fs::ifstream in(path);
    std::string xml((std::istreambuf_iterator<char>(in)), {});
    in.close();
    const auto begin = xml.find(" decoded-texture-cache-mb=");
    assert(begin != std::string::npos);
    const auto value = xml.find('"', begin);
    xml.erase(begin, xml.find('"', value + 1) - begin + 1);
    fs::ofstream(path) << xml;
    Config::CurrentConfig inherited;
    inherited.decoded_texture_cache_mb = 256;
    assert(config::load_custom_config(inherited, dir, "TEST"));
    assert(inherited.decoded_texture_cache_mb == 256);
    auto changed = inherited;
    changed.decoded_texture_cache_mb = 0;
    const auto restart = config::get_restart_required_settings(inherited, changed);
    assert(restart.size() == 1 && restart[0] == config::RestartRequiredSetting::DecodedTextureCache);
    fs::remove_all(dir);
}
