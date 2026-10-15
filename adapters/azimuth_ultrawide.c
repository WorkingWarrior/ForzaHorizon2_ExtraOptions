#include "azimuth_host.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"

/* Azimuth 0.9.7's virtual DXGI output reports one 1920x1080 mode to FH2.
   Its desktop rectangle and mode list must agree. The launcher still chooses
   the real fullscreen surface and the render height from azimuth.ini. */
static const unsigned char width_3440[] = {0x70, 0x0d, 0x00, 0x00};
static const unsigned char height_1440[] = {0xa0, 0x05, 0x00, 0x00};
static const HostCodePatch output_description_patches[] = {
    {0x2e, sizeof(width_3440), width_3440},
    {0x35, sizeof(height_1440), height_1440}};
static const HostCodePatch output_mode_patches[] = {
    {0x41, sizeof(width_3440), width_3440},
    {0x48, sizeof(height_1440), height_1440}};

static const unsigned char description_original_hash[32] = {
    0x61, 0xa9, 0x85, 0x45, 0x92, 0xbb, 0x62, 0xd0, 0x39, 0x40, 0x63, 0x03, 0x38, 0x36, 0xbe, 0xb5,
    0x01, 0x52, 0x5e, 0xe0, 0x24, 0x59, 0x64, 0x68, 0x87, 0x4d, 0xe4, 0x9e, 0x9f, 0x70, 0x03, 0x4a};
static const unsigned char description_patched_hash[32] = {
    0xb9, 0xa6, 0x83, 0xcc, 0xb4, 0x71, 0x4e, 0x10, 0x75, 0x45, 0x74, 0x90, 0xda, 0xa7, 0xd8, 0xad,
    0xad, 0xd9, 0x4e, 0x25, 0x79, 0x80, 0xb5, 0xd8, 0xb3, 0x0b, 0x98, 0x6f, 0x04, 0x47, 0x44, 0x8e};
static const unsigned char mode_original_hash[32] = {
    0x1d, 0xe5, 0xdd, 0x32, 0x59, 0xb6, 0x0f, 0x49, 0xd3, 0x89, 0x7f, 0x69, 0x3e, 0x20, 0x0d, 0x8b,
    0xe6, 0xf7, 0xc9, 0x45, 0x2e, 0x67, 0x0c, 0xde, 0x73, 0x53, 0x6f, 0xf3, 0x17, 0x26, 0x82, 0x40};
static const unsigned char mode_patched_hash[32] = {
    0x9e, 0x0b, 0x1c, 0x30, 0x4a, 0x46, 0x8b, 0x84, 0x5f, 0x6d, 0x71, 0x6d, 0xb2, 0xb2, 0xc3, 0x3c,
    0x60, 0xdb, 0x50, 0xe8, 0xe9, 0x92, 0x36, 0xa2, 0x5f, 0x64, 0x67, 0xc0, 0xc7, 0x5e, 0xf4, 0x39};

static const HostPatchSet output_patches[] = {
    {0x3d7d0, 0x48, description_original_hash, description_patched_hash,
     output_description_patches, ARRAY_COUNT(output_description_patches)},
    {0x3d820, 0x6f, mode_original_hash, mode_patched_hash,
     output_mode_patches, ARRAY_COUNT(output_mode_patches)}};

/* RenderBuffer11 stores the dimensions used for both swap-chain creation and
   DXGIXPresentArray's source rectangle. Change its width before creation. */
typedef __int64(WINAPI *CreateGameSwapChainFn)(void *, void *, int, int);
static CreateGameSwapChainFn original_create_swapchain;
static int swapchain_logged;
static const unsigned char swapchain_entry[17] = {
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xa0};
static const unsigned char swapchain_hash[32] = {
    0xa1, 0x5c, 0x1d, 0xa5, 0x98, 0x76, 0xff, 0xf8, 0x9e, 0xc6, 0xcc, 0xe2, 0xfb, 0x07, 0x35, 0xc9,
    0x7a, 0x2e, 0x53, 0x7b, 0xab, 0xb2, 0x54, 0x48, 0x50, 0x32, 0x59, 0xc8, 0xe1, 0x10, 0x30, 0x70};

