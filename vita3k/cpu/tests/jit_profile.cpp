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

int main() {
    Temp temp;
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
        assert(!bounded.enabled());
        bounded.begin(0x30);
        bounded.complete(52, 8);
        assert(bounded.records().size() == 2);
        assert(bounded.records()[0].hook_reads == 1);
        assert(bounded.records()[1].flags == (JitCompileProfile::OutsideStartupRegion | JitCompileProfile::ReadLimit | JitCompileProfile::InvalidRead | JitCompileProfile::InvalidatedDuringCompile));
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
