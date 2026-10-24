// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "testenv.h"
#include <cassert>
#include <chrono>
#include <cpu/jit_profile.h>
#include <fstream>
#include <memory>
#include <string>

using cpu::JitCompileProfile;

struct Temp {
    std::filesystem::path parent = std::filesystem::temp_directory_path() / ("vita-jit-observer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(parent); }
    ~Temp() { std::filesystem::remove_all(parent); }
    cpu::JitProfileConfig config(const char *name, size_t cap = 16) const {
        cpu::JitProfileConfig c;
        c.directory = parent / name;
        c.title = "TEST\"01";
        c.version = "1.0";
        c.executable = "eboot";
        c.regions.push_back({ "app0:module", 0, 4096, 5 });
        c.max_records = cap;
        return c;
    }
};

template <class Base>
class Observed : public Base {
public:
    std::shared_ptr<JitCompileProfile> profile;
    Dynarmic::A32::Jit *jit = nullptr;
    unsigned begins = 0, completes = 0, svcs = 0;
    bool IsJitCompileProfilingEnabled() const noexcept override { return profile && profile->enabled(); }
    void JitCompileBegin(uint64_t descriptor) noexcept override {
        ++begins;
        profile->begin(descriptor);
    }
    void JitCompileComplete(uint32_t end, size_t bytes) noexcept override {
        ++completes;
        profile->complete(end, bytes);
    }
    void JitCompileAborted() noexcept override { profile->abort(); }
    void JitCacheInvalidated() noexcept override { profile->invalidated(); }
    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        auto value = Base::MemoryReadCode(address);
        if (profile && value)
            profile->code_read(address, *value);
        return value;
    }
    void CallSVC(uint32_t swi) override {
        assert(swi == 1);
        ++svcs;
        jit->HaltExecution();
    }
};

template <class Base>
void execution_test(Temp &temp, const char *name, bool thumb) {
    Observed<Base> env, control;
    if constexpr (sizeof(typename Base::InstructionType) == 4) {
        env.code_mem = { 0xe3a00007, 0xe5810000, 0xef000001, 0xeafffffe };
    } else {
        env.code_mem = { 0x2007, 0x6008, 0xdf01, 0xe7fe };
    }
    env.PadCodeMem();
    control.code_mem = env.code_mem;
    env.profile = std::make_shared<JitCompileProfile>(temp.config(name));
    Dynarmic::A32::Jit jit{ Dynarmic::A32::UserConfig{ &env } }, plain{ Dynarmic::A32::UserConfig{ &control } };
    env.jit = &jit;
    control.jit = &plain;
    auto run = [&](auto &cpu, auto &e) {
        cpu.ClearHalt();
        cpu.Regs()[15] = 0;
        cpu.Regs()[0] = 0;
        cpu.Regs()[1] = 4096;
        cpu.SetCpsr(thumb ? 0x30 : 0x10);
        cpu.SetFpscr(0x03000000);
        e.ticks_left = 20;
        cpu.Run();
        assert(cpu.Regs()[0] == 7 && e.MemoryRead32(4096) == 7);
    };
    run(jit, env);
    run(plain, control);
    assert(jit.Regs() == plain.Regs() && jit.Cpsr() == plain.Cpsr() && jit.Fpscr() == plain.Fpscr());
    assert(env.modified_memory == control.modified_memory && env.svcs == 1 && control.svcs == 1);
    assert(control.begins == 0 && control.completes == 0);
    const auto first = env.profile->records().size();
    assert(first > 0 && env.begins == env.completes);
    assert(env.profile->records()[0].reads > 0 && env.profile->records()[0].native_bytes > 0);
    assert(((env.profile->records()[0].descriptor >> 32) & 1) == unsigned(thumb));
    run(jit, env);
    assert(env.profile->records().size() == first); // Cache hits are never observed.
    jit.InvalidateCacheRange(0, 16);
    run(jit, env);
    assert(env.profile->records().size() > first);
    assert(env.profile->records().back().epoch > 0);
    assert(env.profile->finish());
    assert(env.profile->finish());
    assert(std::filesystem::file_size(temp.parent / name / "compilations.csv") > 100);
}

static uint64_t fake_qpc = 1000;
static uint64_t fake_counter() noexcept { return fake_qpc; }

