// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/buffer_cache/region_definitions.h"

#include <cstddef>
#include <cstdio>
#include <unistd.h>

int main() {
    const long host_page_size = sysconf(_SC_PAGESIZE);
    const auto tracker_page_size = VideoCore::TRACKER_BYTES_PER_PAGE;
    const bool matches = host_page_size > 0 &&
                         static_cast<unsigned long>(host_page_size) == tracker_page_size;
    std::printf("{\"host_page_size\":%ld,\"tracker_page_size\":%zu,\"verdict\":\"%s\"}\n",
                host_page_size, static_cast<std::size_t>(tracker_page_size),
                matches ? "PASS" : "FAIL");
    return matches ? 0 : 1;
}
