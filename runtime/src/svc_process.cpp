// Process-level SVCs and state that do not touch the scheduler: svcGetInfo, thread core masks, transfer memory
// mappings, and the initial static TLS every thread starts with. kernel.cpp dispatches to these.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"

namespace nx {

namespace {
uint64_t g_random_entropy[4]{};
bool g_random_entropy_initialized = false;

enum class EntropySource {
    GetRandom,
    Urandom,
    RandomDevice,
    ClockAddressMix
};

const char* entropy_source_name(EntropySource source) {
    switch (source) {
        case EntropySource::GetRandom: return "getrandom";
        case EntropySource::Urandom: return "/dev/urandom";
        case EntropySource::RandomDevice: return "random_device";
        default: return "clock+address-mix";
    }
}

uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void clock_address_entropy(uint8_t* bytes, size_t size) {
    uint64_t stack_marker = 0;
    const uint64_t stack_address = reinterpret_cast<uintptr_t>(&stack_marker);
    uint64_t buffer_address = reinterpret_cast<uintptr_t>(bytes);
    uint64_t state =
        static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count()) ^
        (static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) << 1) ^
        buffer_address ^ (stack_address << 7);

#if defined(__linux__)
    state ^= static_cast<uint64_t>(::getpid()) << 32;
#endif

    size_t offset = 0;
    while (offset < size) {
        const uint64_t value = splitmix64(state);
        const size_t count = std::min(sizeof(value), size - offset);
        std::memcpy(bytes + offset, &value, count);
        offset += count;
    }
}

bool acquire_entropy(uint8_t* bytes, size_t size, EntropySource& source) {
    if (!bytes && size)
        return false;
    if (!size) {
        source = EntropySource::GetRandom;
        return true;
    }

#if defined(__linux__)
    size_t filled = 0;
    while (filled < size) {
        const long result = ::syscall(
            SYS_getrandom, bytes + filled, size - filled, 0);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            break;
        filled += static_cast<size_t>(result);
    }
    if (filled == size) {
        source = EntropySource::GetRandom;
        return true;
    }

    // Discard partial getrandom output and retry the complete request.
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        filled = 0;
        while (filled < size) {
            const ssize_t result =
                ::read(fd, bytes + filled, size - filled);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                break;
            filled += static_cast<size_t>(result);
        }
        ::close(fd);
        if (filled == size) {
            source = EntropySource::Urandom;
            return true;
        }
    }
#else
    try {
        std::random_device random;
        size_t filled = 0;
        while (filled < size) {
            const unsigned int value = random();
            const size_t count = std::min(sizeof(value), size - filled);
            std::memcpy(bytes + filled, &value, count);
            filled += count;
        }
        source = EntropySource::RandomDevice;
        return true;
    } catch (...) {
        // Use the last-resort mixer below.
    }
#endif

    static std::atomic_flag fallback_warning = ATOMIC_FLAG_INIT;
    if (!fallback_warning.test_and_set(std::memory_order_relaxed)) {
        log("WARNING: entropy: secure entropy sources unavailable; "
            "using clock+address fallback");
    }
    clock_address_entropy(bytes, size);
    source = EntropySource::ClockAddressMix;
    return true;
}

uint32_t entropy_hash(const uint8_t* bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}
}

bool generate_random_bytes(uint8_t* bytes, size_t size) {
    EntropySource source{};
    return acquire_entropy(bytes, size, source);
}

