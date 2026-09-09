/*
 * driver.c — run a libFuzzer harness over every file in the given paths.
 *
 * Lets the seed corpus be replayed under gcc, ASan or plain, on machines and
 * CI jobs that do not have clang. Not a fuzzer: it only replays.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    static uint8_t buf[1 << 16];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    LLVMFuzzerTestOneInput(buf, n);
    return 0;
}

static int run_path(const char *path, int *count)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "cannot stat %s\n", path);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        (*count)++;
        return run_file(path);
    }
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        (*count)++;
        if (run_file(full) != 0)
            rc = -1;
    }
    closedir(d);
    return rc;
}

int main(int argc, char **argv)
{
    int count = 0, rc = 0;
    for (int i = 1; i < argc; i++)
        if (run_path(argv[i], &count) != 0)
            rc = 1;
    printf("  replayed %d corpus files%s\n", count, rc ? " (with errors)" : "");
    return rc;
}
