/*
 * hal_adc.c (POSIX) — replays raw little-endian int32 samples from a file.
 *
 * The file is the "ADC". Pacing is the application's job: this just hands
 * over as many samples as asked for, so the same file can be replayed in
 * real time or fifty times faster in an integration test.
 */

#include "hal/hal_adc.h"

#include <stdio.h>

static FILE *f;
static int   looping;

int hal_adc_init(const char *source, int loop)
{
    if (source == NULL)
        return -1;
    f = fopen(source, "rb");
    if (f == NULL)
        return -1;
    looping = loop;
    return 0;
}

int hal_adc_read(int32_t *out, size_t max)
{
    if (f == NULL || out == NULL)
        return -1;

    size_t got = 0;
    while (got < max) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) {
            if (!looping || got > 0)
                break;
            if (fseek(f, 0, SEEK_SET) != 0)
                break;
            continue;
        }
        /* Little-endian on disk: the natural output of struct.pack('<i'). */
        out[got++] = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                               ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
    }
    if (got == 0 && !looping)
        return -1;
    return (int)got;
}

void hal_adc_close(void)
{
    if (f != NULL)
        fclose(f);
    f = NULL;
}
