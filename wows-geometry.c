#define _POSIX_C_SOURCE 200809L

#include <argp.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdbool.h>
#include "inc/wows-geometry.h"

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#define SET_BINARY_MODE(file)
#endif

#define CHUNK 16384

const char *argp_program_version = BFD_VERSION;

const char *argp_program_bug_address = "https://github.com/kakwa/wows-depack/issues";

static char doc[] = "\nWorld of Warships .geometry debug tool";

static struct argp_option options[] = {
    {"input", 'i', "INPUT_SPLASH", 0, "Input geometry file"}, {"print", 'p', NULL, 0, "Print All files"}, {0}};

/* A description of the arguments we accept. */
static char args_doc[] = "<-i INPUT_FILE | -I INPUT_DIR | -W WOWS_BASE_DIR>";

struct arguments {
    char *args[2]; /* arg1 & arg2 */
    char *input;
    bool print;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    /* Get the input argument from argp_parse, which we
       know is a pointer to our arguments structure. */
    struct arguments *arguments = (struct arguments *)state->input;

    switch (key) {
    case 'i':
        arguments->input = arg;
        break;
    case 'p':
        arguments->print = true;
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc};

int main(int argc, char **argv) {
    struct arguments *args = calloc(sizeof(struct arguments), 1);
    int ret = 0;
    argp_parse(&argp, argc, argv, 0, 0, args);

    wows_geometry *content;
    if (args->input != NULL) {
        ret = wows_parse_geometry(args->input, &content);
    }
    if (args->print) {
        wows_geometry_print(content);
    }
    wows_geometry_free(content);
    free(args);
    return ret;
}
