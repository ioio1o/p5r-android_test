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

// longjmp(buf, val): unwinds the host stack to the recompiled function that called setjmp on buf.
static void long_jmp(CPU& c) { throw LongJmp{c.x[0], uint32_t(c.x[1])}; }

static void process_return(CPU&) {
    log("nn::init::Start returned");
    plat::exit_process(0);
}

NX_HLE("longjmp", long_jmp);
NX_HLE("_longjmp", long_jmp);
NX_HLE("__rtld_init", rtld_init);
NX_HLE("__rtld_fini", rtld_fini);
NX_HLE("__rtld_mark", rtld_mark);
NX_HLE("__process_return", process_return);


#if NX_NCE
namespace {

static uint64_t g_skyline_function_hook_requests = 0;
static uint64_t g_skyline_inline_hook_requests = 0;

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


struct SkylinePluginRuntime {
    std::string name;
    std::unique_ptr<nce::NroImage> nro;
    std::vector<std::string> names;
    std::vector<Symbol> symbols;
    std::vector<Reloc> relocs;
    ModuleInfo info{};
    uint64_t base = 0;
    uint64_t mapped_size = 0;

    uint64_t dt_init = 0;
    uint64_t dt_init_array = 0;
    uint64_t dt_init_array_size = 0;

    size_t imports_plugin = 0, imports_hle = 0;
    size_t imports_main = 0, imports_sdk = 0, imports_unresolved = 0;

    size_t reloc_relative = 0, reloc_glob_dat = 0;
    size_t reloc_jump_slot = 0, reloc_abs64 = 0;
};

static std::vector<std::unique_ptr<SkylinePluginRuntime>> g_skyline_plugins;
static uint64_t g_skyline_next_slot = 0;
static void run_skyline_plugins(CPU& c);

static constexpr uint64_t kSkylinePluginBase = 0x4030000000ull;
static constexpr uint64_t kSkylinePluginSlotSize = 0x10000000ull;
static constexpr uint64_t kSkylinePluginSlotCount = 4;

static uint64_t skyline_module_symbol(const ModuleInfo* module,
                                      const char* name) {
    if (!module || !name || !*name)
        return 0;

    for (size_t i = 0; i < module->symbol_count; ++i) {
        const Symbol& sym = module->symbols[i];
        if (!sym.imported && sym.bind != 0 && sym.name &&
            std::strcmp(sym.name, name) == 0)
            return module->base + sym.value;
    }
    return 0;
}


static bool guest_range_ok(
    uint64_t address,
    uint64_t length,
    uint32_t permission
) {
    if (!address || !length || address > UINT64_MAX - length)
        return false;

    const uint64_t end = address + length;
    uint64_t cursor = address;

    while (cursor < end) {
        const VmInfo region = vm_query(cursor);

        if (region.state == kMemFree ||
            region.state == kMemInaccessible ||
            (region.perm & permission) != permission ||
            !region.size ||
            cursor < region.addr ||
            region.addr > UINT64_MAX - region.size) {
            return false;
        }

        const uint64_t region_end = region.addr + region.size;
        if (region_end <= cursor)
            return false;

        cursor = std::min(end, region_end);
    }

    return true;
}

static bool skyline_main_text_target(uint64_t target) {
    if (!g_main || !g_main->text_size)
        return false;

    const uint64_t begin = g_main->base + g_main->text;
    return target >= begin && target - begin < g_main->text_size;
}

static const char* skyline_pc_relative_kind(uint32_t insn) {
    if ((insn & 0x1F000000u) == 0x10000000u) return "ADR/ADRP";
    if ((insn & 0x7C000000u) == 0x14000000u) return "B/BL";
    if ((insn & 0x3B000000u) == 0x18000000u) return "literal-load";
    if ((insn & 0xFF000010u) == 0x54000000u) return "B.cond";
    if ((insn & 0x7E000000u) == 0x34000000u) return "CBZ/CBNZ";
    if ((insn & 0x7E000000u) == 0x36000000u) return "TBZ/TBNZ";
    return "other/non-PC-relative";
}

static void skyline_log_hook_target(const char* hook, uint64_t target) {
    if (!skyline_main_text_target(target))
        return;

    if (!guest_range_ok(target, sizeof(uint32_t), 1)) {
        log("skyline: %s target=%llx is in main text but unreadable",
            hook, (unsigned long long)target);
        return;
    }

    uint32_t instruction = 0;
    std::memcpy(
        &instruction,
        reinterpret_cast<const void*>(target),
        sizeof(instruction)
    );

    log("skyline: %s main-text target=%llx first_insn=%08x pc_relative=%s",
        hook,
        (unsigned long long)target,
        instruction,
        skyline_pc_relative_kind(instruction)
    );
}

static void skyline_get_region(CPU& c) {
    const uint64_t requested = c.x[0];
    uint64_t result = 0;

    if (g_main) {
        switch (requested) {
            case 0: result = g_main->base + g_main->text; break;
            case 1: result = g_main->base + g_main->ro; break;
            case 2: result = g_main->base + g_main->data; break;
            case 3: result = g_main->base + g_main->bss; break;
            case 4: result = kHeapBase; break;
            default: break;
        }
    }

    c.x[0] = result;

    log("skyline: getRegionAddress region=%llu -> %llx",
        (unsigned long long)requested,
        (unsigned long long)result
    );
}

static void skyline_tcp_send_raw(CPU& c) {
    static uint64_t sequence = 0;
    const uint64_t address = c.x[0];
    const uint64_t write_sequence = ++sequence;

    if (!guest_range_ok(address, 1, 1)) {
        log("skyline: TCP write #%llu buffer=%llx length=unreadable",
            (unsigned long long)write_sequence,
            (unsigned long long)address);
        return;
    }

    char raw[1024];
    size_t count = 0;

    for (; count < sizeof(raw); ++count) {
        if (address > UINT64_MAX - count ||
            !guest_range_ok(address + count, 1, 1)) {
            break;
        }

        const char ch =
            *reinterpret_cast<const char*>(address + count);
        if (!ch)
            break;
        raw[count] = ch;
    }

    const size_t write_length = count;
    const size_t preview_count = std::min<size_t>(write_length, 48);
    char hex[48 * 3 + 1]{};
    char ascii[49]{};

    for (size_t i = 0; i < preview_count; ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw[i]);
        const size_t offset = i * 3;
        std::snprintf(
            hex + offset, sizeof(hex) - offset,
            "%02x%s", ch, i + 1 < preview_count ? " " : ""
        );
        ascii[i] = ch >= 0x20 && ch <= 0x7e
            ? static_cast<char>(ch) : '.';
    }

