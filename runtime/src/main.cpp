// Boot: map and link the recompiled modules (what rtld does), create the main thread and enter
// nn::init::Start(main thread handle, main module base, init, fini) like rtld's _start.
// The platform's entry point (platform_win.cpp / platform_android.cpp) calls nx::boot().
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include "nce/nro.h"
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


#if NX_NCE
namespace {

template <typename T>
bool nro_read(const std::vector<uint8_t>& image, uint64_t offset, T& value) {
    if (offset > image.size() || sizeof(T) > image.size() - offset)
        return false;
    std::memcpy(&value, image.data() + offset, sizeof(T));
    return true;
}

struct NroDynamicSymbol {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t section;
    uint64_t value;
    uint64_t size;
};
static_assert(sizeof(NroDynamicSymbol) == 24);

static bool module_exports(const ModuleInfo* module, const char* name) {
    if (!module || !name || !*name)
        return false;

    for (size_t i = 0; i < module->symbol_count; ++i) {
        const Symbol& symbol = module->symbols[i];
        if (!symbol.imported && symbol.bind != 0 && symbol.name &&
            std::strcmp(symbol.name, name) == 0)
            return true;
    }
    return false;
}

static void inspect_skyline_nro(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        log("skyline: cannot open %s", path.string().c_str());
        return;
    }

