// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <execinfo.h>
#include <set>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "gow3_copy.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/host_timing.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "gow3_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    // gow3: GOW3_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("GOW3_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

namespace {
struct PassEndSite {
    const char* file;
    unsigned line;
    u64 count;
    u64 resumed; ///< the same pass began again right after this end: a split
};
std::mutex pass_end_mutex;
std::array<PassEndSite, 48> pass_end_sites{};
PassEndSite* last_pass_end{};
RenderState last_ended_state{};
} // namespace

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && RenderStateContinues(new_state)) {
        return;
    }
    EndRendering();
    if (WaitDiagnostics::Enabled()) {
        std::scoped_lock lock{pass_end_mutex};
        if (last_pass_end && last_ended_state.ContinuesWith(new_state)) {
            ++last_pass_end->resumed;
        }
        last_pass_end = nullptr;
    }
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    if (PassLabelsEnabled() && !pass_label.empty()) {
        if (!recording_threaded) {
            SetObjectName(instance.GetDevice(), current_cmdbuf, pass_label);
        } else {
            Record([device = instance.GetDevice(), label = pass_label](vk::CommandBuffer cmdbuf) {
                SetObjectName(device, cmdbuf, label);
            });
        }
    }
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
}


void Scheduler::ReportPassEnds() {
    if (!WaitDiagnostics::Enabled()) {
        return;
    }
    std::array<PassEndSite, 48> sites;
    {
        std::scoped_lock lock{pass_end_mutex};
        sites = pass_end_sites;
        pass_end_sites = {};
        last_pass_end = nullptr;
    }
    std::sort(sites.begin(), sites.end(),
              [](const PassEndSite& a, const PassEndSite& b) { return a.count > b.count; });
    for (const auto& site : sites) {
        if (!site.count) {
            break;
        }
        const std::string_view file{site.file};
        const auto name = file.substr(file.find_last_of("/\\") + 1);
        std::printf("Pass end: %.*s:%u; %llu, of which the same pass resumed %llu\n",
                    int(name.size()), name.data(), site.line,
                    static_cast<unsigned long long>(site.count),
                    static_cast<unsigned long long>(site.resumed));
    }
}

void Scheduler::EndRendering(std::source_location caller) {
    if (!is_rendering) {
        return;
    }
    if (WaitDiagnostics::Enabled()) {
        std::scoped_lock lock{pass_end_mutex};
        for (auto& site : pass_end_sites) {
            if (!site.count || (site.line == caller.line() &&
                                std::string_view{site.file} == caller.file_name())) {
                site.file = caller.file_name();
                site.line = caller.line();
                ++site.count;
                last_pass_end = &site;
                last_ended_state = render_state;
                break;
            }
        }
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

namespace {
std::atomic<u32> recording_reports{0};

u64 CurrentThreadKey() {
    return static_cast<u64>(reinterpret_cast<uintptr_t>(pthread_self()));
}
std::string ThreadName(u64 key) {
    char name[64] = "?";
    pthread_getname_np(reinterpret_cast<pthread_t>(static_cast<uintptr_t>(key)), name, sizeof(name));
    return name;
}
void ReportRecordingConflict(const char* what, u64 other, const char* other_where,
                             const char* where) {
    if (recording_reports.fetch_add(1, std::memory_order_relaxed) >= 12) {
        return;
    }
    std::fprintf(stderr, "[recording-check] %s: '%s' (%s) while '%s' (%s)\n", what,
                 ThreadName(CurrentThreadKey()).c_str(), where, ThreadName(other).c_str(),
                 other_where ? other_where : "raw command buffer");
    void* frames[24];
    const int count = backtrace(frames, 24);
    backtrace_symbols_fd(frames, count, 2);
}
} // namespace

bool Scheduler::RecordingCheckEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_RECORDING_CHECK");
        return value && value[0] == '1';
    }();
    return enabled;
}

bool Scheduler::RecordingEnter(const char* where) {
    const u64 me = CurrentThreadKey();
    CheckRawOwner(where);
    u64 expected = 0;
    if (check_owner.compare_exchange_strong(expected, me, std::memory_order_acq_rel)) {
        check_where.store(where, std::memory_order_relaxed);
        return true;
    }
    if (expected != me) {
        ReportRecordingConflict("concurrent recording", expected,
                                check_where.load(std::memory_order_relaxed), where);
    }
    return false;
}

bool Scheduler::ProducerEnter(const char* where) {
    const u64 me = CurrentThreadKey();
    u64 expected = 0;
    if (check_producer.compare_exchange_strong(expected, me, std::memory_order_acq_rel)) {
        check_producer_where.store(where, std::memory_order_relaxed);
        return true;
    }
    if (expected != me) {
        ReportRecordingConflict("concurrent chunk producers", expected,
                                check_producer_where.load(std::memory_order_relaxed), where);
    }
    return false;
}

void Scheduler::ProducerExit(bool owned) {
    if (owned) {
        check_producer.store(0, std::memory_order_release);
    }
}