    log("skyline: TCP write #%llu buffer=%llx length=%zu "
        "preview_hex=[%s] preview_ascii=\"%s\"",
        (unsigned long long)write_sequence,
        (unsigned long long)address,
        write_length, hex, ascii);

    while (count &&
           (raw[count - 1] == '\n' || raw[count - 1] == '\r')) {
        --count;
    }

    std::string sanitized;
    sanitized.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw[i]);
        sanitized.push_back(
            ch >= 0x20 && ch <= 0x7e
                ? static_cast<char>(ch)
                : '.'
        );
    }
    log("plugin: %s", sanitized.c_str());
}

static void skyline_a64_hook_function(CPU& c) {
        ++g_skyline_function_hook_requests;
const uint64_t target = c.x[0];
    const uint64_t replacement = c.x[1];
    const uint64_t result_slot = c.x[2];

    skyline_log_hook_target("A64HookFunction", target);

    if ((result_slot & 7) == 0 &&
        guest_range_ok(result_slot, sizeof(uint64_t), 3)) {
        std::memcpy(
            reinterpret_cast<void*>(result_slot),
            &target,
            sizeof(target)
        );

        log("skyline: A64HookFunction no-op target=%llx replacement=%llx; "
            "original target returned at %llx; guest code unchanged",
            (unsigned long long)target,
            (unsigned long long)replacement,
            (unsigned long long)result_slot
        );
    } else {
        log("skyline: A64HookFunction no-op target=%llx replacement=%llx; "
            "invalid result slot=%llx; guest code unchanged",
            (unsigned long long)target,
            (unsigned long long)replacement,
            (unsigned long long)result_slot
        );
    }
}

static void skyline_a64_inline_hook(CPU& c) {
        ++g_skyline_inline_hook_requests;
const uint64_t target = c.x[0];
    const uint64_t replacement = c.x[1];

    skyline_log_hook_target("A64InlineHook", target);

    log("skyline: A64InlineHook no-op target=%llx replacement=%llx; "
        "guest code unchanged",
        (unsigned long long)target,
        (unsigned long long)replacement
    );
}