/* This render-target initializer derives its width as height * 16 * (1/9).
   Give only this instruction 43/288; the shared 1/9 constant also belongs
   to unrelated rendering code. At height 1440 this yields width 3440. */
static const unsigned char render_width_instruction[8] = {
    0xc5, 0xf2, 0x59, 0x15, 0xd9, 0xb0, 0xbd, 0x01};
static const unsigned char render_width_hash[32] = {
    0xbe, 0x8c, 0x50, 0x44, 0xe2, 0xdd, 0x96, 0xce, 0x6b, 0x00, 0x64, 0x42, 0x9b, 0xd1, 0xcb, 0x0d,
    0x90, 0xf1, 0x7b, 0x0f, 0x05, 0xa3, 0xf6, 0x3d, 0xd1, 0xc0, 0xf6, 0xb1, 0xef, 0x98, 0x27, 0x01};

static int install_render_width(unsigned char *game_image) {
    unsigned char *site = game_image + 0x2211a3;
    unsigned char inspected[0x20], digest[32];
    Sha256Context hash;
    if (!memory_read(site, inspected, sizeof(inspected)) ||
        !memory_equal(inspected, render_width_instruction, sizeof(render_width_instruction))) {
        return 0;
    }
    sha256_initialize(&hash);
    sha256_update(&hash, inspected, sizeof(inspected));
    sha256_finalize(&hash, digest);
    if (!memory_equal(digest, render_width_hash, sizeof(digest))) {
        return 0;
    }
    uintptr_t candidate = ((uintptr_t)game_image + GAME_IMAGE_SIZE + 0xffffu) & ~(uintptr_t)0xffffu;
    unsigned char *constant = NULL;
    for (unsigned attempt = 0; attempt < 256 && !constant; ++attempt) {
        constant = (unsigned char *)VirtualAlloc((void *)(candidate + attempt * 0x100000u),
                                                  4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!constant) {
        return -1;
    }
    int64_t distance = (int64_t)(uintptr_t)constant - (int64_t)(uintptr_t)(site + 8);
    if (distance < INT32_MIN || distance > INT32_MAX) {
        VirtualFree(constant, 0, MEM_RELEASE);
        return -1;
    }
    const uint32_t factor_bits = 0x3e18e38eu;
    memory_copy(constant, &factor_bits, sizeof(factor_bits));
    DWORD previous;
    if (!VirtualProtect(constant, 4096, PAGE_READONLY, &previous)) {
        VirtualFree(constant, 0, MEM_RELEASE);
        return -1;
    }
    DWORD old_protection;
    if (!VirtualProtect(site, sizeof(render_width_instruction), PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        VirtualFree(constant, 0, MEM_RELEASE);
        return -1;
    }
    int32_t displacement = (int32_t)distance;
    memory_copy(site + 4, &displacement, sizeof(displacement));
    DWORD ignored;
    if (!FlushInstructionCache(GetCurrentProcess(), site, sizeof(render_width_instruction)) ||
        !VirtualProtect(site, sizeof(render_width_instruction), old_protection, &ignored)) {
        VirtualProtect(site, sizeof(render_width_instruction), PAGE_EXECUTE_READWRITE, &ignored);
        memory_copy(site, inspected, sizeof(render_width_instruction));
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(render_width_instruction));
        VirtualProtect(site, sizeof(render_width_instruction), old_protection, &ignored);
        VirtualFree(constant, 0, MEM_RELEASE);
        return -1;
    }
    return 1;
}

static void absolute_jump(unsigned char *code, const void *target) {
    static const unsigned char prefix[6] = {0xff, 0x25, 0, 0, 0, 0};
    uintptr_t address = (uintptr_t)target;
    memory_copy(code, prefix, sizeof(prefix));
    memory_copy(code + sizeof(prefix), &address, sizeof(address));
}

static __int64 WINAPI ultrawide_create_swapchain(void *buffer, void *configuration, int format,
                                                  int profile) {
    unsigned *size = (unsigned *)buffer;
    if (size[2] == 2560 && size[3] == 1440) {
        size[2] = 3440;
        if (!swapchain_logged) {
            swapchain_logged = 1;
            log_append("ULTRAWIDE: game RenderBuffer11 changed from 2560x1440 to 3440x1440.\r\n");
        }
    } else if (!swapchain_logged) {
        swapchain_logged = 1;
        log_append("ULTRAWIDE: game RenderBuffer11 did not contain 2560x1440; unchanged.\r\n");
    }
    return original_create_swapchain(buffer, configuration, format, profile);
}

static int install_swapchain_hook(unsigned char *game_image) {
    unsigned char *entry = game_image + 0x10ca800;
    unsigned char inspected[0x50];
    unsigned char digest[32];
    Sha256Context hash;
    if (!memory_read(entry, inspected, sizeof(inspected)) ||
        !memory_equal(inspected, swapchain_entry, sizeof(swapchain_entry))) {
        return 0;
    }
    sha256_initialize(&hash);
    sha256_update(&hash, inspected, sizeof(inspected));
    sha256_finalize(&hash, digest);
    if (!memory_equal(digest, swapchain_hash, sizeof(digest))) {
        return 0;
    }
    HMODULE pinned;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)(uintptr_t)ultrawide_create_swapchain, &pinned)) {
        return -1;
    }
    unsigned char *trampoline =
        (unsigned char *)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!trampoline) {
        return -1;
    }
    memory_copy(trampoline, swapchain_entry, sizeof(swapchain_entry));
    absolute_jump(trampoline + sizeof(swapchain_entry), entry + sizeof(swapchain_entry));
    DWORD trampoline_old;
    if (!VirtualProtect(trampoline, 64, PAGE_EXECUTE_READ, &trampoline_old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return -1;
    }
    original_create_swapchain = (CreateGameSwapChainFn)(uintptr_t)trampoline;
    DWORD old_protection;
    if (!VirtualProtect(entry, sizeof(swapchain_entry), PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        original_create_swapchain = NULL;
        return -1;
    }
    unsigned char branch[17] = {0};
    absolute_jump(branch, ultrawide_create_swapchain);
    branch[14] = 0x90;
    branch[15] = 0x90;
    branch[16] = 0x90;
    memory_copy(entry, branch, sizeof(branch));
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(branch));
    DWORD ignored;
    VirtualProtect(entry, sizeof(branch), old_protection, &ignored);
    return 1;
}

