/* SPDX-License-Identifier: GPL-3.0-or-later
 * ABI layout dump, C++20 side. Output must match abi_layout_c byte for byte. */

#include "abi_layout_dump.h"

int main() {
    hv_dump_layout();
    return 0;
}
