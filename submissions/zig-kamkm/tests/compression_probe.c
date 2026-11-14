#define _POSIX_C_SOURCE 200809L
#include <igzip_lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(2);
    return t.tv_sec + t.tv_nsec / 1000000000.0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    rewind(f);
    if (length <= 0 || length > 256 * 1024) return 2;
    const unsigned sizes[] = {0, ISAL_DEF_LVL1_DEFAULT, ISAL_DEF_LVL2_DEFAULT, ISAL_DEF_LVL3_DEFAULT};
    unsigned scratch_size = 0;
    for (unsigned level = 1; level <= 3; level++)
        if (sizes[level] > scratch_size) scratch_size = sizes[level];
    unsigned char *input = malloc((size_t)length);
    unsigned char *output = malloc(256 * 1024);
    unsigned char *scratch = calloc(1, scratch_size);
    if (!input || !output || !scratch || fread(input, 1, (size_t)length, f) != (size_t)length) return 2;
    fclose(f);
    const int trials = 10000;
    puts("[");
    for (unsigned level = 0; level <= 3; level++) {
        unsigned used = 0;
        double start = now();
        for (int trial = 0; trial < trials; trial++) {
            struct isal_zstream stream;
            isal_deflate_stateless_init(&stream);
            stream.next_in = input;
            stream.avail_in = (unsigned)length;
            stream.next_out = output;
            stream.avail_out = 256 * 1024;
            stream.gzip_flag = IGZIP_GZIP;
            stream.level = level;
            if (level) {
                stream.level_buf = scratch;
                stream.level_buf_size = sizes[level];
            }
            int status = isal_deflate_stateless(&stream);
            if (status != COMP_OK) {
                fprintf(stderr, "ISA-L level %u failed with %d\n", level, status);
                return 1;
            }
            used = 256 * 1024 - stream.avail_out;
        }
        double elapsed = now() - start;
        char filename[1024];
        int written = snprintf(filename, sizeof(filename), "%s.level%u.gz", argv[1], level);
        if (written < 0 || (size_t)written >= sizeof(filename)) return 2;
        f = fopen(filename, "wb");
        if (!f || fwrite(output, 1, used, f) != used || fclose(f)) return 2;
        printf("{\"level\":%u,\"input_bytes\":%ld,\"output_bytes\":%u,\"us_per_compression\":%.6f}%s\n",
               level, length, used, elapsed * 1000000 / trials, level == 3 ? "" : ",");
    }
    puts("]");
    free(input);
    free(output);
    free(scratch);
    return 0;
}
