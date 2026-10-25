/* Deterministic regression coverage for ATRAC9 instance reuse. The decoder is
 * stubbed so this exercises AJM control ordering without game audio or assets. */
#undef NDEBUG
#include "../runtime/modules/libSceAjm/ps4_ajm.c"
#include <assert.h>

void *Atrac9GetHandle(void) { return malloc(1); }
void Atrac9ReleaseHandle(void *handle) { free(handle); }
int Atrac9InitDecoder(void *handle, unsigned char *config) {
    (void)handle;
    (void)config;
    return 0;
}
int Atrac9GetCodecInfo(void *handle, Atrac9CodecInfo *info) {
    (void)handle;
    *info = (Atrac9CodecInfo){
        .channels = 2,
        .samplingRate = 48000,
        .superframeSize = 8,
        .framesInSuperframe = 2,
        .frameSamples = 4,
    };
    return 0;
}
int Atrac9Decode(void *handle, const unsigned char *input, int input_size,
                 short *pcm, int *bytes_used, int nointerleave) {
    static int frame_number;
    (void)handle;
    (void)input;
    (void)nointerleave;
    if (input_size < 4) return 1;
    for (int i = 0; i < 8; ++i) pcm[i] = (short)(frame_number * 16 + i);
    ++frame_number;
    *bytes_used = 4;
    return 0;
}
int Atrac9DecodeS32(void *handle, const unsigned char *input, int input_size,
                    int *pcm, int *bytes_used, int nointerleave) {
    (void)handle; (void)input; (void)input_size; (void)pcm;
    (void)bytes_used; (void)nointerleave;
    return 1;
}
int Atrac9DecodeF32(void *handle, const unsigned char *input, int input_size,
                    float *pcm, int *bytes_used, int nointerleave) {
    (void)handle; (void)input; (void)input_size; (void)pcm;
    (void)bytes_used; (void)nointerleave;
    return 1;
}

typedef struct {
    OrbisAjmSidebandResult result;
    OrbisAjmSidebandStream stream;
    OrbisAjmSidebandGaplessDecode gapless;
} TestSideband;

static TestSideband decode(AjmInstance *instance, uint32_t instance_id,
                           const OrbisAjmSidebandGaplessDecode *gapless,
                           int multiple_frames, uint64_t output_size) {
    static const uint8_t encoded_superframe[8] = {0};
    static const uint8_t config[8] = {0xFE, 0, 0, 0};
    uint8_t control[16] = {0};
    size_t control_size = 0;
    uint32_t sideband_flags = AJM_SIDEBAND_STREAM;
    if (gapless) {
        memcpy(control, gapless, sizeof(*gapless));
        control_size += sizeof(*gapless);
        sideband_flags |= AJM_SIDEBAND_GAPLESS;
    }
    memcpy(control + control_size, config, sizeof(config));
    control_size += sizeof(config);

    uint8_t pcm[64] = {0};
    TestSideband sideband = {0};
    AjmParsedJob job = {0};
    job.instance_id = instance_id;
    job.flags = ((uint64_t)sideband_flags << 45) |
                ((uint64_t)AJM_CONTROL_INITIALIZE << 13) |
                ((uint64_t)(multiple_frames ? AJM_RUN_MULTIPLE_FRAMES : 0) << 11);
    job.input_buf = (void *)encoded_superframe;
    job.input_size = sizeof(encoded_superframe);
    job.num_inputs = 1;
    job.input_bufs[0] = (void *)encoded_superframe;
    job.input_sizes[0] = sizeof(encoded_superframe);
    job.num_outputs = 1;
    job.output_bufs[0] = pcm;
    job.output_sizes[0] = output_size;
    job.control_in_buf = control;
    job.control_in_size = control_size;
    job.control_out_buf = &sideband;
    job.control_out_size = sizeof(sideband);

    process_single_job(&job);
    assert(instance->is_initialized);
    return sideband;
}

int main(void) {
    const uint32_t instance_id = 1;
    AjmInstance *instance = &g_ajm_instances[instance_id];
    memset(instance, 0, sizeof(*instance));
    assert(pthread_mutex_init(&instance->mutex, NULL) == 0);
    instance->in_use = true;
    instance->id = instance_id;
    instance->codec_type = AJM_CODEC_AT9_DEC;
    instance->format = AJM_FORMAT_S16;

    const OrbisAjmSidebandGaplessDecode first_stream = {8, 0, 0};
    TestSideband first = decode(instance, instance_id, &first_stream, 0, 64);
    assert(first.result.result == 0);
    assert(first.stream.output_written == 16);
    assert(first.stream.total_decoded_samples == 4);
    assert(instance->gapless.current.total_samples == 4);

    /* A second short stream must not inherit the first stream's four processed
     * samples. On the old ordering, the new 4-sample metadata was applied to
     * stale counters and the stream appeared finished before decoding. */
    const OrbisAjmSidebandGaplessDecode second_stream = {4, 0, 0};
    TestSideband second = decode(instance, instance_id, &second_stream, 1, 64);
    assert(second.result.result == 0);
    assert(second.stream.output_written == 16);
    assert(second.stream.total_decoded_samples == 4);
    assert(instance->gapless.current.total_samples == 0);

    /* Reinitialize without gapless metadata: state from the previous stream
     * must not limit the next output. */
    TestSideband no_gapless = decode(instance, instance_id, NULL, 1, 64);
    assert(no_gapless.result.result == 0);
    assert(no_gapless.stream.output_written == 32);
    assert(no_gapless.stream.total_decoded_samples == 8);

    /* An explicit zero-length gapless record also clears the stream limit. */
    const OrbisAjmSidebandGaplessDecode zero_stream = {0, 0, 0};
    TestSideband zero = decode(instance, instance_id, &zero_stream, 1, 64);
    assert(zero.result.result == 0);
    assert(zero.stream.output_written == 32);
    assert(zero.stream.total_decoded_samples == 8);

    Atrac9ReleaseHandle(instance->at9_handle);
    free(instance->pcm_buffer);
    pthread_mutex_destroy(&instance->mutex);
    memset(instance, 0, sizeof(*instance));
    puts("AJM reinitialize test: ok");
    return 0;
}
