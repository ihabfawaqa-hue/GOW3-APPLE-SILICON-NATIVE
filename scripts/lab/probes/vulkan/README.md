# GoW3 Vulkan host probe

This small C++ program measures Vulkan capabilities on the actual Mac and runs representative host workloads. It reads required and optional extension/feature requests from the pinned `gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp` implementation. A diagnostic device may be created with only supported feature bits so independent workloads can run; that device is never reported as renderer-compatible.

## Build and run

```sh
cmake -S scripts/lab/probes/vulkan -B /tmp/gow3_vulkan_probe_build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gow3_vulkan_probe_build -j4
VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  /tmp/gow3_vulkan_probe_build/gow3-vulkan-probe --output /tmp/gow3-moltenvk-probe.json
```

For KosmicKrisp from LunarG Vulkan SDK 1.4.363.0, source the SDK environment
script and select its ICD explicitly through the Vulkan loader:

```sh
source "$HOME/VulkanSDK/1.4.363.0/setup-env.sh"
VK_DRIVER_FILES="$VULKAN_SDK/share/vulkan/icd.d/libkosmickrisp_icd.json" \
  /tmp/gow3_vulkan_probe_build/gow3-vulkan-probe --output /tmp/gow3-kosmickrisp-probe.json
```

The SDK must be installed and its license accepted through LunarG's installer.
Use the SDK's per-user destination and leave optional system-wide components
unselected for this probe. The probe runs through the Vulkan loader; direct
linking to MoltenVK bypasses ICD selection. See [LunarG's macOS Vulkan SDK
guide](https://vulkan.lunarg.com/doc/view/latest/mac/getting_started.html) for
the current driver setup and loader requirements.

`--source-instance` intentionally reproduces the pinned source's pre-portability-fix macOS instance policy. On MoltenVK this returns `VK_ERROR_INCOMPATIBLE_DRIVER` before device enumeration. The default probe enables portability enumeration and the portability subset extension when advertised, matching the local Apple source patch; it then separately attempts the unmodified required renderer feature set and a diagnostic supported-feature device.

Exit status is `0` only when the renderer-required device and all required workloads pass, `1` for a completed partial/incompatible capability result, and `2` for probe setup or argument errors. The JSON `verdict` is authoritative; a diagnostic workload pass does not make the renderer compatible.

The probe performs bounded swapchain present, push-descriptor compute with storage-buffer and storage-image readback, offscreen clear/barrier/fence/readback, and aliased-image plus UNORM/sRGB-view checks. It also records feature/extension support and image-format capabilities. It does not initialize the GoW3 renderer or consume guest assets.

The JSON reports core `robustBufferAccess` separately from `VK_EXT_robustness2`'s `robustBufferAccess2`, `robustImageAccess2`, and `nullDescriptor`. It also reports the buffer-cache requirements discovered in the pinned source: core `sparseBinding`, `sparseResidencyBuffer`, and Vulkan 1.2 `bufferDeviceAddress`. `source_buffer_cache_features_present` is false if any of those are missing. The renderer creates 1–4 GiB sparse virtual buffer arenas and builds a BDA page table over resident guest-memory ranges, so a device that lacks sparse buffer residency is not renderer-compatible even if presentation and ordinary buffer readback pass.
