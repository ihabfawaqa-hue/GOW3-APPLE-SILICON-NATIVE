// SPDX-License-Identifier: GPL-2.0-or-later
// Port file (not upstream). shadPS4 dereferences guest addresses as host pointers. This port
// routes each such dereference through GuestToHost, which the host implements over its own
// guest memory mapping.

#pragma once

#include "common/types.h"

namespace Shader {

// Maps a guest virtual address, as stored in user data, to host memory that can be read.
// Returns nullptr when the address is not mapped.
const void* GuestToHost(u64 guest_addr);

} // namespace Shader
