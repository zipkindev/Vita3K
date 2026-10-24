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
    assert(!cfg.automatic_texture_preparation);
    assert(!cfg.jit_compile_telemetry && !cfg.sync_wait_telemetry && !cfg.decoded_texture_telemetry);
    assert(cfg.parallel_texture_decode);
    Config::CurrentConfig settings;
    assert(settings.decoded_texture_cache_mb == 0);
    const auto dir = fs::temp_directory_path() / ("vita3k-cache-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    for (int mib : { 0, 128, 256, 512 }) {
        settings.decoded_texture_cache_mb = mib;
        settings.automatic_texture_preparation = mib != 0;
        settings.jit_compile_telemetry = mib == 512;
        settings.sync_wait_telemetry = mib == 256;
        settings.decoded_texture_telemetry = mib == 128;
        settings.parallel_texture_decode = mib != 0;
        assert(config::save_custom_config(settings, dir, "TEST"));
        Config::CurrentConfig loaded;
        assert(config::load_custom_config(loaded, dir, "TEST"));
        assert(loaded.decoded_texture_cache_mb == mib);
        assert(loaded.automatic_texture_preparation == (mib != 0));
        assert(loaded.jit_compile_telemetry == (mib == 512));
        assert(loaded.sync_wait_telemetry == (mib == 256));
        assert(loaded.decoded_texture_telemetry == (mib == 128));
        assert(loaded.parallel_texture_decode == (mib != 0));
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
    const auto auto_begin = xml.find(" automatic-texture-preparation=");
    assert(auto_begin != std::string::npos);
    const auto auto_value = xml.find('"', auto_begin);
    xml.erase(auto_begin, xml.find('"', auto_value + 1) - auto_begin + 1);
    fs::ofstream(path) << xml;
    Config::CurrentConfig inherited;
    inherited.decoded_texture_cache_mb = 256;
    inherited.automatic_texture_preparation = true;
    assert(config::load_custom_config(inherited, dir, "TEST"));
    assert(inherited.decoded_texture_cache_mb == 256);
    assert(inherited.automatic_texture_preparation);
    auto changed = inherited;
    changed.decoded_texture_cache_mb = 0;
    const auto restart = config::get_restart_required_settings(inherited, changed);
    assert(restart.size() == 1 && restart[0] == config::RestartRequiredSetting::DecodedTextureCache);
    changed = inherited;
    changed.automatic_texture_preparation = false;
    const auto auto_restart = config::get_restart_required_settings(inherited, changed);
    assert(auto_restart.size() == 1 && auto_restart[0] == config::RestartRequiredSetting::AutomaticTexturePreparation);
    for (auto [member, expected] : { std::pair{ &Config::CurrentConfig::jit_compile_telemetry, config::RestartRequiredSetting::JitCompileTelemetry },
             std::pair{ &Config::CurrentConfig::sync_wait_telemetry, config::RestartRequiredSetting::SyncWaitTelemetry },
             std::pair{ &Config::CurrentConfig::decoded_texture_telemetry, config::RestartRequiredSetting::DecodedTextureTelemetry },
             std::pair{ &Config::CurrentConfig::parallel_texture_decode, config::RestartRequiredSetting::ParallelTextureDecode } }) {
        changed = inherited;
        changed.*member = !(inherited.*member);
        const auto pending = config::get_restart_required_settings(inherited, changed);
        assert(pending.size() == 1 && pending[0] == expected);
    }
    fs::remove_all(dir);
}