static void rolling_session_test(Temp &temp) {
    JitCompileProfile profile(temp.config("rolling", 2), { fake_counter, 1000 });
    for (unsigned i = 0; i < 5; ++i) {
        profile.begin(16 + i * 4);
        fake_qpc += 10;
        profile.complete(20 + i * 4, i + 1);
    }
    assert(profile.enabled());
    assert(profile.chronological_record(0).sequence == 3);
    assert(profile.chronological_record(1).sequence == 4);
    assert(profile.used_buckets() == 1);
    assert(profile.session_buckets()[0].count == 5);
    assert(profile.session_buckets()[0].compile_ns == 50000000);
    assert(profile.session_buckets()[0].emitted_bytes == 15);
    fake_qpc = 1000 + 4097ull * 1000;
    profile.invalidated(); // Forces summary compaction, even during idle time.
    assert(profile.bucket_width() == 2000);
    assert(profile.session_buckets()[0].count == 5);
    profile.begin(128);
    fake_qpc += 25;
    profile.complete(132, 20);
    fake_qpc = 1000 + 20000ull * 1000;
    profile.begin(256);
    fake_qpc += 1;
    profile.abort(); // More folds, preserving prior counts/maxima and aborts.
    assert(profile.finish());
    assert(!profile.enabled());
    uint64_t count = 0, ns = 0, bytes = 0, aborts = 0, invalidations = 0;
    for (size_t i = 0; i < profile.used_buckets(); ++i) {
        const auto &b = profile.session_buckets()[i];
        count += b.count;
        ns += b.compile_ns;
        bytes += b.emitted_bytes;
        aborts += b.aborted;
        invalidations += b.invalidations;
    }
    assert(count == 6 && ns == 75000000 && bytes == 35);
    assert(aborts == 1 && invalidations == 1);
    assert(profile.chronological_record(0).sequence == 4);
    assert(profile.chronological_record(1).sequence == 5);
    const auto &b = profile.session_buckets()[4097 / 8];
    assert(b.count == 1 && b.max_ns == 25000000);
    assert(profile.used_buckets() <= JitCompileProfile::bucket_limit);
    assert(std::filesystem::file_size(temp.parent / "rolling" / "session.csv") > 100);
    std::ifstream report(temp.parent / "rolling" / "summary.json");
    std::string json((std::istreambuf_iterator<char>(report)), {});
    assert(json.find("\"totalCompilations\":6") != std::string::npos);
    assert(json.find("\"overwrittenRecords\":4") != std::string::npos);
    assert(json.find("\"retainedCompileNs\":35000000") != std::string::npos);
}

int main() {
    Temp temp;
    rolling_session_test(temp);
    execution_test<ArmTestEnv>(temp, "arm", false);
    execution_test<ThumbTestEnv>(temp, "thumb", true);
    {
        JitCompileProfile bounded(temp.config("bounded", 2));
        bounded.begin(0x10);
        bounded.code_read(16, 123);
        bounded.code_read(20, 456, true);
        bounded.complete(24, 16);
        bounded.begin(0x20);
        bounded.code_read(32, 789);
        bounded.invalidated();
        bounded.invalid_read();
        for (size_t i = 0; i < JitCompileProfile::read_limit + 2; ++i)
            bounded.code_read(8192, 1);
        bounded.complete(36, 32);
        assert(bounded.enabled());

        assert(bounded.records().size() == 2);
        assert(bounded.records()[0].hook_reads == 1);
        assert(bounded.records()[1].flags == (JitCompileProfile::OutsideStartupRegion | JitCompileProfile::ReadLimit | JitCompileProfile::InvalidRead | JitCompileProfile::InvalidatedDuringCompile));
        bounded.begin(0x30);
        bounded.complete(52, 8);
        assert(bounded.records().size() == 2);
        assert(bounded.chronological_record(0).sequence == 1);
        assert(bounded.chronological_record(1).descriptor == 0x30);
        assert(bounded.finish());
        bool rejected = false;
        try {
            JitCompileProfile duplicate(temp.config("bounded"));
        } catch (...) {
            rejected = true;
        }
        assert(rejected);
    }
    {
        JitCompileProfile aborted(temp.config("abort"));
        aborted.begin(0);
        aborted.abort();
        assert(aborted.records().empty());
        assert(aborted.finish());
    }
    bool rejected = false;
    try {
        JitCompileProfile bad(temp.config("oversize", JitCompileProfile::record_limit + 1));
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
}