    const std::streamoff length = file.tellg();
    if (length <= 0 || uint64_t(length) > SIZE_MAX) {
        log("skyline: invalid file size: %s", path.string().c_str());
        return;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(length));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(raw.data()), length);
    if (!file || file.gcount() != length) {
        log("skyline: incomplete read: %s", path.string().c_str());
        return;
    }

    std::string error;
    auto nro = nce::parse_nro_bytes(raw, path.string(), error);
    if (!nro) {
        log("skyline: invalid NRO: %s", error.c_str());
        return;
    }

    const auto& image = nro->image;

    // MOD0 + 4 contains a signed offset to the dynamic table.
    int32_t dynamic_relative = 0;
    if (!nro_read(image, uint64_t(nro->mod0_offset) + 4, dynamic_relative)) {
        log("skyline: %s: cannot read MOD0 dynamic offset", path.filename().string().c_str());
        return;
    }

    const int64_t dynamic_signed =
        int64_t(nro->mod0_offset) + int64_t(dynamic_relative);
    if (dynamic_signed < 0 || uint64_t(dynamic_signed) >= image.size()) {
        log("skyline: %s: dynamic table is outside image", path.filename().string().c_str());
        return;
    }

    constexpr int64_t DT_NULL = 0;
    constexpr int64_t DT_HASH = 4;
    constexpr int64_t DT_STRTAB = 5;
    constexpr int64_t DT_SYMTAB = 6;
    constexpr int64_t DT_STRSZ = 10;
    constexpr int64_t DT_SYMENT = 11;
    constexpr int64_t DT_RELA = 7;
    constexpr int64_t DT_RELASZ = 8;
    constexpr int64_t DT_RELAENT = 9;
    constexpr int64_t DT_PLTRELSZ = 2;
    constexpr int64_t DT_PLTREL = 20;
    constexpr int64_t DT_JMPREL = 23;

    uint64_t hash = 0, strtab = 0, symtab = 0, strsz = 0, syment = 0;
    uint64_t rela = 0, relasz = 0, relaent = 0;
    uint64_t jmprel = 0, pltrelsz = 0, pltrel = 0;
    bool terminated = false;

    // Elf64_Dyn entries are 16 bytes. Bound the scan even if the file is malformed.
    for (size_t i = 0; i < 4096; ++i) {
        const uint64_t at = uint64_t(dynamic_signed) + uint64_t(i) * 16;
        int64_t tag = 0;
        uint64_t value = 0;

        if (!nro_read(image, at, tag) || !nro_read(image, at + 8, value))
            break;

        if (tag == DT_NULL) {
            terminated = true;
            break;
        }

        switch (tag) {
            case DT_HASH:   hash = value; break;
            case DT_STRTAB: strtab = value; break;
            case DT_SYMTAB: symtab = value; break;
            case DT_STRSZ:  strsz = value; break;
            case DT_SYMENT: syment = value; break;
            case DT_RELA: rela = value; break;
            case DT_RELASZ: relasz = value; break;
            case DT_RELAENT: relaent = value; break;
            case DT_JMPREL: jmprel = value; break;
            case DT_PLTRELSZ: pltrelsz = value; break;
            case DT_PLTREL: pltrel = value; break;
            default: break;
        }
    }

    const std::string plugin_name = path.filename().string();
    if (!terminated || !hash || !strtab || !symtab || !strsz ||
        syment != sizeof(NroDynamicSymbol)) {
        log("skyline: %s: missing or invalid dynamic symbol metadata", plugin_name.c_str());
        return;
    }

    uint32_t bucket_count = 0, symbol_count = 0;
    if (!nro_read(image, hash, bucket_count) ||
        !nro_read(image, hash + 4, symbol_count) ||
        symbol_count == 0 ||
        symbol_count > image.size() / sizeof(NroDynamicSymbol) ||
        symtab > image.size() ||
        uint64_t(symbol_count) > (image.size() - symtab) / sizeof(NroDynamicSymbol) ||
        strtab > image.size() || strsz > image.size() - strtab) {
        log("skyline: %s: invalid hash, symbol, or string table bounds", plugin_name.c_str());
        return;
    }
    (void)bucket_count;

    std::vector<std::string> names(symbol_count);
    std::vector<uint16_t> sections(symbol_count);

    for (uint32_t i = 0; i < symbol_count; ++i) {
        NroDynamicSymbol symbol{};
        const uint64_t at = symtab + uint64_t(i) * sizeof(symbol);

        if (!nro_read(image, at, symbol)) {
            log("skyline: %s: truncated dynamic symbol table", plugin_name.c_str());
            return;
        }

        sections[i] = symbol.section;

        if (symbol.name >= strsz)
            continue;

        const char* start = reinterpret_cast<const char*>(image.data() + strtab + symbol.name);
        const size_t remaining = static_cast<size_t>(strsz - symbol.name);
        const void* end = std::memchr(start, '\0', remaining);
        if (!end) {
            log("skyline: %s: unterminated symbol name at index %u", plugin_name.c_str(), i);
            return;
        }

        names[i].assign(start, static_cast<const char*>(end) - start);
    }

    size_t imports = 0, local = 0, hle = 0, game = 0, unresolved = 0;

    for (uint32_t i = 1; i < symbol_count; ++i) {
        // SHN_UNDEF means this dynamic symbol is imported.
        if (sections[i] != 0 || names[i].empty())
            continue;

        ++imports;
        const char* name = names[i].c_str();
        const char* source = "UNRESOLVED";

        // Resolution priority: plugin-defined symbol, HLE, then main/SDK.
        bool found_local = false;
        for (uint32_t j = 1; j < symbol_count; ++j) {
            if (sections[j] != 0 && names[j] == names[i]) {
                found_local = true;
                break;
            }
        }

        if (found_local) {
            source = "PLUGIN";
            ++local;
        } else if (hle_lookup(name)) {
            source = "HLE";
            ++hle;
        } else if (module_exports(g_main, name) || module_exports(g_sdk, name)) {
            source = "MAIN/SDK";
            ++game;
        } else {
            ++unresolved;
        }

        log("skyline: import[%u] %s -> %s", i, name, source);
    }


    // Count AArch64 Elf64_Rela relocation types without applying them.
    auto relocation_name = [](uint32_t type) -> const char* {
        switch (type) {
            case 0: return "NONE";
            case 257: return "ABS64";
            case 258: return "ABS32";
            case 259: return "ABS16";
            case 260: return "PREL64";
            case 261: return "PREL32";
            case 262: return "PREL16";
            case 275: return "ADR_PREL_PG_HI21";
            case 277: return "ADD_ABS_LO12_NC";
            case 282: return "JUMP26";
            case 283: return "CALL26";
            case 1024: return "COPY";
            case 1025: return "GLOB_DAT";
            case 1026: return "JUMP_SLOT";
            case 1027: return "RELATIVE";
            case 1028: return "TLS_DTPREL";
            case 1029: return "TLS_DTPMOD";
            case 1030: return "TLS_TPREL";
            case 1031: return "TLSDESC";
            case 1032: return "IRELATIVE";
            default: return "OTHER";
        }
    };
    auto report_relocations = [&](const char* table_name,
                                  uint64_t offset, uint64_t size,
                                  uint64_t entry_size) {
        constexpr uint64_t kRelaSize = 24;

        if (size == 0) {
            log("skyline: %s relocations: 0", table_name);
            return;
        }

        if (!offset || entry_size != kRelaSize ||
            size % kRelaSize != 0 ||
            offset > image.size() || size > image.size() - offset) {
            log("skyline: %s relocations: invalid table bounds/entry size",
                table_name);
            return;
        }

        std::vector<std::pair<uint32_t, size_t>> counts;
        const uint64_t total = size / kRelaSize;

        for (uint64_t i = 0; i < total; ++i) {
            uint64_t info = 0;
            const uint64_t at = offset + i * kRelaSize + 8;

            if (!nro_read(image, at, info)) {
                log("skyline: %s relocations: truncated entry %llu",
                    table_name, static_cast<unsigned long long>(i));
                return;
            }

            const uint32_t type = static_cast<uint32_t>(info);
            auto it = std::find_if(counts.begin(), counts.end(),
                [type](const auto& item) { return item.first == type; });

            if (it == counts.end())
                counts.emplace_back(type, 1);
            else
                ++it->second;
        }

        log("skyline: %s relocations: %llu entries",
            table_name, static_cast<unsigned long long>(total));

        for (const auto& item : counts) {
            log("skyline:   type %u (%s): %zu",
                item.first, relocation_name(item.first), item.second);
        }
    };

    report_relocations("DT_RELA", rela, relasz, relaent);

    if (pltrel == DT_RELA) {
        report_relocations("DT_JMPREL", jmprel, pltrelsz, relaent);
    } else if (pltrelsz != 0) {
        log("skyline: DT_JMPREL uses unsupported relocation format %llu",
            static_cast<unsigned long long>(pltrel));
    } else {
        log("skyline: DT_JMPREL relocations: 0");
    }

    log("skyline: %s dry-run: %zu imports; plugin=%zu HLE=%zu main/sdk=%zu unresolved=%zu",
        plugin_name.c_str(), imports, local, hle, game, unresolved);
}

}  // namespace
#endif

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

    // Read-only Skyline plugin import inspection. Plugins are not loaded or executed.
    namespace fs = std::filesystem;
    const fs::path mods_root = fs::u8path(plat::data_root()) / "mods";
    std::vector<fs::path> plugin_paths;
    std::error_code ec;

    if (fs::is_directory(mods_root, ec) && !ec) {
        for (fs::directory_iterator it(mods_root, ec), finish;
             !ec && it != finish; it.increment(ec)) {
            std::error_code entry_ec;
            if (!it->is_directory(entry_ec) || entry_ec)
                continue;

            const std::string folder = it->path().filename().string();
            if (folder.empty() || folder[0] == '.' || folder[0] == '_')
                continue;

            const fs::path plugins_dir =
                it->path() / "romfs" / "skyline" / "plugins";
            std::error_code plugins_ec;
            if (!fs::is_directory(plugins_dir, plugins_ec) || plugins_ec)
                continue;

            for (fs::directory_iterator plugin_it(plugins_dir, plugins_ec), plugin_end;
                 !plugins_ec && plugin_it != plugin_end;
                 plugin_it.increment(plugins_ec)) {
                std::error_code file_ec;
                if (!plugin_it->is_regular_file(file_ec) || file_ec)
                    continue;

                if (plugin_it->path().extension() == ".nro")
                    plugin_paths.push_back(plugin_it->path());
            }
        }
    }

    std::sort(plugin_paths.begin(), plugin_paths.end());
    log("skyline: discovered %zu plugin NRO file(s)", plugin_paths.size());

    for (const fs::path& plugin_path : plugin_paths)
        inspect_skyline_nro(plugin_path);

    // This boot batch intentionally contains only the game's main and SDK modules.
    const ModuleInfo* mods[] = {g_main, g_sdk};
    const uint8_t* images[] = {
        nso[0]->image.data(), nso[1]->image.data()
    };
    load_modules(mods, 2, "", images, static_cast<size_t>(-1));
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
