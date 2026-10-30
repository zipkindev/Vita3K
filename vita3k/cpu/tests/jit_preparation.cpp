// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cpu/jit_preparation.h>
#include <fstream>
#ifndef JIT_PREPARATION_STORAGE_ONLY
#include "testenv.h"
#endif

using cpu::JitPreparation;
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("vita-jit-preparation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(root); }
    ~Temp() { std::filesystem::remove_all(root); }
};
void storage(Temp &t) {
    const auto path = t.root / "TEST.pj20";
    std::array<uint8_t, 32> identity{};
    {
        JitPreparation p(path, identity, { { 0, 65536 } });
        p.begin(0);
        p.read(0, 0xe3a00007, false);
        p.complete(4, 100000);
        p.begin(4);
        p.read(4, 1, true);
        p.invalidated();
        p.complete(8, 100000);
        p.begin(8);
        for (size_t i = 0; i <= JitPreparation::read_limit; ++i)
            p.read(8, 1, false);
        p.complete(12, 100000);
        assert(p.ranked().size() == 1 && p.stats.rejected == 2);
        auto b = p.ranked()[0];
        p.replay(b);
        assert(p.replay_read(0, 0xe3a00007, false));
        assert(p.allow_emit(0, 4));
        p.end_replay();
        p.replay(b);
        assert(!p.replay_read(0, 0xe3a00008, false));
        assert(!p.allow_emit(0, 4));
        p.end_replay();
        p.replay(b);
        assert(!p.allow_emit(0, 4));
        p.end_replay();
        p.replay(b);
        assert(!p.replay_read(0, 0xe3a00007, true));
        assert(!p.allow_emit(0, 4));
        p.end_replay();
        assert(p.finish() && p.finish());
    }
    {
        JitPreparation p(path, identity, { { 0, 65536 } });
        assert(p.stats.profile_valid && p.stats.loaded == 1 && p.ranked()[0].reads[0].value == 0xe3a00007);
    }
    {
        identity[0] = 1;
        JitPreparation p(path, identity, { { 0, 65536 } });
        assert(!p.stats.profile_valid && p.ranked().empty());
        identity[0] = 0;
    }
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(60);
        f.put(0x55);
    }
    {
        JitPreparation p(path, identity, { { 0, 65536 } });
        assert(!p.stats.profile_valid && p.ranked().empty());
    }
    // Capacity remains bounded under adversarial distinct descriptors. Bad alignment/reserved bits rejected.
    JitPreparation p(t.root / "BOUND.pj20", identity, { { 0, 65536 } });
    for (uint32_t pc = 0; pc < 65536; pc += 4) {
        p.begin(pc);
        p.read(pc, 0, false);
        p.complete(pc + 4, 100000);
    }
    assert(p.ranked().size() <= JitPreparation::capacity);
    p.begin(1);
    p.read(0, 0, false);
    p.complete(4, 100000);
    p.begin(uint64_t(4) << 32);
    p.read(0, 0, false);
    p.complete(4, 100000);
    assert(p.stats.rejected >= 2);
    // Storage is pruned by age only within the owned profile namespace.
    for (int i = 0; i < 8; ++i) {
        const auto old = t.root / ("OLD" + std::to_string(i) + ".pj20");
        std::ofstream(old).put('x');
        std::filesystem::resize_file(old, 10 * 1024 * 1024);
        std::filesystem::last_write_time(old, std::filesystem::file_time_type::clock::now() - std::chrono::hours(10 - i));
    }
    std::ofstream(t.root / "keep.txt") << "unrelated";
    assert(p.finish());
    uint64_t total = 0;
    for (const auto &entry : std::filesystem::directory_iterator(t.root))
        if (entry.path().extension() == ".pj20")
            total += entry.file_size();
    assert(total <= 64 * 1024 * 1024 && !std::filesystem::exists(t.root / "OLD0.pj20") && std::filesystem::exists(t.root / "keep.txt"));
}
#ifndef JIT_PREPARATION_STORAGE_ONLY
template <class Base>
struct Env : Base {
    JitPreparation *prep = nullptr;
    Dynarmic::A32::Jit *jit = nullptr;
    unsigned svc = 0, begins = 0;
    bool IsJitCompileProfilingEnabled() const noexcept override { return prep != nullptr; }
    void JitCompileBegin(uint64_t d) noexcept override {
        ++begins;
        prep->begin(d);
    }
    void JitCompileComplete(uint32_t e, size_t) noexcept override { prep->complete(e, 100000); }
    void JitCompileAborted() noexcept override { prep->abort(); }
    void JitCacheInvalidated() noexcept override { prep->invalidated(); }
    bool AllowJitPreparedBlock(uint64_t d, uint32_t e) noexcept override { return prep && prep->allow_emit(d, e); }
    std::optional<uint32_t> MemoryReadCode(uint32_t a) override {
        const auto v = Base::MemoryReadCode(a);
        if (prep && v) {
            if (prep->is_replaying())
                return prep->replay_read(a, *v, false);
            prep->read(a, *v, false);
        }
        return v;
    }
    void CallSVC(uint32_t) override {
        ++svc;
        jit->HaltExecution();
    }
};
template <class Base>
void execution(Temp &t, bool thumb) {
    Env<Base> env;
    if constexpr (sizeof(typename Base::InstructionType) == 4)
        env.code_mem = { 0xe3a00007, 0xe5810000, 0xef000001, 0xeafffffe };
    else
        env.code_mem = { 0x2007, 0x6008, 0xdf01, 0xe7fe };
    env.PadCodeMem();
    JitPreparation prep(t.root / (thumb ? "THUMB.pj20" : "ARM.pj20"), {}, { { 0, 4096 } });
    env.prep = &prep;
    Dynarmic::A32::Jit jit{ Dynarmic::A32::UserConfig{ &env } };
    env.jit = &jit;
    auto reset = [&] {jit.ClearHalt();jit.Regs()[15]=0;jit.Regs()[0]=0;jit.Regs()[1]=4096;jit.SetCpsr(thumb?0x30:0x10);jit.SetFpscr(0x03000000);env.ticks_left=30; };
    reset();
    jit.Run();
    assert(env.svc == 1 && jit.Regs()[0] == 7);
    auto blocks = prep.ranked();
    assert(!blocks.empty());
    // Drain invalidation by running once, then use a separate fresh JIT for preparation.
    Env<Base> warm;
    warm.code_mem = env.code_mem;
    warm.prep = &prep;
    Dynarmic::A32::Jit other{ Dynarmic::A32::UserConfig{ &warm } };
    warm.jit = &other;
    const auto regs = other.Regs();
    const auto ext = other.ExtRegs();
    const auto cpsr = other.Cpsr(), fpscr = other.Fpscr();
    Env<Base> denied;
    denied.code_mem = env.code_mem;
    Dynarmic::A32::Jit default_deny{ Dynarmic::A32::UserConfig{ &denied } };
    assert(default_deny.PrepareBlock(blocks[0].descriptor) == 0 && denied.modified_memory.empty() && denied.svc == 0);
    size_t bytes = 0;
    for (const auto &b : blocks) {
        prep.replay(b);
        bytes += other.PrepareBlock(b.descriptor);
        prep.end_replay();
    }
    assert(bytes > 0 && warm.begins == 0 && warm.svc == 0 && warm.modified_memory.empty());
    assert(other.Regs() == regs && other.ExtRegs() == ext && other.Cpsr() == cpsr && other.Fpscr() == fpscr);
    for (const auto &b : blocks) {
        prep.replay(b);
        assert(other.PrepareBlock(b.descriptor) == 0);
        prep.end_replay();
    }
    other.Regs()[15] = 0;
    other.Regs()[1] = 4096;
    other.SetCpsr(thumb ? 0x30 : 0x10);
    other.SetFpscr(0x03000000);
    warm.ticks_left = 30;
    other.Run();
    assert(other.Regs()[0] == 7 && warm.svc == 1 && warm.modified_memory == env.modified_memory && warm.begins == 0);
    // Pending invalidation prevents speculative emission; normal demand honors changed code.
    other.InvalidateCacheRange(0, 16);
    if constexpr (sizeof(typename Base::InstructionType) == 4)
        warm.code_mem[0] = 0xe3a00008;
    else
        warm.code_mem[0] = 0x2008;
    prep.replay(blocks[0]);
    assert(other.PrepareBlock(blocks[0].descriptor) == 0);
    prep.end_replay();
    other.ClearHalt();
    other.Regs()[15] = 0;
    warm.ticks_left = 30;
    other.Run();
    assert(other.Regs()[0] == 8 && warm.begins > 0);
    // A fresh JIT must reject the old exact read sequence when bytes differ.
    Dynarmic::A32::Jit changed{ Dynarmic::A32::UserConfig{ &warm } };
    for (const auto &b : blocks) {
        prep.replay(b);
        assert(changed.PrepareBlock(b.descriptor) == 0);
        prep.end_replay();
    }
}
#endif
int main() {
    Temp t;
    storage(t);
#ifndef JIT_PREPARATION_STORAGE_ONLY
    execution<ArmTestEnv>(t, false);
    execution<ThumbTestEnv>(t, true);
#endif
}
