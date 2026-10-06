#pragma once

// Location and first bytes of this engine's PNG loader.
// The launcher checks them before starting an .exe; the hook checks them
// before patching. Per-title: see "Retargeting another game" in the README.
#define ARTEMIS_RVA_PNG_LOADER 0x20BE50UL
#define ARTEMIS_PROLOGUE_LEN 5

static const unsigned char ARTEMIS_PROLOGUE[ARTEMIS_PROLOGUE_LEN] = {
    0x55, 0x8B, 0xEC, 0x6A, 0xFF /* push ebp; mov ebp,esp; push -1 */
};
