/* SPDX-License-Identifier: GPL-3.0-or-later
 * ABI layout dump, C11 side. Output must match abi_layout_cxx byte for byte. */

#include "support/abi_layout_dump.h"

int main(void) {
    hv_dump_layout();
    return 0;
}