void azimuth_apply_ultrawide(int enabled, unsigned char *game_image) {
    if (!enabled) {
        return;
    }
    unsigned char *host = (unsigned char *)GetModuleHandleW(NULL);
    if (azimuth_host_build(host) != AZIMUTH_097) {
        log_append("ULTRAWIDE: unsupported Azimuth build; virtual display left unchanged.\r\n");
        return;
    }
    int result = azimuth_patch_host_codes(host, output_patches, ARRAY_COUNT(output_patches));
    if (result > 0) {
        log_append("ULTRAWIDE: Azimuth virtual DXGI output set to 3440x1440.\r\n");
    } else if (result < 0) {
        log_append("ULTRAWIDE: could not change Azimuth memory protection; output left unchanged.\r\n");
    } else {
        log_append("ULTRAWIDE: unexpected Azimuth output code; output left unchanged.\r\n");
    }
    if (result <= 0) {
        return;
    }
    int hook_result = install_swapchain_hook(game_image);
    if (hook_result > 0) {
        log_append("ULTRAWIDE: game RenderBuffer11 swap-chain hook installed.\r\n");
        int width_result = install_render_width(game_image);
        if (width_result > 0) {
            log_append("ULTRAWIDE: game render-target width changed to 3440 at height 1440.\r\n");
        } else if (width_result < 0) {
            log_append("ULTRAWIDE: could not install game render-target width patch.\r\n");
        } else {
            log_append("ULTRAWIDE: unexpected game render-target code; width patch skipped.\r\n");
        }
    } else if (hook_result < 0) {
        log_append("ULTRAWIDE: could not install game swap-chain hook.\r\n");
    } else {
        log_append("ULTRAWIDE: unexpected game swap-chain code; hook skipped.\r\n");
    }
}
