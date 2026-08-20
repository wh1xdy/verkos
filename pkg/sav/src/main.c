/* savc — the sav compiler.
 *
 *   savc program.sav -o program
 *
 * One file in, one executable out, and nothing in between: no preprocessor,
 * no assembler, no linker, no libc. What the binary contains is what this
 * compiler decided to put in it, which is the only reason the manifest in it
 * can be believed.
 */
#include "sav.h"

int opt_contracts = 1;
int opt_verbose = 0;

static void usage(void) {
    fprintf(stderr,
        "usage: savc FILE.sav... [-o OUT] [--no-contracts] [--manifest] [-v]\n"
        "\n"
        "  -o OUT          where to write the executable (default a.out)\n"
        "  --no-contracts  do not compile requires/ensures into checks. The\n"
        "                  refusal of a vacuous claim is not optional; only the\n"
        "                  runtime check is.\n"
        "  --manifest      print what the program may ask of the kernel\n"
        "  -v              say what was produced\n"
        "  --omit-call N   leave call N out of the manifest while still\n"
        "                  generating it. This exists for one purpose: to build\n"
        "                  a binary whose note is a lie, so that a kernel's\n"
        "                  refusal can be watched happening. Never use it for a\n"
        "                  program meant to run.\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *in[16]; int nin = 0;
    const char *out = "a.out";
    int show_manifest = 0;
    long omit_call = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--no-contracts") == 0) opt_contracts = 0;
        else if (strcmp(argv[i], "--manifest") == 0) show_manifest = 1;
        else if (strcmp(argv[i], "-v") == 0) opt_verbose = 1;
        else if (strcmp(argv[i], "--omit-call") == 0 && i + 1 < argc) omit_call = atol(argv[++i]);
        else if (argv[i][0] == '-') usage();
        else if (nin < 16) in[nin++] = argv[i];
        else { fprintf(stderr, "savc: too many files\n"); exit(2); }
    }
    if (!nin) usage();

    /* Several files on one command line, and `use` inside them: either way
     * every file is parsed once and they share one set of names. */
    Unit *u = parse_unit(in[0]);
    for (int i = 1; i < nin; i++) parse_into(u, in[i]);
    check_unit(u);

    Image img;
    memset(&img, 0, sizeof img);
    gen_unit(u, &img);

    if (omit_call >= 0) {
        int kept = 0;
        for (int i = 0; i < img.nsyscalls; i++)
            if (img.syscalls[i] != (u32)omit_call) img.syscalls[kept++] = img.syscalls[i];
        fprintf(stderr,
                "savc: %s claims %d call%s and makes %d: call %ld was left out of the "
                "manifest on purpose.\n", out, kept, kept == 1 ? "" : "s",
                img.nsyscalls, omit_call);
        img.nsyscalls = kept;
    }

    write_elf(&img, out);

    if (show_manifest || opt_verbose) {
        printf("%s: %zu bytes of code and read-only data, %zu bytes of zeroed memory\n",
               out, img.text.n, img.bss_size);
        printf("  capabilities:");
        if (!img.ncapnames) printf(" none");
        for (int i = 0; i < img.ncapnames; i++) printf(" %s", img.capnames[i]);
        printf("\n  system calls it may make:");
        for (int i = 0; i < img.nsyscalls; i++) printf(" %u", img.syscalls[i]);
        printf("\n  relocations: 0\n");
    }
    return 0;
}
