#include <stdint.h>
#include <stdio.h>

#include "kernel.h"

static int expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected)
        return 1;
    fprintf(stderr, "%s: got 0x%08X, expected 0x%08X\n",
            name, actual, expected);
    return 0;
}

int main(void)
{
    ULONG result = 0xDEADBEEFu;
    const ULONG expected_capabilities =
        AV_PACK_HDTV | (AV_STANDARD_NTSC_M << AV_STANDARD_SHIFT) |
        AV_FLAGS_HDTV_480p | AV_FLAGS_60Hz;

    xbox_AvSendTVEncoderOption(NULL, AV_QUERY_AV_CAPABILITIES, 0, &result);
    if (!expect_u32("AV_QUERY_AV_CAPABILITIES", result,
                    expected_capabilities))
        return 1;
    if (!expect_u32("AV pack field", result & AV_PACK_MASK, AV_PACK_HDTV) ||
        !expect_u32("AV standard field", result & AV_STANDARD_MASK,
                    (AV_STANDARD_NTSC_M << AV_STANDARD_SHIFT)) ||
        !expect_u32("AV HDTV field", result & AV_HDTV_MODE_MASK,
                    AV_FLAGS_HDTV_480p) ||
        !expect_u32("AV refresh field", result & AV_REFRESH_MASK,
                    AV_FLAGS_60Hz))
        return 1;

    /* Numeric guest ABI guard: changing constants and implementation
     * together must not silently change what JSRF's option 6 receives. */
    result = 0;
    xbox_AvSendTVEncoderOption(NULL, 6, 0, &result);
    if (!expect_u32("guest option 6", result, 0x00480104u))
        return 1;

    /* Configuration options used by JSRF permit a NULL result pointer. */
    xbox_AvSendTVEncoderOption(NULL, AV_OPTION_BLANK_SCREEN, 1, NULL);
    xbox_AvSendTVEncoderOption(NULL, AV_OPTION_FLICKER_FILTER, 5, NULL);
    xbox_AvSendTVEncoderOption(NULL, AV_OPTION_ENABLE_LUMA_FILTER, 1, NULL);

    printf("ok  AV_QUERY_AV_CAPABILITIES -> 0x%08X\n", result);
    return 0;
}
