#define _POSIX_C_SOURCE 200809L

#include <argp.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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

const char *argp_program_version = BFD_VERSION;

const char *argp_program_bug_address = "https://github.com/kakwa/wows-depack/issues";

static char doc[] = "\nWorld of Warships .geometry debug tool\n"
                    "\n"
                    "Example:\n"
                    "  wows-geometry-cli -i path/to/hull.geometry -o hull.glb\n";

static struct argp_option options[] = {
    {"input", 'i', "INPUT_FILE", 0, "Input .geometry file"},
    {"print", 'p', NULL, 0, "Print parsed data"},
    {"verbose", 'v', NULL, 0, "Print all vertices (use with -p)"},
    {"output-glb", 'o', "OUTPUT_FILE", 0, "Export to binary glTF (.glb)"},
    {"sections", 's', "0,1,2,...", 0, "Comma-separated vertex section indices to export (default: all)"},
    {0}};

static char args_doc[] = "-i INPUT_FILE [-p] [-o OUTPUT_FILE] [-s 0,1,...]";

struct arguments {
    char *input;
    char *glb_output;
    char *sections_str;
    bool print;
    bool verbose;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    struct arguments *arguments = (struct arguments *)state->input;
    switch (key) {
    case 'i':
        arguments->input = arg;
        break;
    case 'p':
        arguments->print = true;
        break;
    case 'v':
        arguments->verbose = true;
        break;
    case 'o':
        arguments->glb_output = arg;
        break;
    case 's':
        arguments->sections_str = arg;
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

    if (!args->input) {
        fprintf(stderr, "Error: -i INPUT_FILE is required\n");
        free(args);
        return 1;
    }

    wows_geometry *content = NULL;
    ret = wows_parse_geometry(args->input, &content);
    if (ret != 0) {
        fprintf(stderr, "Error: failed to parse %s (code %d)\n", args->input, ret);
        free(args);
        return ret;
    }

    if (args->print) {
        ret = wows_geometry_print(content, args->verbose);
    }

    if (args->glb_output) {
        uint32_t *sections = NULL;
        uint32_t n_sections = 0;

        if (args->sections_str) {
            /* count commas to size the array */
            size_t cap = 1;
            for (const char *p = args->sections_str; *p; p++)
                if (*p == ',')
                    cap++;
            sections = malloc(cap * sizeof(uint32_t));
            if (!sections) {
                ret = 1;
                goto cleanup;
            }
            char *copy = strdup(args->sections_str);
            char *tok = strtok(copy, ",");
            while (tok) {
                sections[n_sections++] = (uint32_t)strtoul(tok, NULL, 10);
                tok = strtok(NULL, ",");
            }
            free(copy);
        }

        ret = wows_geometry_to_glb_sections(content, args->glb_output, sections, n_sections);
        free(sections);
        if (ret != 0)
            fprintf(stderr, "Error: failed to write GLB to %s (code %d)\n", args->glb_output, ret);
        else
            printf("Wrote %s\n", args->glb_output);
    }

cleanup:
    if (content != NULL)
        wows_geometry_free(content);
    free(args);
    return ret;
}
