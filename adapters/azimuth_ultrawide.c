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

/* This render-target initializer derives its width as height * 16 * (1/9).
   Give only this instruction 43/288; the shared 1/9 constant also belongs
   to unrelated rendering code. At height 1440 this yields width 3440. */
static const unsigned char render_width_instruction[8] = {
    0xc5, 0xf2, 0x59, 0x15, 0xd9, 0xb0, 0xbd, 0x01};
static const unsigned char render_width_hash[32] = {
    0xbe, 0x8c, 0x50, 0x44, 0xe2, 0xdd, 0x96, 0xce, 0x6b, 0x00, 0x64, 0x42, 0x9b, 0xd1, 0xcb, 0x0d,
    0x90, 0xf1, 0x7b, 0x0f, 0x05, 0xa3, 0xf6, 0x3d, 0xd1, 0xc0, 0xf6, 0xb1, 0xef, 0x98, 0x27, 0x01};

typedef struct {
    unsigned char *site;
    unsigned char *factor;
    unsigned char original[sizeof(render_width_instruction)];
    DWORD original_protection;
} RenderWidthPatch;

/* Keep the factor allocated if rollback cannot make the instruction safe: the
   processor may still execute a cached instruction that reads this address. */
static int restore_render_width(RenderWidthPatch *patch) {
    DWORD ignored;
    if (!VirtualProtect(patch->site, sizeof(patch->original), PAGE_EXECUTE_READWRITE, &ignored)) {
        return 0;
    }
    memory_copy(patch->site, patch->original, sizeof(patch->original));
    int flushed = FlushInstructionCache(GetCurrentProcess(), patch->site,
                                        sizeof(patch->original)) != 0;
    int protected = VirtualProtect(patch->site, sizeof(patch->original),
                                   patch->original_protection, &ignored) != 0;
    if (!flushed || !protected) {
        return 0;
    }
    VirtualFree(patch->factor, 0, MEM_RELEASE);
    patch->factor = NULL;
    return 1;
}

static int install_render_width(unsigned char *game_image, RenderWidthPatch *patch) {
    patch->site = game_image + 0x2211a3;
    patch->factor = NULL;
    unsigned char inspected[0x20], digest[32];
    Sha256Context hash;
    if (!memory_read(patch->site, inspected, sizeof(inspected)) ||
        !memory_equal(inspected, render_width_instruction, sizeof(render_width_instruction))) {
        return 0;
    }
    sha256_initialize(&hash);
    sha256_update(&hash, inspected, sizeof(inspected));
    sha256_finalize(&hash, digest);
    if (!memory_equal(digest, render_width_hash, sizeof(digest))) {
        return 0;
    }
    memory_copy(patch->original, inspected, sizeof(patch->original));
    uintptr_t candidate = ((uintptr_t)game_image + GAME_IMAGE_SIZE + 0xffffu) & ~(uintptr_t)0xffffu;
    for (unsigned attempt = 0; attempt < 256 && !patch->factor; ++attempt) {
        patch->factor = (unsigned char *)VirtualAlloc((void *)(candidate + attempt * 0x100000u),
                                                      4096, MEM_RESERVE | MEM_COMMIT,
                                                      PAGE_READWRITE);
    }
    if (!patch->factor) {
        return -1;
    }
    int64_t distance = (int64_t)(uintptr_t)patch->factor -
                       (int64_t)(uintptr_t)(patch->site + sizeof(patch->original));
    if (distance < INT32_MIN || distance > INT32_MAX) {
        VirtualFree(patch->factor, 0, MEM_RELEASE);
        patch->factor = NULL;
        return -1;
    }
    const uint32_t factor_bits = 0x3e18e38eu;
    memory_copy(patch->factor, &factor_bits, sizeof(factor_bits));
    DWORD previous;
    if (!VirtualProtect(patch->factor, 4096, PAGE_READONLY, &previous)) {
        VirtualFree(patch->factor, 0, MEM_RELEASE);
        patch->factor = NULL;
        return -1;
    }
    if (!VirtualProtect(patch->site, sizeof(patch->original), PAGE_EXECUTE_READWRITE,
                        &patch->original_protection)) {
        VirtualFree(patch->factor, 0, MEM_RELEASE);
        patch->factor = NULL;
        return -1;
    }
    int32_t displacement = (int32_t)distance;
    memory_copy(patch->site + 4, &displacement, sizeof(displacement));
    DWORD ignored;
    int flushed = FlushInstructionCache(GetCurrentProcess(), patch->site,
                                        sizeof(patch->original)) != 0;
    int protected = VirtualProtect(patch->site, sizeof(patch->original),
                                   patch->original_protection, &ignored) != 0;
    if (!flushed || !protected) {
        return restore_render_width(patch) ? -1 : -2;
    }
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
    RenderWidthPatch render_width;
    int width_result = install_render_width(game_image, &render_width);
    if (width_result == 0) {
        log_append("ULTRAWIDE: unexpected game render-target code; output left unchanged.\r\n");
        return;
    }
    if (width_result < 0) {
        log_append(width_result == -2
                       ? "ULTRAWIDE: render-target rollback failed; constant retained.\r\n"
                       : "ULTRAWIDE: could not install game render-target width patch.\r\n");
        return;
    }
    int output_result = azimuth_patch_host_codes(host, output_patches, ARRAY_COUNT(output_patches));
    if (output_result <= 0) {
        int rolled_back = restore_render_width(&render_width);
        log_append(output_result < 0
                       ? "ULTRAWIDE: could not change Azimuth output.\r\n"
                       : "ULTRAWIDE: unexpected Azimuth output code.\r\n");
        log_append(rolled_back
                       ? "ULTRAWIDE: game render-target patch rolled back.\r\n"
                       : "ULTRAWIDE: game render-target rollback failed; constant retained.\r\n");
        return;
    }
    log_append("ULTRAWIDE: Azimuth output and game render target set to 3440x1440.\r\n");
}
