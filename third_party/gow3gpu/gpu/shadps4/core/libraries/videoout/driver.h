// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <functional>

#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "core/libraries/videoout/video_out.h"

#include <condition_variable>
#include <mutex>
#include <queue>

namespace Vulkan {
struct Frame;
}

namespace Libraries::VideoOut {

struct VideoOutPort {
    SceVideoOutResolutionStatus resolution;
    std::array<VideoOutBuffer, MaxDisplayBuffers> buffer_slots;
    // Guest-visible labels must be backed by the runtime's authoritative guest
    // mapping. Keep the stable host view and guest VA separately.
    u64* buffer_labels{};
    u64 buffer_labels_guest_address{};
    static_assert(sizeof(u64) == 8u);
    std::array<BufferAttributeGroup, MaxDisplayBufferGroups> groups;
    FlipStatus flip_status;
    SceVideoOutVblankStatus vblank_status;
    std::vector<Kernel::OrbisKernelEqueue> flip_events;
    std::vector<Kernel::OrbisKernelEqueue> vblank_events;
    std::mutex vo_mutex;
    std::mutex port_mutex;
    std::condition_variable vo_cv;
    std::condition_variable vblank_cv;
    int flip_rate = 0;
    int prev_index = -1;
    bool is_open = false;
    bool is_hdr = false;

    s32 FindFreeGroup() const {
        s32 index = 0;
        while (index < groups.size() && groups[index].is_occupied) {
            index++;
        }
        return index;
    }

    bool IsVoLabel(const u64* address) const {
        if (!address || !buffer_labels) return false;
        const auto candidate = reinterpret_cast<std::uintptr_t>(address);
        const auto start = reinterpret_cast<std::uintptr_t>(buffer_labels);
        return candidate >= start && candidate - start <
               sizeof(u64) * MaxDisplayBuffers &&
               (candidate - start) % sizeof(u64) == 0;
    }

    void WaitVoLabel(auto&& pred) {
        std::unique_lock lk{vo_mutex};
        vo_cv.wait(lk, pred);
    }

    void SignalVoLabel() {
        std::scoped_lock lk{vo_mutex};
        vo_cv.notify_one();
    }

    [[nodiscard]] int NumRegisteredBuffers() const {
        return std::count_if(buffer_slots.cbegin(), buffer_slots.cend(),
                             [](auto& buffer) { return buffer.group_index != -1; });
    }
};

struct ServiceThreadParams {
    u32 unknown;
    bool set_priority;
    u32 priority;
    bool set_affinity;
    u64 affinity;
};

class VideoOutDriver {
public:
    VideoOutDriver(u32 width, u32 height);
    ~VideoOutDriver();

    int Open(const ServiceThreadParams* params);
    void Close(s32 handle);

    VideoOutPort* GetPort(s32 handle);

    int RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses, s32 bufferNum,
                        const BufferAttribute* attribute);
    int UnregisterBuffers(VideoOutPort* port, s32 attributeIndex);
    int ChangeBufferAttribute(VideoOutPort* port, s32 bufferIndex,
                              const BufferAttribute* attribute);

    bool SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);

private:
    struct Request {
        Vulkan::Frame* frame;
        VideoOutPort* port;
        s64 flip_arg;
        s32 index;
        bool eop;

        operator bool() const noexcept {
            return frame != nullptr;
        }
    };

    void Flip(const Request& req);
    void DrawBlankFrame(); // Video port out not open
    void DrawLastFrame();  // Used when there is no flip request
    void SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);
    void PresentThread(std::stop_token token);
    /// gow3: runs presenter work (acquire, blit, vkQueuePresent) off the vblank thread, in
    /// order. Acquire and present block while the GPU is the bottleneck (much longer on NVIDIA);
    /// on the vblank thread that delayed the guest's vblank events and its frame timing jumped.
    /// The presenter's frame pool still bounds how far the GPU command thread runs ahead.
    void SwapThread(std::stop_token token);
    void RunPresenter(std::function<void()> work, bool if_idle = false);

    std::mutex swap_mutex;
    std::condition_variable_any swap_cv;
    std::deque<std::function<void()>> swap_queue;
    bool swap_busy = false;
    bool separate_swap = true;
    std::jthread swap_thread; ///< before present_thread: destroyed after it

    std::mutex mutex;
    std::condition_variable request_cv; ///< gow3: a flip was queued (immediate flips)
    VideoOutPort main_port{};
    std::jthread present_thread;
    std::queue<Request> requests;
};

} // namespace Libraries::VideoOut
