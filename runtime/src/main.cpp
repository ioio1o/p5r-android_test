// Boot: map and link the recompiled modules (what rtld does), create the main thread and enter
// nn::init::Start(main thread handle, main module base, init, fini) like rtld's _start.
// The platform's entry point (platform_win.cpp / platform_android.cpp) calls nx::boot().
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

#if NX_NCE
#include "game_image.h"
#include "nce/nce.h"
#include "nce/nso.h"
#else
extern const nx::ModuleInfo nx_module_main;
extern const nx::ModuleInfo nx_module_sdk;
#endif

namespace nx {

// The game's two modules: the recompiled ones, or (native execution) the NSOs of the player's exefs.
static const ModuleInfo* g_main = nullptr;
static const ModuleInfo* g_sdk = nullptr;

uint64_t mrs(CPU& c, uint32_t sysreg) {
    switch (sysreg) {
        case 0xDF02:  // CNTVCT_EL0
        case 0xDF01:  // CNTPCT_EL0
            return system_tick();
        case 0xDF00:  // CNTFRQ_EL0
            return 19200000;
        case 0xD807:  // DCZID_EL0: DC ZVA allowed, 64-byte blocks
            return 4;
        case 0xC000:  // MIDR_EL1 (Cortex-A57)
            return 0x411FD071;
        case 0xD801:  // CTR_EL0 (Cortex-A57: 64-byte lines, PIPT instruction cache)
            return 0x8444C004;
        default:
            log("mrs %#x at lr %s", sysreg, describe(c.x[30]).c_str());
            return 0;
    }
}

void msr(CPU& c, uint32_t sysreg, uint64_t value) {
    log("msr %#x = %llx at lr %s", sysreg, (unsigned long long)value, describe(c.x[30]).c_str());
}

// ---- module initializers --------------------------------------------------------------------------------------
// Each module's DT_INIT (at +0x30 in both main and sdk, see .dynamic) registers the module with the libc
// (__nnmusl_init_dso: TLS, unwind info) and runs its init_array, unless the registration reports 1: then the
// constructors are left to the loader and we run them. Registration only works once the program has given nn::os
// a TLS allocator (SetMemoryAllocatorForThreadLocal, in nninitStartup), so rtld's initializer callback does nothing
// and the modules are initialized right before nnMain, SDK first. Static TLS itself comes from the kernel
// (kernel.cpp: a block per thread at TLS region +0x1F0, where nn::os loads TPIDR_EL0 from).
constexpr uint64_t kDtInit = 0x30;

static void call_preserving(CPU& c, uint64_t target) {
    uint64_t saved[13];
    for (int r = 19; r <= 31; r++) saved[r - 19] = c.x[r];
    call(c, target);
    for (int r = 19; r <= 31; r++) c.x[r] = saved[r - 19];
}

// DT_INIT loads the word where it keeps __nnmusl_init_dso's result with `adrp xN, page` ... `ldr w8, [xN, #off]`
// in its first instructions (SDK 9: adjacent; SDK 16: another adrp in between). Follow the adrp targets until
// that load.
static uint64_t init_status_addr(const ModuleInfo& m) {
    uint64_t page[32] = {};
    for (uint64_t pc = m.base + kDtInit; pc < m.base + kDtInit + 0x40; pc += 4) {
        uint32_t w = ld<uint32_t>(pc);
        if ((w & 0x9F000000) == 0x90000000) {  // adrp
            int64_t imm = int64_t(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 43 >> 31;  // sign-extended << 12
            page[w & 31] = (pc & ~0xFFFull) + uint64_t(imm);
        } else if ((w & 0xFFC0001F) == 0xB9400008 && page[(w >> 5) & 31]) {  // ldr w8, [xN, #imm]
            return page[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * 4;
        }
    }
    fatal("%s: DT_INIT does not load its init status as expected", m.name);
}

// DT_INIT hands __nnmusl_init_dso the module's layout: consecutive GOT slots it loads with adrp + ldr. Slots 13
// and 14 are the start and end of the module's .tdata (checked on SDK 16: Wonder's main 0x359cc18..0x359cc28,
// sdk empty). Returns false when the pattern is not there (SDK 9 modules have no .tdata to worry about).
static bool tdata_range(const ModuleInfo& m, uint64_t& start, uint64_t& end) {
    uint64_t page[32] = {}, lo = ~0ull, hi = 0;
    for (uint64_t pc = m.base + kDtInit; pc < m.base + kDtInit + 0x100; pc += 4) {
        uint32_t w = ld<uint32_t>(pc);
        if ((w & 0x9F000000) == 0x90000000) {  // adrp
            int64_t imm = int64_t(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 43 >> 31;
            page[w & 31] = (pc & ~0xFFFull) + uint64_t(imm);
        } else if ((w & 0xFFC00000) == 0xF9400000 && page[(w >> 5) & 31]) {  // ldr xT, [xN, #imm]
            uint64_t slot = page[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * 8;
            lo = std::min(lo, slot), hi = std::max(hi, slot);
        }
    }
    if (hi < lo || (hi - lo) / 8 + 1 < 15) return false;
    start = ld<uint64_t>(lo + 13 * 8), end = ld<uint64_t>(lo + 14 * 8);
    return start <= end && end - start <= 0x1000 && start >= m.base && end <= m.base + 0x10000000;
}

static void init_module(CPU& c, const ModuleInfo& m) {
    call_preserving(c, m.base + kDtInit);
    uint32_t status = ld<uint32_t>(init_status_addr(m));
    if (status == 1)
        for (size_t k = 0; k < m.init_count; k++) call_preserving(c, m.base + m.init_array[k]);
    log("%s constructors: %zu", m.name, status == 1 ? m.init_count : 0);
}

static void rtld_init(CPU&) {}
static void rtld_fini(CPU&) {}
static void rtld_mark(CPU&) {}

// nnMain, as called by nn::init::Start: the constructors need the program's allocator, which nninitStartup has
// set up by now, so they run here, right before the real nnMain.
static void nn_main(CPU& c) {
    init_module(c, *g_sdk);
    init_module(c, *g_main);
    game::after_init();
    log("entering nnMain");
    uint64_t real = 0;
    for (size_t k = 0; k < g_main->symbol_count; k++)
        if (!g_main->symbols[k].imported && std::string(g_main->symbols[k].name) == "nnMain")
            real = g_main->base + g_main->symbols[k].value;
    call(c, real);
}

// longjmp(buf, val): unwinds the host stack to the recompiled function that called setjmp on buf.
static void long_jmp(CPU& c) { throw LongJmp{c.x[0], uint32_t(c.x[1])}; }

static void process_return(CPU&) {
    log("nn::init::Start returned");
    plat::exit_process(0);
}

NX_HLE("nnMain", nn_main);
NX_HLE("longjmp", long_jmp);
NX_HLE("_longjmp", long_jmp);
NX_HLE("__rtld_init", rtld_init);
NX_HLE("__rtld_fini", rtld_fini);
NX_HLE("__rtld_mark", rtld_mark);
NX_HLE("__process_return", process_return);

// ---- boot -----------------------------------------------------------------------------------------------------

void boot() {
    if (const char* t = std::getenv("NX_TRACE")) g_trace = *t == '1';
    if (plat::setting("trace") == "1") g_trace = true;
    memory_init();
    kernel_init();
#if NX_NCE
    // P5R 1.0.2 test: only execute modules from the explicitly selected game image.
    // Never fall back to a separately extracted ExeFS directory.
    nce::init();
    static std::unique_ptr<nce::NsoModule> nso[3];
    const char* names[2] = {"main", "sdk"};
    const nxi::GameImage* image = game_image();
    if (!image)
        fatal("P5R 1.0.2 test requires a selected game image; refusing extracted ExeFS fallback");
    for (int i = 0; i < 2; i++) {
        std::string error;
        const uint64_t base = kModuleBase + uint64_t(i) * 0x10000000;
        auto it = image->exefs().find(names[i]);
        if (it == image->exefs().end())
            fatal("selected game image's ExeFS has no %s", names[i]);
        nso[i] = nce::load_nso_bytes(it->second, std::string("ExeFS/") + names[i], names[i], base, error);
        if (!nso[i]) fatal("%s", error.c_str());
    }
    const char* want = game::kInfo.main_build_id;
    auto build_id_hex = [](const std::vector<uint8_t>& id) {
        std::string hex;
        hex.reserve(id.size() * 2);
        for (uint8_t b : id) {
            char byte[3];
            std::snprintf(byte, sizeof byte, "%02X", b);
            hex += byte;
        }
        return hex;
    };
    const std::string main_id = build_id_hex(nso[0]->build_id);
    const bool main_matches = want &&
        main_id.compare(0, std::strlen(want), want) == 0;

    if (plat::setting("version_log") == "1") {
        const std::string sdk_id = build_id_hex(nso[1]->build_id);
        log("version: title_id=%016llX main_build_id=%s expected_main_prefix=%s main_match=%s sdk_build_id=%s",
            static_cast<unsigned long long>(image->title_id()), main_id.c_str(),
            want ? want : "(none)", main_matches ? "yes" : "no", sdk_id.c_str());
    }

    if (want && !main_matches)
        fatal("exefs/main is build %s, not %s: this port is made for one version of the game (%s)",
              main_id.c_str(), want, "use the Persona 5 Royal 1.0.2 ExeFS");
    g_main = &nso[0]->info;
    g_sdk = &nso[1]->info;

    // Find the winning mod using the same alphabetical priority as the
    // existing ROMFS overlay: sorted ascending, last matching mod wins.
    namespace fs = std::filesystem;
    fs::path selected_subsdk9;
    std::vector<fs::path> mod_dirs;
    const fs::path mods_root = fs::u8path(plat::data_root()) / "mods";
    std::error_code ec;
    if (fs::is_directory(mods_root, ec)) {
        for (fs::directory_iterator it(mods_root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entry_ec;
            if (!it->is_directory(entry_ec) || entry_ec) continue;

            const std::string folder = it->path().filename().string();
            if (folder.empty() || folder[0] == '.' || folder[0] == '_') continue;

            std::error_code romfs_ec, subsdk_ec;
            const bool has_romfs = fs::is_directory(it->path() / "romfs", romfs_ec);
            const bool has_subsdk9 = fs::is_regular_file(
                it->path() / "exefs" / "subsdk9", subsdk_ec);

            if (has_romfs && !romfs_ec && has_subsdk9 && !subsdk_ec)
                mod_dirs.push_back(it->path());
        }
    }
    std::sort(mod_dirs.begin(), mod_dirs.end());
    if (!mod_dirs.empty())
        selected_subsdk9 = mod_dirs.back() / "exefs" / "subsdk9";

    size_t module_count = 2;
    const ModuleInfo* mods[3] = {g_main, g_sdk, nullptr};
    const uint8_t* images[3] = {
        nso[0]->image.data(), nso[1]->image.data(), nullptr
    };
    size_t optional_index = static_cast<size_t>(-1);

    if (!selected_subsdk9.empty()) {
        if (!main_matches) {
            log("exefs: optional subsdk9 skipped: main version mismatch");
        } else if (!plat::reserve(0x4020000000ull, 0x10000000ull)) {
            log("exefs: optional subsdk9 skipped: cannot reserve 256 MiB slot");
        } else {
            std::string error;
            nso[2] = nce::load_nso(selected_subsdk9.string(), "subsdk9",
                                   0x4020000000ull, error);
            if (!nso[2]) {
                log("exefs: optional subsdk9 skipped: %s", error.c_str());
            } else {
                const auto& mi = nso[2]->info;
                log("exefs: optional subsdk9 candidate: %s", selected_subsdk9.string().c_str());
                log("exefs: subsdk9 MOD0=%#x DT_INIT=%#llx DT_INIT_ARRAY=%#llx size=%#llx entries=%zu",
                    nso[2]->mod0_offset,
                    (unsigned long long)nso[2]->dt_init,
                    (unsigned long long)nso[2]->dt_init_array,
                    (unsigned long long)nso[2]->dt_init_array_size,
                    nso[2]->init_array.size());

                size_t collisions = 0;
                for (size_t k = 0; k < mi.symbol_count; k++) {
                    const Symbol& sym = mi.symbols[k];
                    if (sym.imported || !sym.name[0] || sym.bind == 0) continue;
                    bool collision = false;
                    for (size_t j = 0; j < g_main->symbol_count; j++) {
                        const Symbol& other = g_main->symbols[j];
                        if (!other.imported && other.bind != 0 &&
                            std::strcmp(sym.name, other.name) == 0) {
                            collision = true;
                            break;
                        }
                    }
                    if (!collision) {
                        for (size_t j = 0; j < g_sdk->symbol_count; j++) {
                            const Symbol& other = g_sdk->symbols[j];
                            if (!other.imported && other.bind != 0 &&
                                std::strcmp(sym.name, other.name) == 0) {
                                collision = true;
                                break;
                            }
                        }
                    }
                    if (collision) ++collisions;
                }
                log("exefs: subsdk9 exported names colliding with main/sdk: %zu", collisions);

                char first_words[16 * 9 + 1] = {};
                size_t used = 0;
                const size_t text_at = size_t(mi.text);
                for (size_t k = 0; k < 16 && text_at + k * 4 + 4 <= nso[2]->image.size(); k++) {
                    uint32_t word = 0;
                    std::memcpy(&word, nso[2]->image.data() + text_at + k * 4, 4);
                    int n = std::snprintf(first_words + used, sizeof(first_words) - used,
                                          "%s%08X", k ? " " : "", word);
                    if (n <= 0 || size_t(n) >= sizeof(first_words) - used) break;
                    used += size_t(n);
                }
                log("exefs: subsdk9 first text words: %s", first_words);

                mods[1] = &nso[2]->info;
                images[1] = nso[2]->image.data();
                mods[2] = g_sdk;
                images[2] = nso[1]->image.data();
                module_count = 3;
                optional_index = 1;
            }
        }
    }

    load_modules(mods, module_count, "", images, optional_index);
#else
    g_main = &nx_module_main, g_sdk = &nx_module_sdk;
    const ModuleInfo* mods[] = {g_main, g_sdk};
    load_modules(mods, 2, plat::data_root() + "/analysis");
#endif

    uint64_t tdata = 0, tdata_end = 0;
    if (tdata_range(*g_main, tdata, tdata_end) && tdata_end > tdata) {
        set_tls_template(tdata, tdata_end);
        log("main .tdata: %llu bytes", (unsigned long long)(tdata_end - tdata));
    }
    uint32_t main_thread = create_main_thread(16ull << 20);
    CPU& c = current_cpu();
    c.x[0] = main_thread;
    c.x[1] = g_main->base;
    // SDK 9: Start(thread, main base, init, fini). SDK 16 adds a first callback: rtld's sets a word to 1 (rtld+0x1330
    // in Wonder), before its init and fini that walk the module list.
    uint64_t start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (start) {
        c.x[2] = hle_address("__rtld_init");
        c.x[3] = hle_address("__rtld_fini");
    } else if ((start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_"))) {
        c.x[2] = hle_address("__rtld_mark");
        c.x[3] = hle_address("__rtld_init");
        c.x[4] = hle_address("__rtld_fini");
    } else {
        fatal("nn::init::Start not found");
    }
    c.x[30] = hle_address("__process_return");
    log("entering nn::init::Start at %s", describe(start).c_str());
    call(c, start);
    log("returned from nn::init::Start");
}

}  // namespace nx