NX_HLE("getRegionAddress", skyline_get_region);
NX_HLE("skyline_tcp_send_raw", skyline_tcp_send_raw);
NX_HLE("A64HookFunction", skyline_a64_hook_function);
NX_HLE("A64InlineHook", skyline_a64_inline_hook);


struct SkylineElfRela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};
static_assert(sizeof(SkylineElfRela) == 24);

static bool skyline_parse_dynamic(
    const std::vector<uint8_t>& image,
    uint64_t mod0,
    uint64_t& hash,
    uint64_t& strtab,
    uint64_t& symtab,
    uint64_t& strsz,
    uint64_t& rela,
    uint64_t& relasz,
    uint64_t& relaent,
    uint64_t& jmprel,
    uint64_t& pltrelsz,
    uint64_t& pltrel,
    uint64_t& dt_init,
    uint64_t& dt_init_array,
    uint64_t& dt_init_array_size) {

    int32_t relative = 0;
    if (!nro_read(image, mod0 + 4, relative))
        return false;

    const int64_t dynamic = int64_t(mod0) + relative;
    if (dynamic < 0 || uint64_t(dynamic) >= image.size())
        return false;

    constexpr int64_t DT_NULL = 0;
    constexpr int64_t DT_HASH = 4;
    constexpr int64_t DT_STRTAB = 5;
    constexpr int64_t DT_SYMTAB = 6;
    constexpr int64_t DT_RELA = 7;
    constexpr int64_t DT_RELASZ = 8;
    constexpr int64_t DT_RELAENT = 9;
    constexpr int64_t DT_STRSZ = 10;
    constexpr int64_t DT_JMPREL = 23;
    constexpr int64_t DT_PLTRELSZ = 2;
    constexpr int64_t DT_PLTREL = 20;
    constexpr int64_t DT_INIT = 12;
    constexpr int64_t DT_INIT_ARRAY = 25;
    constexpr int64_t DT_INIT_ARRAYSZ = 27;

    bool terminated = false;
    for (size_t i = 0; i < 4096; ++i) {
        const uint64_t at = uint64_t(dynamic) + uint64_t(i) * 16;
        int64_t tag = 0;
        uint64_t value = 0;
        if (!nro_read(image, at, tag) || !nro_read(image, at + 8, value))
            return false;
        if (tag == DT_NULL) {
            terminated = true;
            break;
        }

        switch (tag) {
        case DT_HASH: hash = value; break;
        case DT_STRTAB: strtab = value; break;
        case DT_SYMTAB: symtab = value; break;
        case DT_STRSZ: strsz = value; break;
        case DT_RELA: rela = value; break;
        case DT_RELASZ: relasz = value; break;
        case DT_RELAENT: relaent = value; break;
        case DT_JMPREL: jmprel = value; break;
        case DT_PLTRELSZ: pltrelsz = value; break;
        case DT_PLTREL: pltrel = value; break;
        case DT_INIT: dt_init = value; break;
        case DT_INIT_ARRAY: dt_init_array = value; break;
        case DT_INIT_ARRAYSZ: dt_init_array_size = value; break;
        default: break;
        }
    }
    return terminated && hash && strtab && symtab && strsz &&
           rela && relaent == sizeof(SkylineElfRela);
}

static bool skyline_load_rela_table(
    const std::vector<uint8_t>& image,
    uint64_t offset,
    uint64_t size,
    uint32_t symbol_count,
    std::vector<Reloc>& output) {

    if (!size)
        return true;
    if (size % sizeof(SkylineElfRela) || offset > image.size() ||
        size > image.size() - offset)
        return false;

    const size_t count = static_cast<size_t>(size / sizeof(SkylineElfRela));
    if (count > 1000000 || output.size() > 1000000 - count)
        return false;

    for (size_t i = 0; i < count; ++i) {
        SkylineElfRela r{};
        if (!nro_read(image, offset + i * sizeof(r), r))
            return false;

        const uint32_t type = uint32_t(r.info);
        const uint32_t sym = uint32_t(r.info >> 32);
        if (sym >= symbol_count)
            return false;

        output.push_back({r.offset, type, sym, r.addend});
    }
    return true;
}