void init_random_entropy() {
    if (g_random_entropy_initialized)
        return;

    uint8_t bytes[sizeof(g_random_entropy)]{};
    EntropySource source{};
    if (!acquire_entropy(bytes, sizeof(bytes), source))
        fatal("entropy: could not generate process entropy");

    std::memcpy(g_random_entropy, bytes, sizeof(g_random_entropy));

    // Keep all four RandomEntropy subtypes distinct.
    for (size_t i = 0; i < 4; ++i) {
        for (;;) {
            bool duplicate = false;
            for (size_t j = 0; j < i; ++j) {
                if (g_random_entropy[i] == g_random_entropy[j]) {
                    uint64_t state = g_random_entropy[i] ^
                        (0xD1B54A32D192ED03ull * (i + 1));
                    g_random_entropy[i] = splitmix64(state);
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                break;
        }
    }

    log("entropy source=%s", entropy_source_name(source));
    log("entropy-hash=%08x",
        static_cast<unsigned>(entropy_hash(
        reinterpret_cast<const uint8_t*>(g_random_entropy),
        sizeof(g_random_entropy))));
    g_random_entropy_initialized = true;
}

// ---- svcGetInfo -------------------------------------------------------------------------------------------------

uint64_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint32_t& rc) {
    (void)handle;
    rc = 0;
    switch (type) {
        case 0: return 0xF;                         // AllowedCpuIdBitmask
        case 1: return 0xFFFFFFFFFFFFFFFFull >> 0;  // AllowedThreadPriorityMask
        case 2: return kAliasBase;
        case 3: return kAliasSize;
        case 4: return kHeapBase;
        case 5: return kHeapMax;
        case 6: return 0xCD500000;  // TotalMemoryAvailable (~3.2 GiB, like an application)
        case 7: return heap_size() + 0x4000000;
        case 8: return 0;  // IsCurrentProcessBeingDebugged
        case 9: return 0;  // ResourceLimit handle
        case 10: return 0;  // IdleTickCount
        case 11:
            if (sub < 4)
                return g_random_entropy[sub];  // RandomEntropy
            rc = 0xF001;
            return 0;
        case 12: return kAddressSpaceBase;
        case 13: return kAddressSpaceSize;
        case 14: return kStackBase;
        case 15: return kStackSize;
        case 16: return game::kInfo.system_resource_size;  // SystemResourceSizeTotal
        case 17: return 0;
        case 18: return game::kInfo.program_id;  // ProgramId
        case 20: {  // UserExceptionContextAddress: a page of ours (SDK 16 keeps its dying-message region at +0x1C0)
            static uint64_t ctx = kernel_alloc(0x1000);
            return ctx;
        }
        case 21: return 0xCD500000;
        case 22: return heap_size() + 0x4000000;
        case 23: return 1;  // IsApplication
        case 0xF0000002: return 0;  // ThreadTickCount
        default:
            log("svcGetInfo: unknown type %u sub %llu", type, (unsigned long long)sub);
            rc = 0xF001;
            return 0;
    }
}

// ---- svcSetThreadCoreMask ---------------------------------------------------------------------------------------

// (handle, ideal core, affinity mask): the core a thread then reports with svcGetCurrentProcessorNumber. Engines
// keep one job queue per core and pick theirs that way (Wonder's workers never saw their jobs while every thread
// reported core 0).
uint32_t set_thread_core_mask(uint32_t handle, int32_t ideal, uint64_t mask) {
    auto t = std::dynamic_pointer_cast<KThread>(handle_get(handle));
    if (!t || ideal == -3) return 0;  // -3: keep the ideal core
    if (ideal == -2) ideal = 0;       // the process's default core (main.npdm)
    if (ideal < 0 || !((mask >> ideal) & 1))
        for (int k = 0; k < 4; k++)
            if ((mask >> k) & 1) {
                ideal = k;
                break;
            }
    if (ideal >= 0) t->core = ideal;
    return 0;
}

// ---- transfer memory --------------------------------------------------------------------------------------------

// MapTransferMemory(handle, addr, size, perm): the owner cannot touch the memory while it is mapped, so a copy
// behaves the same (SDK 16's in-process audio renderer maps its work buffer this way).
uint32_t map_transfer_memory(uint32_t handle, uint64_t addr, uint64_t size) {
    auto tm = std::dynamic_pointer_cast<KTransferMemory>(handle_get(handle));
    if (!tm) return 0xE401;
    commit(addr, size);
    std::memcpy(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(tm->addr), std::min(size, tm->size));
    vm_set(addr, size, kMemAlias, 3);
    return 0;
}

// UnmapTransferMemory(handle, addr, size): the contents go back to the owner.
uint32_t unmap_transfer_memory(uint32_t handle, uint64_t addr, uint64_t size) {
    auto tm = std::dynamic_pointer_cast<KTransferMemory>(handle_get(handle));
    if (tm) std::memcpy(reinterpret_cast<void*>(tm->addr), reinterpret_cast<void*>(addr), std::min(size, tm->size));
    decommit(addr, size);
    vm_set(addr, size, kMemFree, 0);
    return 0;
}

// ---- static TLS -------------------------------------------------------------------------------------------------

namespace {
std::vector<uint8_t> g_tls_template;
}

void set_tls_template(uint64_t start, uint64_t end) {
    g_tls_template.assign(reinterpret_cast<const uint8_t*>(start), reinterpret_cast<const uint8_t*>(end));
}

void init_tls_block(uint64_t block) {
    if (!g_tls_template.empty()) std::memcpy(reinterpret_cast<void*>(block + 0x10), g_tls_template.data(), g_tls_template.size());
}

}  // namespace nx
