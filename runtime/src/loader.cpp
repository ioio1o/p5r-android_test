// Module loader and dynamic linker (what rtld does on Horizon), indirect-call dispatch and HLE thunks.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nx/runtime.h>

#include "kernel.h"

#include "platform.h"
#if NX_NCE
#include "nce/nce.h"
#endif

namespace nx {

bool g_trace = false;
std::atomic<uint64_t> g_frame{0};

namespace {

std::mutex g_log_lock;
FILE* g_log_file = nullptr;

std::vector<LoadedModule> g_modules;
std::unordered_map<std::string, uint64_t> g_exports;

struct Thunk {
    std::string name;
    HleFn fn;
};
std::vector<Thunk> g_thunks;
std::unordered_map<std::string, uint64_t> g_thunk_by_name;

std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}

thread_local const char* t_missing_name = nullptr;

}  // namespace

// ---- logging --------------------------------------------------------------------------------------------------

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lk(g_log_lock);
    if (!g_log_file && plat::kDiagnostics) {  // NX_RELEASE: the system log only
        const char* path = std::getenv("NX_LOG");
#if defined(__ANDROID__)
        std::string def = plat::data_root() + "/nx.log";  // next to the data, readable from the phone
#else
        std::string def = plat::data_root() + "/../nx.log";
#endif
        std::string file = path ? path : def;
        // Keep the previous session's log (a problem report should survive reopening the game).
        std::remove((file + ".prev").c_str());
        std::rename(file.c_str(), (file + ".prev").c_str());
        g_log_file = std::fopen(file.c_str(), "w");
    }
    plat::log_line(buf);
    if (g_log_file) {
        std::fprintf(g_log_file, "%s\n", buf);
        std::fflush(g_log_file);
    }
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
    plat::exit_process(1);
}

// ---- HLE registry ---------------------------------------------------------------------------------------------

HleRegistrar::HleRegistrar(const char* name, HleFn fn) { registry()[name] = fn; }

HleFn hle_lookup(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

static void missing_stub(CPU& c) {
    // Name comes from the thunk that dispatched here.
    log("missing import called: %s (lr %s) -> 0", t_missing_name, describe(c.x[30]).c_str());
    c.x[0] = 0;
}

uint64_t hle_address_or(const char* name, HleFn fallback) {
    static std::mutex lock;  // NVN hands out procs by name at run time, from any thread
    std::lock_guard lk(lock);
    // Other threads index g_thunks while we add to it: never reallocate (one entry per 16 bytes of the region).
    if (g_thunks.capacity() < kThunkSize / 16) g_thunks.reserve(kThunkSize / 16);
    auto it = g_thunk_by_name.find(name);
    if (it != g_thunk_by_name.end()) return it->second;
    if (g_thunks.size() == kThunkSize / 16) fatal("out of HLE thunks");
    HleFn fn = hle_lookup(name);
    uint64_t addr = kThunkBase + g_thunks.size() * 16;
    g_thunks.push_back({name, fn ? fn : fallback});
    g_thunk_by_name[name] = addr;
    return addr;
}

uint64_t hle_address(const char* name) { return hle_address_or(name, missing_stub); }

const char* hle_current_name() { return t_missing_name; }

uint64_t hook_function(uint64_t addr, HleFn fn) {
#if NX_NCE
    return nce::hook(addr, fn);
#else
    const LoadedModule* m = module_at(addr);
    const uint64_t off = m ? addr - m->base : 0;
    if (!m || !m->table || off >= m->info->text_size || !m->table[off / 4]) return 0;
    const Fn original = m->table[off / 4];
    m->table[off / 4] = fn;
    char name[64];
    std::snprintf(name, sizeof name, "__original_%llx", (unsigned long long)addr);
    return hle_address_or(name, original);  // the original, as a thunk of its own
#endif
}

// ---- dispatch -------------------------------------------------------------------------------------------------

const LoadedModule* module_at(uint64_t addr) {
    for (auto& m : g_modules)
        if (addr - m.base < m.size) return &m;
    return nullptr;
}

std::string describe(uint64_t addr) {
    char buf[512];
    if (addr - kThunkBase < kThunkSize) {
        size_t i = (addr - kThunkBase) / 16;
        snprintf(buf, sizeof buf, "hle:%s", i < g_thunks.size() ? g_thunks[i].name.c_str() : "?");
        return buf;
    }
    const LoadedModule* m = module_at(addr);
    if (!m) {
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)addr);
        return buf;
    }
    uint64_t off = addr - m->base;
    // Nearest preceding function symbol.
    const Symbol* best = nullptr;
    for (size_t k = 0; k < m->info->symbol_count; k++) {
        const Symbol& s = m->info->symbols[k];
        if (s.imported || s.type != 2 || s.value > off) continue;
        if (!best || s.value > best->value) best = &s;
    }
    if (best && off - best->value < 0x10000)
        snprintf(buf, sizeof buf, "%s+%llx (%s+%llx)", m->info->name, (unsigned long long)off, best->name,
                 (unsigned long long)(off - best->value));
    else
        snprintf(buf, sizeof buf, "%s+%llx", m->info->name, (unsigned long long)off);
    return buf;
}

