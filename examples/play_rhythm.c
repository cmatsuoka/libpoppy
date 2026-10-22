#define _POSIX_C_SOURCE 199309L

#include <AL/al.h>
#include <AL/alc.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mp7.h"

#define BUFFER_COUNT 4
#define BUFFER_FRAMES 4096
#define SAMPLE_RATE MP7_DEFAULT_SAMPLE_RATE

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signum)
{
    (void)signum;
    keep_running = 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--tempo BPM] [--bank A|B] [--buttons=N[,N...]]\n"
            "\n"
            "  --tempo BPM         tempo in BPM, 1..240 (default 120)\n"
            "  --bank A|B          rhythm bank (default A)\n"
            "  --buttons=N[,N...]  rhythm buttons, 1..10 (default none)\n",
            program);
}

static int parse_number(const char *text, long minimum, long maximum, long *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if ((errno != 0) || (end == text) || (*end != '\0') ||
        (value < minimum) || (value > maximum)) {
        return 0;
    }

    *out = value;
    return 1;
}

static int parse_buttons(const char *text, uint16_t *out)
{
    const char *cursor = text;
    uint16_t mask = 0;

    if (*cursor == '\0') {
        return 0;
    }

    for (;;) {
        char *end = NULL;
        long button;

        errno = 0;
        button = strtol(cursor, &end, 10);
        if ((errno != 0) || (end == cursor) || (button < 1) || (button > 10)) {
            return 0;
        }

        mask = (uint16_t)(mask | (uint16_t)(1u << (unsigned int)(button - 1)));

        if (*end == '\0') {
            break;
        }
        if (*end != ',') {
            return 0;
        }
        cursor = end + 1;
        if (*cursor == '\0') {
            return 0;
        }
    }

    *out = mask;
    return 1;
}

static int parse_bank(const char *text, mp7_bank *out)
{
    if (((text[0] == 'A') || (text[0] == 'a')) && (text[1] == '\0')) {
        *out = MP7_BANK_A;
        return 1;
    }
    if (((text[0] == 'B') || (text[0] == 'b')) && (text[1] == '\0')) {
        *out = MP7_BANK_B;
        return 1;
    }
    return 0;
}

static int option_value(const char *arg, const char *name, int argc,
                        char **argv, int *index, const char **value)
{
    size_t length = strlen(name);

    if ((strncmp(arg, name, length) != 0) ||
        ((arg[length] != '\0') && (arg[length] != '='))) {
        return 0;
    }
    if (arg[length] == '=') {
        *value = arg + length + 1;
        return 1;
    }
    if ((*index + 1) >= argc) {
        return -1;
    }

    *index += 1;
    *value = argv[*index];
    return 1;
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (milliseconds % 1000) * 1000000L;
    nanosleep(&delay, NULL);
}

static void clear_errors(void)
{
    while (alGetError() != AL_NO_ERROR) {
    }
}

static int fill_buffer(mp7_context ctx, ALuint buffer, int16_t *pcm)
{
    clear_errors();
    mp7_process(ctx, pcm, BUFFER_FRAMES);
    alBufferData(buffer, AL_FORMAT_STEREO16, pcm,
                 (ALsizei)(BUFFER_FRAMES * 2u * sizeof(int16_t)),
                 (ALsizei)SAMPLE_RATE);

    return alGetError() == AL_NO_ERROR;
}