static bool load_skyline_nro(const std::filesystem::path& path) {
    const std::string plugin_name = path.filename().string();

    if (!skyline_plugin_slot_available()) {
        log("skyline: %s skipped: plugin address range unavailable",
            plugin_name.c_str());
        return false;
    }

    if (g_skyline_next_slot >= kSkylinePluginSlotCount) {
        log("skyline: %s skipped: all four plugin slots are occupied",
            plugin_name.c_str());
        return false;
    }

    const uint64_t base =
        kSkylinePluginBase + g_skyline_next_slot++ * kSkylinePluginSlotSize;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        log("skyline: %s cannot be opened", plugin_name.c_str());
        return false;
    }

    const std::streamoff length = file.tellg();
    if (length <= 0 || uint64_t(length) > SIZE_MAX) {
        log("skyline: %s has invalid file size", plugin_name.c_str());
        return false;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(raw.data()), length);
    if (!file || file.gcount() != length) {
        log("skyline: %s read was incomplete", plugin_name.c_str());
        return false;
    }

    std::string error;
    auto nro = nce::parse_nro_bytes(raw, path.string(), error);
    if (!nro) {
        log("skyline: %s rejected: %s", plugin_name.c_str(), error.c_str());
        return false;
    }

    uint64_t hash = 0, strtab = 0, symtab = 0, strsz = 0;
    uint64_t rela = 0, relasz = 0, relaent = 0;
    uint64_t jmprel = 0, pltrelsz = 0, pltrel = 0;
    uint64_t dt_init = 0, dt_init_array = 0, dt_init_array_size = 0;

    if (!skyline_parse_dynamic(nro->image, nro->mod0_offset,
            hash, strtab, symtab, strsz, rela, relasz, relaent,
            jmprel, pltrelsz, pltrel, dt_init, dt_init_array, dt_init_array_size) ||
        (pltrelsz && pltrel != 7)) {
        log("skyline: %s has invalid dynamic metadata", plugin_name.c_str());
        return false;
    }

    uint32_t buckets = 0, symbol_count = 0;
    if (!nro_read(nro->image, hash, buckets) ||
        !nro_read(nro->image, hash + 4, symbol_count) ||
        !symbol_count ||
        symbol_count > nro->image.size() / sizeof(NroDynamicSymbol) ||
        symtab > nro->image.size() ||
        uint64_t(symbol_count) >
            (nro->image.size() - symtab) / sizeof(NroDynamicSymbol) ||
        strtab > nro->image.size() ||
        strsz > nro->image.size() - strtab) {
        log("skyline: %s has invalid symbol table bounds", plugin_name.c_str());
        return false;
    }
    (void)buckets;

    auto plugin = std::make_unique<SkylinePluginRuntime>();
    plugin->name = plugin_name;
    plugin->nro = std::move(nro);
    plugin->base = base;
    plugin->dt_init = dt_init;
    plugin->dt_init_array = dt_init_array;
    plugin->dt_init_array_size = dt_init_array_size;

    const auto& image = plugin->nro->image;
    plugin->names.resize(symbol_count);
    plugin->symbols.resize(symbol_count);

    for (uint32_t i = 0; i < symbol_count; ++i) {
        NroDynamicSymbol raw_symbol{};
        if (!nro_read(image, symtab + uint64_t(i) * sizeof(raw_symbol),
                      raw_symbol)) {
            log("skyline: %s has a truncated symbol table", plugin_name.c_str());
            return false;
        }

        if (raw_symbol.name >= strsz)
            continue;

        const char* start = reinterpret_cast<const char*>(
            image.data() + strtab + raw_symbol.name);
        const size_t remaining = static_cast<size_t>(strsz - raw_symbol.name);
        const void* end = std::memchr(start, 0, remaining);
        if (!end) {
            log("skyline: %s has an unterminated symbol name",
                plugin_name.c_str());
            return false;
        }

        plugin->names[i].assign(
            start, static_cast<const char*>(end) - start);

        plugin->symbols[i] = {
            plugin->names[i].c_str(),
            raw_symbol.value,
            raw_symbol.size,
            uint8_t(raw_symbol.info & 0x0f),
            uint8_t(raw_symbol.info >> 4),
            uint8_t(raw_symbol.section == 0)
        };
    }

    if (!skyline_load_rela_table(image, rela, relasz, symbol_count,
                                 plugin->relocs) ||
        !skyline_load_rela_table(image, jmprel, pltrelsz, symbol_count,
                                 plugin->relocs)) {
        log("skyline: %s has invalid relocation tables", plugin_name.c_str());
        return false;
    }

    const uint64_t bss = plugin->nro->data + plugin->nro->data_size;
    if (plugin->nro->image.size() > kSkylinePluginSlotSize ||
        bss > kSkylinePluginSlotSize ||
        plugin->nro->bss_size > kSkylinePluginSlotSize - bss) {
        log("skyline: %s exceeds its 256 MiB slot", plugin_name.c_str());
        return false;
    }

    const uint64_t raw_size = std::max<uint64_t>(
        plugin->nro->image.size(), bss + plugin->nro->bss_size);
    const uint64_t mapped_size = (raw_size + 0xffff) & ~uint64_t(0xffff);
    if (!mapped_size || mapped_size > kSkylinePluginSlotSize ||
        !commit(base, mapped_size)) {
        log("skyline: %s mapping allocation failed at %llx",
            plugin_name.c_str(), (unsigned long long)base);
        return false;
    }

    plugin->mapped_size = mapped_size;
    std::memset(reinterpret_cast<void*>(base), 0, mapped_size);
    std::memcpy(reinterpret_cast<void*>(base), image.data(), image.size());

    plugin->info.name = plugin->name.c_str();
    plugin->info.base = base;
    plugin->info.text = plugin->nro->text;
    plugin->info.text_size = plugin->nro->text_size;
    plugin->info.ro = plugin->nro->ro;
    plugin->info.ro_size = plugin->nro->ro_size;
    plugin->info.data = plugin->nro->data;
    plugin->info.data_size = plugin->nro->data_size;
    plugin->info.bss = bss;
    plugin->info.bss_size = plugin->nro->bss_size;
    plugin->info.funcs = nullptr;
    plugin->info.func_count = 0;
    plugin->info.relocs = plugin->relocs.data();
    plugin->info.reloc_count = plugin->relocs.size();
    plugin->info.symbols = plugin->symbols.data();
    plugin->info.symbol_count = plugin->symbols.size();

    // Resolve imports after main and SDK have been linked.
    // Priority: plugin definitions, HLE, main, SDK.
    enum : uint8_t {
        kImportUnresolved = 0,
        kImportPlugin = 1,
        kImportHle = 2,
        kImportMain = 3,
        kImportSdk = 4
    };

    std::vector<uint64_t> resolved_symbols(symbol_count, 0);

    size_t imports_plugin = 0;
    size_t imports_hle = 0;
    size_t imports_main = 0;
    size_t imports_sdk = 0;
    size_t imports_unresolved = 0;

    for (uint32_t i = 0; i < symbol_count; ++i) {
        const Symbol& sym = plugin->symbols[i];
        if (i == 0 || !sym.imported)
            continue;

        const char* name = plugin->names[i].c_str();
        uint64_t target = 0;
        uint8_t source = kImportUnresolved;

        // First: a definition inside this plugin.
        if (name[0]) {
            for (uint32_t j = 1; j < symbol_count; ++j) {
                const Symbol& local = plugin->symbols[j];
                if (!local.imported && local.bind != 0 &&
                    plugin->names[j] == name) {
                    target = base + local.value;
                    source = kImportPlugin;
                    break;
                }
            }
        }

        // __nro_header_start denotes the beginning of this NRO.
        if (!target && name[0] &&
            std::strcmp(name, "__nro_header_start") == 0) {
            target = base;
            source = kImportPlugin;
            log("skyline: %s __nro_header_start -> plugin base %llx",
                plugin->name.c_str(), (unsigned long long)base);
        }

        // Then registered HLE handlers.
        if (!target && name[0] && hle_lookup(name)) {
            target = hle_address(name);
            source = kImportHle;
        }

        // Then main, followed by SDK exports.
        if (!target && name[0]) {
            target = skyline_module_symbol(g_main, name);
            if (target)
                source = kImportMain;
        }

        if (!target && name[0]) {
            target = skyline_module_symbol(g_sdk, name);
            if (target)
                source = kImportSdk;
        }

        // An unresolved weak symbol is valid and resolves to zero.
        // For unresolved strong symbols, use the loader's missing-import
        // HLE stub so a later call is logged instead of jumping to zero.
        if (!target && name[0] && sym.bind != 2) {
            target = hle_address(name);
            source = kImportUnresolved;
        }

        resolved_symbols[i] = target;

        switch (source) {
        case kImportPlugin: ++imports_plugin; break;
        case kImportHle: ++imports_hle; break;
        case kImportMain: ++imports_main; break;
        case kImportSdk: ++imports_sdk; break;
        default:
            ++imports_unresolved;
            log("skyline: %s unresolved import[%u] %s%s",
                plugin->name.c_str(), i, name,
                sym.bind == 2 ? " (weak; resolved to zero)" : "");
            break;
        }
    }

    size_t applied_relative = 0;
    size_t applied_glob_dat = 0;
    size_t applied_jump_slot = 0;
    size_t applied_abs64 = 0;
    size_t targets_text = 0;
    size_t targets_ro = 0;
    size_t targets_data = 0;
    size_t targets_bss = 0;
    size_t targets_outside = 0;

    bool relocation_ok = true;

    for (const Reloc& r : plugin->relocs) {
        if (r.type == 0)
            continue;

        if (r.offset > mapped_size ||
            sizeof(uint64_t) > mapped_size - r.offset) {
            ++targets_outside;
            log("skyline: %s relocation target outside mapped image: %llx",
                plugin->name.c_str(), (unsigned long long)r.offset);
            relocation_ok = false;
            break;
        }

        // Classify the complete 8-byte relocation write.
        enum class TargetSegment { Text, Ro, Data, Bss, Outside };
        auto contains_write = [&](uint64_t begin, uint64_t size) {
            return size && r.offset >= begin &&
                r.offset - begin <= size &&
                sizeof(uint64_t) <= size - (r.offset - begin);
        };

        TargetSegment segment = TargetSegment::Outside;
        if (contains_write(plugin->nro->text, plugin->nro->text_size))
            segment = TargetSegment::Text;
        else if (contains_write(plugin->nro->ro, plugin->nro->ro_size))
            segment = TargetSegment::Ro;
        else if (contains_write(plugin->nro->data, plugin->nro->data_size))
            segment = TargetSegment::Data;
        else if (contains_write(
                     plugin->nro->data + plugin->nro->data_size,
                     plugin->nro->bss_size))
            segment = TargetSegment::Bss;

        switch (segment) {
        case TargetSegment::Text: ++targets_text; break;
        case TargetSegment::Ro: ++targets_ro; break;
        case TargetSegment::Data: ++targets_data; break;
        case TargetSegment::Bss: ++targets_bss; break;
        default: ++targets_outside; break;
        }

        // Text is executable and must not receive relocation writes.
        // RO remains host-writable until the plugin's mapping is finalized.
        if (segment == TargetSegment::Text ||
            segment == TargetSegment::Outside) {
            log("skyline: %s rejected relocation target %llx "
                "(segment=%s)",
                plugin->name.c_str(),
                (unsigned long long)r.offset,
                segment == TargetSegment::Text ? "text" : "outside");
            relocation_ok = false;
            break;
        }

        uint64_t target = 0;
        if (r.type == R_AARCH64_RELATIVE) {
            target = base;
        } else if (r.type == R_AARCH64_ABS64 ||
                   r.type == R_AARCH64_GLOB_DAT ||
                   r.type == R_AARCH64_JUMP_SLOT) {
            if (r.sym >= symbol_count) {
                log("skyline: %s relocation has invalid symbol index %u",
                    plugin->name.c_str(), r.sym);
                relocation_ok = false;
                break;
            }

            const Symbol& sym = plugin->symbols[r.sym];
            target = sym.imported
                ? resolved_symbols[r.sym]
                : base + sym.value;

            // Zero is valid for an unresolved weak import.
            if (!target && !(sym.imported && sym.bind == 2)) {
                log("skyline: %s relocation has unresolved symbol %s",
                    plugin->name.c_str(),
                    plugin->names[r.sym].c_str());
                relocation_ok = false;
                break;
            }
        } else {
            log("skyline: %s unsupported relocation type %u",
                plugin->name.c_str(), r.type);
            relocation_ok = false;
            break;
        }

        const uint64_t value = target + uint64_t(r.addend);
        std::memcpy(reinterpret_cast<void*>(base + r.offset),
                    &value, sizeof(value));

        switch (r.type) {
        case R_AARCH64_RELATIVE: ++applied_relative; break;
        case R_AARCH64_GLOB_DAT: ++applied_glob_dat; break;
        case R_AARCH64_JUMP_SLOT: ++applied_jump_slot; break;
        case R_AARCH64_ABS64: ++applied_abs64; break;
        }
    }

    log("skyline: %s imports: plugin=%zu HLE=%zu main=%zu SDK=%zu "
        "unresolved=%zu",
        plugin->name.c_str(), imports_plugin, imports_hle,
        imports_main, imports_sdk, imports_unresolved);
    plugin->imports_plugin = imports_plugin;
    plugin->imports_hle = imports_hle;
    plugin->imports_main = imports_main;
    plugin->imports_sdk = imports_sdk;
    plugin->imports_unresolved = imports_unresolved;

    log("skyline: %s relocations applied: RELATIVE=%zu GLOB_DAT=%zu "
        "JUMP_SLOT=%zu ABS64=%zu; targets text=%zu ro=%zu data=%zu "
        "bss=%zu outside=%zu",
        plugin->name.c_str(), applied_relative, applied_glob_dat,
        applied_jump_slot, applied_abs64, targets_text, targets_ro,
        targets_data, targets_bss, targets_outside);
    plugin->reloc_relative = applied_relative;
    plugin->reloc_glob_dat = applied_glob_dat;
    plugin->reloc_jump_slot = applied_jump_slot;
    plugin->reloc_abs64 = applied_abs64;

    if (!relocation_ok) {
        decommit(base, mapped_size);
        vm_set(base, mapped_size, kMemFree, 0);
        log("skyline: %s skipped: relocation failed", plugin_name.c_str());
        return false;
    }

    if (plugin->info.text_size)
        vm_set(base + plugin->info.text, plugin->info.text_size, kMemCode, 5);
    if (plugin->info.ro_size)
        vm_set(base + plugin->info.ro, plugin->info.ro_size, kMemCode, 1);
    const uint64_t data_extent =
        plugin->info.data_size + plugin->info.bss_size;
    if (data_extent)
        vm_set(base + plugin->info.data, data_extent, kMemCodeData, 3);

    LoadedModule loaded{&plugin->info, base, mapped_size, nullptr};
    if (!nce::prepare_module_optional(loaded)) {
        decommit(base, mapped_size);
        vm_set(base, mapped_size, kMemFree, 0);
        log("skyline: %s skipped: NCE preparation failed",
            plugin_name.c_str());
        return false;
    }

    log("skyline: %s mapped at %llx (+%llx), %zu symbols, %zu relocations; "
        "NCE prepared; execution deferred until nnMain",
        plugin_name.c_str(),
        (unsigned long long)base,
        (unsigned long long)mapped_size,
        plugin->symbols.size(),
        plugin->relocs.size());

    g_skyline_plugins.push_back(std::move(plugin));
    return true;
}


