// Written for ocarina-of-time-recomp (MIT, Copyright (c) 2026 Jeff Aigner).
// Command-line driver of runtime/sm86_lower.c for tools/shader_census.py --lower: `sm86_lower --batch` reads lines
// `<key> <stage> <control file> <code file> <lowered code file> <lowered c1 file>` on stdin and answers one line
// each, `ok <key>` or `fail <key> <offset> <message>`; `sm86_lower <control> <code> <out code> <out c1>` does one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm86_lower.h"

static unsigned char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*size > 0 ? (size_t)*size : 1);
    if (b && fread(b, 1, (size_t)*size, f) != (size_t)*size) free(b), b = NULL;
    fclose(f);
    return b;
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static int one(const char *key, const char *ctl_path, const char *code_path, const char *out_code, const char *out_c1) {
    long ctl_size, code_size;
    unsigned char *ctl = read_file(ctl_path, &ctl_size), *code = read_file(code_path, &code_size);
    if (!ctl || !code) { printf("fail %s 0 cannot read the program files\n", key); free(ctl), free(code); return 0; }
    Sm86Lowered low;
    sm86_lower_tensor_cores = getenv("SM86_TENSOR") ? atoi(getenv("SM86_TENSOR")) : 0;   // HMMA as the MMA marker (1, 2: vk_caps.c)
    int ok = sm86_lower(code, (uint32_t)code_size, ctl, (uint32_t)ctl_size, &low);
    if (!ok) printf("fail %s %x %s\n", key, low.error_offset, low.error);
    else if (!write_file(out_code, low.code, low.code_size) || !write_file(out_c1, low.c1, low.c1_len))
        printf("fail %s 0 cannot write the lowered program\n", key), ok = 0;
    else printf("ok %s\n", key);
    sm86_lower_free(&low);
    free(ctl), free(code);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--batch")) {
        char line[4096];
        while (fgets(line, sizeof line, stdin)) {
            char key[128], stage[16], ctl[1024], code[1024], oc[1024], o1[1024];
            if (sscanf(line, "%127s %15s %1023s %1023s %1023s %1023s", key, stage, ctl, code, oc, o1) != 6) {
                printf("fail ? 0 malformed request\n");
                continue;
            }
            one(key, ctl, code, oc, o1);
            fflush(stdout);
        }
        return 0;
    }
    if (argc != 5) { fprintf(stderr, "usage: sm86_lower --batch | <control> <code> <out code> <out c1>\n"); return 2; }
    return one("program", argv[1], argv[2], argv[3], argv[4]) ? 0 : 1;
}