void backtrace(CPU& c);

// Indirect calls (every blr of the game) look the target up here: the modules' text ranges in a small flat array.
struct FastModule {
    uint64_t base, text_size;
    const Fn* table;
};
FastModule g_fast[4];
size_t g_fast_count = 0;

// The HLE function of thunk `index`, on c (call() for thunk addresses; nce_dispatch for the guest's calls to them).
void hle_invoke(CPU& c, size_t index) {
    if (index >= g_thunks.size()) fatal("call to an HLE thunk never handed out (%zu), lr %s", index, describe(c.x[30]).c_str());
    const Thunk& th = g_thunks[index];
    if (g_trace) log("hle %s", th.name.c_str());
    const char* prev = t_missing_name;
    t_missing_name = th.name.c_str();
    KThread* kt = self_thread();
    if (kt) kt->hle.store(t_missing_name, std::memory_order_relaxed);
    th.fn(c);
    t_missing_name = prev;
    if (kt) kt->hle.store(prev, std::memory_order_relaxed);
}

#if NX_NCE
// Native execution: guest code runs on the host CPU (nce.cpp); only the thunks are host functions.
void call(CPU& c, uint64_t t) {
    if (t - kThunkBase < kThunkSize) return hle_invoke(c, (t - kThunkBase) / 16);
    nce::enter(c, t);
}
#else
void call(CPU& c, uint64_t t) {
    for (size_t k = 0; k < g_fast_count; k++) {
        const FastModule& m = g_fast[k];
        uint64_t off = t - m.base;
        if (off < m.text_size) [[likely]] {
            Fn fn = m.table[off >> 2];
            if (!fn) [[unlikely]] {
                for (int k = 0; k < 31; k += 4)
                    log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k],
                        (unsigned long long)c.x[k + 1], (unsigned long long)c.x[k + 2],
                        (unsigned long long)(k + 3 < 31 ? c.x[k + 3] : c.x[31]));
                backtrace(c);
                fatal("call into the middle of a function: %s (lr %s)", describe(t).c_str(), describe(c.x[30]).c_str());
            }
            fn(c);
            return;
        }
    }
    uint64_t off = t - kThunkBase;
    if (off < kThunkSize && off / 16 < g_thunks.size()) return hle_invoke(c, off / 16);
    fatal("call to %llx (%s), lr %s", (unsigned long long)t, describe(t).c_str(), describe(c.x[30]).c_str());
}
#endif

void trace_sp(CPU& c, uint64_t pc) {
    log("sp %llx x19 %llx at %s", (unsigned long long)c.x[31], (unsigned long long)c.x[19], describe(pc).c_str());
}

[[noreturn]] void unimpl(CPU& c, uint64_t pc, const char* what) {
    for (int k = 0; k < 31; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)(k + 3 < 32 ? c.x[k + 3] : 0));
    backtrace(c);
    fatal("unimplemented instruction at %s: %s (lr %s)", describe(pc).c_str(), what, describe(c.x[30]).c_str());
}

// ---- loading and linking --------------------------------------------------------------------------------------

uint64_t find_symbol(const char* name) {
    auto it = g_exports.find(name);
    return it == g_exports.end() ? 0 : it->second;
}