static bool skyline_plugin_function(
    const SkylinePluginRuntime& plugin,
    uint64_t address
) {
    const uint64_t text_begin = plugin.base + plugin.info.text;

    return plugin.info.text_size &&
           address >= text_begin &&
           address - text_begin < plugin.info.text_size &&
           (address & 3) == 0 &&
           guest_range_ok(address, sizeof(uint32_t), 1);
}

static void run_skyline_plugins(CPU& c) {
    for (const auto& owned : g_skyline_plugins) {
        SkylinePluginRuntime& plugin = *owned;

        log("skyline: begin controlled execution of %s",
            plugin.name.c_str());

        if (plugin.dt_init) {
            const uint64_t target = plugin.base + plugin.dt_init;

            if (plugin.dt_init < plugin.info.text_size &&
                skyline_plugin_function(plugin, target)) {
                log("skyline: %s calling DT_INIT at %llx",
                    plugin.name.c_str(),
                    (unsigned long long)target);

                call_preserving(c, target);

                log("skyline: %s DT_INIT returned",
                    plugin.name.c_str());
            } else {
                log("skyline: %s skipped invalid DT_INIT offset=%llx",
                    plugin.name.c_str(),
                    (unsigned long long)plugin.dt_init);
            }
        }

        if (plugin.dt_init_array_size) {
            const uint64_t offset = plugin.dt_init_array;
            const uint64_t size = plugin.dt_init_array_size;

            if (!offset ||
                size % sizeof(uint64_t) ||
                size / sizeof(uint64_t) > 4096 ||
                offset > plugin.nro->image.size() ||
                size > plugin.nro->image.size() - offset ||
                offset > plugin.mapped_size ||
                size > plugin.mapped_size - offset ||
                !guest_range_ok(plugin.base + offset, size, 1)) {
                log("skyline: %s rejected DT_INIT_ARRAY offset=%llx size=%llx",
                    plugin.name.c_str(),
                    (unsigned long long)offset,
                    (unsigned long long)size);
            } else {
                const size_t count =
                    static_cast<size_t>(size / sizeof(uint64_t));

                for (size_t i = 0; i < count; ++i) {
                    uint64_t target = 0;

                    std::memcpy(
                        &target,
                        reinterpret_cast<const void*>(
                            plugin.base + offset + i * sizeof(uint64_t)
                        ),
                        sizeof(target)
                    );

                    if (!target)
                        continue;

                    if (!skyline_plugin_function(plugin, target)) {
                        log("skyline: %s skipped invalid init_array[%zu]=%llx",
                            plugin.name.c_str(),
                            i,
                            (unsigned long long)target);
                        continue;
                    }

                    log("skyline: %s calling init_array[%zu] at %llx",
                        plugin.name.c_str(),
                        i,
                        (unsigned long long)target);

                    call_preserving(c, target);

                    log("skyline: %s init_array[%zu] returned",
                        plugin.name.c_str(),
                        i);
                }
            }
        }

        uint64_t entry = 0;

        for (size_t i = 1; i < plugin.symbols.size(); ++i) {
            const Symbol& symbol = plugin.symbols[i];

            if (!symbol.imported && plugin.names[i] == "main") {
                entry = plugin.base + symbol.value;
                break;
            }
        }

        if (!entry || !skyline_plugin_function(plugin, entry)) {
            log("skyline: %s has no valid main entry; execution stopped",
                plugin.name.c_str());
            continue;
        }

        for (unsigned i = 0; i < 8; ++i)
            c.x[i] = 0;

        log("skyline: %s calling plugin main at %llx",
            plugin.name.c_str(),
            (unsigned long long)entry);

        const uint64_t function_hooks_before =
            g_skyline_function_hook_requests;
        const uint64_t inline_hooks_before =
            g_skyline_inline_hook_requests;

        call_preserving(c, entry);
        const bool main_returned_normally = true;

        log("skyline: %s plugin main returned",
            plugin.name.c_str());

        log("skyline: %s post-main hooks: function_requests=%llu "
            "inline_requests=%llu installed=0 main_returned=%s",
            plugin.name.c_str(),
            (unsigned long long)(
                g_skyline_function_hook_requests - function_hooks_before),
            (unsigned long long)(
                g_skyline_inline_hook_requests - inline_hooks_before),
            main_returned_normally ? "yes" : "no");

        log("skyline: %s post-main imports: plugin=%zu HLE=%zu main=%zu "
            "SDK=%zu unresolved=%zu; relocations: RELATIVE=%zu "
            "GLOB_DAT=%zu JUMP_SLOT=%zu ABS64=%zu",
            plugin.name.c_str(),
            plugin.imports_plugin,
            plugin.imports_hle,
            plugin.imports_main,
            plugin.imports_sdk,
            plugin.imports_unresolved,
            plugin.reloc_relative,
            plugin.reloc_glob_dat,
            plugin.reloc_jump_slot,
            plugin.reloc_abs64
        );
    }
}