void Scheduler::CheckRawOwner(const char* where) {
    const u64 raw = check_raw_owner.load(std::memory_order_acquire);
    if (raw && raw != CurrentThreadKey()) {
        ReportRecordingConflict("used while another thread holds the raw command buffer", raw,
                                "raw command buffer", where);
    }
}

void Scheduler::ReleaseRawCommandBuffer(const char* where) {
    if (RecordingCheckEnabled()) [[unlikely]] {
        CheckRawOwner(where);
        check_raw_owner.store(0, std::memory_order_release);
    }
}

void Scheduler::RecordingExit(bool owned) {
    if (owned) {
        check_owner.store(0, std::memory_order_release);
    }
}

void Scheduler::NoteRawCommandBuffer() {
    CheckRawOwner("raw command buffer");
    check_raw_owner.store(CurrentThreadKey(), std::memory_order_release);
    // Each (thread, call site) taking the raw buffer, once, with its stack.
    static std::mutex mutex;
    static std::set<std::pair<std::string, void*>> seen;
    void* frames[16];
    const int count = backtrace(frames, 16);
    if (count < 3) {
        return;
    }
    const auto key = std::make_pair(ThreadName(CurrentThreadKey()), frames[2]);
    std::scoped_lock lk{mutex};
    if (seen.size() > 64 || !seen.insert(key).second) {
        return;
    }
    std::fprintf(stderr, "[recording-check] raw command buffer taken by '%s':\n",
                 key.first.c_str());
    backtrace_symbols_fd(frames, count, 2);
}

void Scheduler::LabelCommandBuffer(std::string label) {
    if (!PassLabelsEnabled()) {
        return;
    }
    if (!recording_threaded) {
        SetObjectName(instance.GetDevice(), current_cmdbuf, label);
        return;
    }
    Record([device = instance.GetDevice(), label = std::move(label)](vk::CommandBuffer cmdbuf) {
        SetObjectName(device, cmdbuf, label);
    });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        struct { const char* dli_fname; void* dli_fbase; } info{nullptr, nullptr};
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    Gow3Copy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    Record([signal = std::move(signal), done = deferred_signals_done](vk::CommandBuffer) mutable {
        Gow3Copy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    });
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        Gow3Toggle::Disabled(Gow3Toggle::OrderedGuestWrites)) {
        return;
    }
    Gow3Stats::WaitTimer timer{Gow3Stats::host_copies_wait_ns};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        Gow3Copy::WaitAsync();
        std::this_thread::yield();
    }
}

void Scheduler::WaitHostCopies() {
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        Gow3Stats::WaitTimer timer{Gow3Stats::host_copies_wait_ns};
        Gow3Stats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    Gow3Stats::WaitTimer timer{Gow3Stats::copy_threads_wait_ns};
    Gow3Copy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    const ProducerScope producer{this, "KickRecording"};
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    ReleaseRawCommandBuffer("kick");
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back(std::move(record_chunk));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording() {
    if (!recording_threaded) {
        return;
    }
    KickRecording(true);
    Gow3Stats::WaitTimer timer{Gow3Stats::sync_recording_ns};
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("gow3:VkRecorder");
    Gow3Threads::MakeCritical();
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            Gow3Threads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            Common::CpuRelax();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread.
        if (RecordingCheckEnabled()) [[unlikely]] {
            const bool owned = RecordingEnter("recorder chunk");
            chunk->Execute(current_cmdbuf);
            RecordingExit(owned);
        } else {
            chunk->Execute(current_cmdbuf);
        }
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish(std::source_location caller) {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    const bool diagnose = WaitDiagnostics::Enabled();
    const auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SubmitInfo info{};
    SubmitExecution(info);
    const u64 submit_ns = diagnose ? std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count() : 0;
    Wait(presubmit_tick, caller, submit_ns);
}

void Scheduler::WaitSubmitted(std::source_location caller) {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1, caller);
    }
}

void Scheduler::Wait(u64 tick, std::source_location caller, u64 submit_ns) {
    const bool diagnose = WaitDiagnostics::Enabled();
    auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    Gow3Stats::WaitTimer timer{Gow3Stats::tick_wait_ns};
    const auto submitted = diagnose ? std::chrono::steady_clock::now() : start;
    work_semaphore.Wait(tick);
    if (diagnose) {
        const auto completed = std::chrono::steady_clock::now();
        WaitDiagnostics::Get().Add(caller.file_name(), caller.line(),
            submit_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - start).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(completed - submitted).count());
    }
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // gow3: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !Gow3Toggle::Disabled(Gow3Toggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
    if (PassLabelsEnabled()) {
        // Pooled command buffers keep their last debug name: reset it so work without a
        // render pass is not attributed to an older one.
        SetObjectName(instance.GetDevice(), current_cmdbuf, std::string_view{"nopass"});
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
    const bool submit_owned = RecordingCheckEnabled() && RecordingEnter("submission");
    Check(current_cmdbuf.end());
    RecordingExit(submit_owned);

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    work_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