static int play(mp7_context ctx)
{
    ALCdevice *device;
    ALCcontext *alc_context;
    ALuint buffers[BUFFER_COUNT] = {0};
    ALuint source = 0;
    ALsizei buffers_generated = 0;
    ALsizei i;
    int16_t pcm[BUFFER_FRAMES * 2u];
    int status = 1;

    device = alcOpenDevice(NULL);
    if (device == NULL) {
        fprintf(stderr, "play: cannot open OpenAL device\n");
        return 1;
    }

    alc_context = alcCreateContext(device, NULL);
    if ((alc_context == NULL) ||
        (alcMakeContextCurrent(alc_context) == ALC_FALSE)) {
        fprintf(stderr, "play: cannot create OpenAL context\n");
        if (alc_context != NULL) {
            alcDestroyContext(alc_context);
        }
        alcCloseDevice(device);
        return 1;
    }

    clear_errors();
    alGenBuffers(BUFFER_COUNT, buffers);
    if (alGetError() != AL_NO_ERROR) {
        fprintf(stderr, "play: cannot create OpenAL buffers\n");
        goto cleanup;
    }
    buffers_generated = BUFFER_COUNT;

    clear_errors();
    alGenSources(1, &source);
    if (alGetError() != AL_NO_ERROR) {
        fprintf(stderr, "play: cannot create OpenAL source\n");
        goto cleanup;
    }

    for (i = 0; i < BUFFER_COUNT; i++) {
        if (!fill_buffer(ctx, buffers[i], pcm)) {
            fprintf(stderr, "play: cannot fill OpenAL buffer\n");
            goto cleanup;
        }
        clear_errors();
        alSourceQueueBuffers(source, 1, &buffers[i]);
        if (alGetError() != AL_NO_ERROR) {
            fprintf(stderr, "play: cannot queue OpenAL buffer\n");
            goto cleanup;
        }
    }

    clear_errors();
    alSourcePlay(source);
    if (alGetError() != AL_NO_ERROR) {
        fprintf(stderr, "play: cannot start OpenAL source\n");
        goto cleanup;
    }

    while (keep_running != 0) {
        ALint processed = 0;

        alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
        while ((processed > 0) && (keep_running != 0)) {
            ALuint buffer;

            clear_errors();
            alSourceUnqueueBuffers(source, 1, &buffer);
            if (alGetError() != AL_NO_ERROR) {
                break;
            }
            if (!fill_buffer(ctx, buffer, pcm)) {
                fprintf(stderr, "play: cannot fill OpenAL buffer\n");
                goto cleanup;
            }
            clear_errors();
            alSourceQueueBuffers(source, 1, &buffer);
            if (alGetError() != AL_NO_ERROR) {
                fprintf(stderr, "play: cannot queue OpenAL buffer\n");
                goto cleanup;
            }
            processed -= 1;
        }

        {
            ALint state = 0;
            ALint queued = 0;

            alGetSourcei(source, AL_SOURCE_STATE, &state);
            alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
            if ((state != AL_PLAYING) && (queued > 0) &&
                (keep_running != 0)) {
                alSourcePlay(source);
            }
        }

        sleep_ms(20);
    }

    status = 0;

cleanup:
    if (source != 0) {
        alSourceStop(source);
        alDeleteSources(1, &source);
    }
    if (buffers_generated > 0) {
        alDeleteBuffers(buffers_generated, buffers);
    }
    alcMakeContextCurrent(NULL);
    alcDestroyContext(alc_context);
    alcCloseDevice(device);

    return status;
}

int main(int argc, char **argv)
{
    long tempo = 120;
    mp7_bank bank = MP7_BANK_A;
    uint16_t buttons = 0;
    mp7_context ctx;
    int i;
    int status;

    for (i = 1; i < argc; i++) {
        const char *value = NULL;
        int matched;

        if ((strcmp(argv[i], "--help") == 0) ||
            (strcmp(argv[i], "-h") == 0)) {
            usage(argv[0]);
            return 0;
        }

        matched = option_value(argv[i], "--tempo", argc, argv, &i, &value);
        if (matched != 0) {
            if ((matched < 0) || !parse_number(value, 1, 240, &tempo)) {
                fprintf(stderr, "%s: invalid --tempo value\n", argv[0]);
                return 2;
            }
            continue;
        }

        matched = option_value(argv[i], "--bank", argc, argv, &i, &value);
        if (matched != 0) {
            if ((matched < 0) || !parse_bank(value, &bank)) {
                fprintf(stderr, "%s: invalid --bank value\n", argv[0]);
                return 2;
            }
            continue;
        }

        matched = option_value(argv[i], "--buttons", argc, argv, &i, &value);
        if (matched != 0) {
            if ((matched < 0) || !parse_buttons(value, &buttons)) {
                fprintf(stderr, "%s: invalid --buttons value\n", argv[0]);
                return 2;
            }
            continue;
        }

        fprintf(stderr, "%s: unknown argument '%s'\n", argv[0], argv[i]);
        usage(argv[0]);
        return 2;
    }

    ctx = mp7_create_context(SAMPLE_RATE);
    if (ctx == NULL) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    mp7_set_tempo(ctx, (uint16_t)tempo);
    mp7_select_rhythm(ctx, bank, buttons);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    status = play(ctx);

    mp7_destroy_context(ctx);
    return status;
}