static void inspect_skyline_nro(const std::filesystem::path& path) {
    (void)load_skyline_nro(path);
}

}  // namespace
#endif

// Run prepared Skyline plugins after game initialization and immediately
// before entering the real game nnMain.
static void nn_main(CPU& c) {
    init_module(c, *g_sdk);
    init_module(c, *g_main);
    game::after_init();

#if NX_NCE
    run_skyline_plugins(c);
#endif

    log("entering nnMain");

    uint64_t real = 0;

    for (size_t k = 0; k < g_main->symbol_count; ++k) {
        const Symbol& symbol = g_main->symbols[k];

        if (!symbol.imported && symbol.name &&
            std::strcmp(symbol.name, "nnMain") == 0) {
            real = g_main->base + symbol.value;
            break;
        }
    }

    if (!real) {
        fatal("real nnMain export not found");
        return;
    }

    call(c, real);
}

NX_HLE("nnMain", nn_main);

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

    // This boot batch intentionally contains only the game's main and SDK modules.
    const ModuleInfo* mods[] = {g_main, g_sdk};
    const uint8_t* images[] = {
        nso[0]->image.data(), nso[1]->image.data()
    };
    load_modules(mods, 2, "", images, static_cast<size_t>(-1));

    // Main/SDK exports are linked before plugin relocation and preparation.
// Plugin constructors and entry points run later from nn_main.
    for (const fs::path& plugin_path : plugin_paths)
        inspect_skyline_nro(plugin_path);
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