// HLE functions that only make sense for recompiled code: natively the game's own run (longjmp unwinds the guest
// stack itself; the SDK switches fibers by switching guest stacks, which native code follows).
static bool recompiled_only(const char* name) {
#if NX_NCE
    static const char* const kNames[] = {"longjmp", "_longjmp", "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi",
                                         "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE", "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"};
    for (const char* n : kNames)
        if (std::strcmp(n, name) == 0) return true;
#else
    (void)name;
#endif
    return false;
}

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir,
                  const uint8_t* const* images, size_t optional_index) {
    const size_t first_new_module = g_modules.size();
    const bool has_optional = optional_index < count;

#if NX_NCE
    LoadedModule optional_lm{};
    bool optional_ready = false;

    // Map and NCE-prepare the optional module before publishing its exports.
    // This lets a failed optional module disappear without affecting main/sdk.
    if (has_optional) {
        const ModuleInfo* mi = mods[optional_index];
        if (!mi) fatal("module loader: optional module descriptor at index %zu is null",
                       optional_index);

        // NCE preparation makes optional text RX before relocation.
        // Skip the optional module if any relocation would write into text.
        bool text_relocation = false;
        for (size_t k = 0; k < mi->reloc_count; k++) {
            const uint64_t offset = mi->relocs[k].offset;
            if (offset >= mi->text && offset - mi->text < mi->text_size) {
                text_relocation = true;
                break;
            }
        }

        constexpr uint64_t kOptionalSlotSize = 0x10000000;
        constexpr uint64_t kNceRegionSize = 0x40000;

        const bool arithmetic_ok =
            mi->data <= UINT64_MAX - mi->data_size &&
            mi->bss <= UINT64_MAX - mi->bss_size;

        const uint64_t image_size = arithmetic_ok ? mi->data + mi->data_size : 0;
        const uint64_t bss_end = arithmetic_ok ? mi->bss + mi->bss_size : 0;
        const bool page_alignment_ok =
            arithmetic_ok && bss_end <= UINT64_MAX - 0xFFF;
        const uint64_t size =
            page_alignment_ok ? ((bss_end + 0xFFF) & ~uint64_t(0xFFF)) : 0;
        const bool nce_alignment_ok =
            page_alignment_ok && size <= UINT64_MAX - 0xFFFF;
        const uint64_t nce_offset =
            nce_alignment_ok ? ((size + 0xFFFF) & ~uint64_t(0xFFFF)) : 0;
        const bool slot_arithmetic_ok =
            nce_alignment_ok &&
            mi->base <= UINT64_MAX - kOptionalSlotSize &&
            mi->base + nce_offset <= UINT64_MAX - kNceRegionSize;

        if (text_relocation) {
            log("exefs: optional %s skipped: relocation targets its text section", mi->name);
        } else if (!slot_arithmetic_ok ||
            image_size > size || nce_offset > kOptionalSlotSize ||
            kNceRegionSize > kOptionalSlotSize - nce_offset) {
            log("exefs: optional %s skipped: image/BSS/NCE region exceeds its 256 MiB slot",
                mi->name);
        } else if (!commit(mi->base, size)) {
            log("exefs: optional %s skipped: cannot commit module memory", mi->name);
        } else {
            bool copied = true;
            if (images) {
                std::memcpy(reinterpret_cast<void*>(mi->base), images[optional_index], image_size);
            } else {
                const std::string path = data_dir + "/" + mi->name + ".bin";
                std::ifstream f(path, std::ios::binary);
                if (!f) {
                    log("exefs: optional %s skipped: missing %s", mi->name, path.c_str());
                    copied = false;
                } else {
                    f.read(reinterpret_cast<char*>(mi->base), std::streamsize(image_size));
                    if (uint64_t(f.gcount()) != image_size) {
                        log("exefs: optional %s skipped: incomplete image", mi->name);
                        copied = false;
                    }
                }
            }

            if (copied) {
                vm_set(mi->base + mi->text, mi->text_size, kMemCode, 5);
                vm_set(mi->base + mi->ro, mi->ro_size, kMemCode, 1);
                vm_set(mi->base + mi->data, size - mi->data, kMemCodeData, 3);

                optional_lm = {
                    mi, mi->base, size,
                    mi->func_count ? new Fn[mi->text_size / 4]() : nullptr
                };

                for (size_t k = 0; k < mi->func_count; k++)
                    optional_lm.table[mi->funcs[k].offset / 4] = mi->funcs[k].fn;

                optional_ready = nce::prepare_module_optional(optional_lm);
                if (!optional_ready) {
                    vm_set(mi->base, size, kMemFree, 0);
                    plat::decommit(mi->base, size);
                    log("exefs: optional %s discarded after NCE preparation failure", mi->name);
                    delete[] optional_lm.table;
                    optional_lm = {};
                }
            } else {
                plat::decommit(mi->base, size);
            }
        }
    }
#else
    (void)has_optional;
#endif

    // Map modules in caller-specified link order. The optional module is
    // already mapped and prepared; a failed optional module is simply omitted.
    for (size_t i = 0; i < count; i++) {
#if NX_NCE
        if (has_optional && i == optional_index) {
            if (!optional_ready) continue;
            g_modules.push_back(optional_lm);
            if (g_fast_count < 4 && optional_lm.table)
                g_fast[g_fast_count++] = {optional_lm.base, optional_lm.info->text_size, optional_lm.table};
            const ModuleInfo* mi = optional_lm.info;
            for (size_t k = 0; k < mi->symbol_count; k++) {
                const Symbol& sym = mi->symbols[k];
                if (sym.imported || !sym.name[0] || sym.bind == 0) continue;
                auto it = g_exports.find(sym.name);
                if (it == g_exports.end())
                    g_exports[sym.name] = mi->base + sym.value;
            }
            log("loaded optional %s at %llx (%llu functions, %llu relocs)",
                mi->name, (unsigned long long)mi->base,
                (unsigned long long)mi->func_count,
                (unsigned long long)mi->reloc_count);
            continue;
        }
#endif
        const ModuleInfo* mi = mods[i];
        if (!mi) fatal("module loader: module descriptor at index %zu is null", i);
        const uint64_t image = mi->data + mi->data_size;
        const uint64_t size = ((mi->bss + mi->bss_size) + 0xFFF) & ~0xFFFull;
        if (!commit(mi->base, size)) fatal("cannot commit module %s", mi->name);
        if (images) {
            std::memcpy(reinterpret_cast<void*>(mi->base), images[i], image);
        } else {
            const std::string path = data_dir + "/" + mi->name + ".bin";
            std::ifstream f(path, std::ios::binary);
            if (!f) fatal("missing %s (run tools/nso2bin.py)", path.c_str());
            f.read(reinterpret_cast<char*>(mi->base), std::streamsize(image));
            if (uint64_t(f.gcount()) != image)
                fatal("%s: expected %llx bytes", path.c_str(), (unsigned long long)image);
        }
        vm_set(mi->base + mi->text, mi->text_size, kMemCode, 5);
        vm_set(mi->base + mi->ro, mi->ro_size, kMemCode, 1);
        vm_set(mi->base + mi->data, size - mi->data, kMemCodeData, 3);

        LoadedModule lm{mi, mi->base, size, mi->func_count ? new Fn[mi->text_size / 4]() : nullptr};
        for (size_t k = 0; k < mi->func_count; k++)
            lm.table[mi->funcs[k].offset / 4] = mi->funcs[k].fn;
        g_modules.push_back(lm);
        if (g_fast_count < 4 && lm.table)
            g_fast[g_fast_count++] = {lm.base, mi->text_size, lm.table};

        for (size_t k = 0; k < mi->symbol_count; k++) {
            const Symbol& sym = mi->symbols[k];
            if (sym.imported || !sym.name[0] || sym.bind == 0) continue;
            auto it = g_exports.find(sym.name);
            if (it == g_exports.end() || sym.bind == 1) {
                if (it == g_exports.end()) g_exports[sym.name] = mi->base + sym.value;
            }
        }
        log("loaded %s at %llx (%llu functions, %llu relocs)", mi->name,
            (unsigned long long)mi->base, (unsigned long long)mi->func_count,
            (unsigned long long)mi->reloc_count);
    }

    size_t hle = 0, missing = 0, linked = 0;
    for (size_t index = first_new_module; index < g_modules.size(); index++) {
        auto& lm = g_modules[index];
        const ModuleInfo* mi = lm.info;
        for (size_t k = 0; k < mi->reloc_count; k++) {
            const Reloc& r = mi->relocs[k];
            uint64_t* slot = reinterpret_cast<uint64_t*>(lm.base + r.offset);
            if (r.type == R_AARCH64_RELATIVE) {
                *slot = lm.base + uint64_t(r.addend);
                continue;
            }
            if (r.type != R_AARCH64_GLOB_DAT && r.type != R_AARCH64_JUMP_SLOT &&
                r.type != R_AARCH64_ABS64) {
                log("%s: relocation type %u at %llx not handled", mi->name, r.type,
                    (unsigned long long)r.offset);
                continue;
            }
            const Symbol& sym = mi->symbols[r.sym];
            uint64_t target = 0;
            if (sym.type == 2 || sym.imported) {
                if (hle_lookup(sym.name) && !recompiled_only(sym.name)) {
                    target = hle_address(sym.name);
                    hle++;
                }
            }
            if (!target) {
                target = find_symbol(sym.name);
                if (target) linked++;
            }
            if (!target && !sym.imported) target = lm.base + sym.value;
            if (!target) {
                if (sym.type == 2 || sym.type == 0) target = hle_address(sym.name);
                missing++;
            }
            *slot = target + uint64_t(r.addend);
        }
    }
    log("linked: %zu to HLE, %zu across modules, %zu unresolved", hle, linked, missing);

#if NX_NCE
    for (size_t index = first_new_module; index < g_modules.size(); index++) {
        auto& lm = g_modules[index];
        if (has_optional && optional_ready && lm.base == mods[optional_index]->base)
            continue; // already prepared safely before publishing exports
        nce::prepare_module(lm);
    }
#endif
}

}  // namespace nx
