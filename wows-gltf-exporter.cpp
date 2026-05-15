/* wows-gltf-exporter: stitch WoWS ship geometry parts into a single GLB. */
#include <argp.h>

#include "wows-model-exporter.h"
#define vlog(tag, fmt, ...) do { if (wows_stitch_verbose) fprintf(stderr, "[%s] " fmt, tag, ##__VA_ARGS__); } while (0)

/* ── argp ─────────────────────────────────────────────────────────── */

const char *argp_program_version = "wows-gltf-exporter " BFD_VERSION;
const char *argp_program_bug_address = "https://github.com/kakwa/wows-depack/issues";

static char doc[] = "\nStitch WoWS ship geometry parts into a single GLB.\n"
                    "\n"
                    "Examples:\n"
                    "  wows-gltf-exporter -W /path/to/World\\ of\\ Warships \\\n"
                    "    -s Kongo_1942 -o kongo.glb\n"
                    "  wows-gltf-exporter -W /path/to/World\\ of\\ Warships \\\n"
                    "    -s Kongo_1942 -H HullB -t -L 2 -o kongo_hullb.glb\n";

static struct argp_option options[] = {
    {"gameparams", 'g', "FILE", OPTION_HIDDEN, "GameParams.data"},
    {"wows-dir", 'W', "DIR", 0, "Root game directory"},
    {"ship", 's', "NAME", 0, "Ship name / pattern (words joined as .*word1.*word2.*, case-insensitive)"},
    {"output", 'o', "FILE", 0, "Output .glb file"},
    {"hull", 'H', "UPG", 0, "Hull upgrade name substring (default: latest)"},
    {"no-turrets", 't', nullptr, 0, "Exclude turret / mounted-component models (default: included)"},
    {"assets-bin", 'a', "FILE", OPTION_HIDDEN, "assets.bin"},
    {"no-textures", 'T', nullptr, 0, "Skip DDS texture application (default: applied)"},
    {"texture-size", 'Z', "N", 0, "Max texture dimension in pixels (default: 2048)"},
    {"lod", 'L', "N", 0, "LOD level to export (-1=auto, default: -1)"},
    {"damage", 'D', nullptr, 0, "Include damage/crack geometry (default: excluded)"},
    {"verbose", 'v', nullptr, 0, "Verbose progress output"},
    {0}};

struct Args {
    char *gameparams = nullptr;
    char *game_dir = nullptr;
    char *ship = nullptr;
    char *output = nullptr;
    char *hull = nullptr;
    char *assets_bin = nullptr;
    bool with_turrets = true;
    bool textures = true;
    int tex_size = 2048;
    int lod = -1;
    bool damage = false;
    bool verbose = false;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    Args *a = static_cast<Args *>(state->input);
    switch (key) {
    case 'g':
        a->gameparams = arg;
        break;
    case 'W':
        a->game_dir = arg;
        break;
    case 's':
        a->ship = arg;
        break;
    case 'o':
        a->output = arg;
        break;
    case 'H':
        a->hull = arg;
        break;
    case 'a':
        a->assets_bin = arg;
        break;
    case 't':
        a->with_turrets = false;
        break;
    case 'T':
        a->textures = false;
        break;
    case 'Z':
        a->tex_size = atoi(arg);
        break;
    case 'L':
        a->lod = atoi(arg);
        break;
    case 'D':
        a->damage = true;
        break;
    case 'v':
        a->verbose = true;
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc};

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    Args args;
    argp_parse(&argp, argc, argv, 0, nullptr, &args);
    wows_stitch_set_verbose(args.verbose);

    if (!args.game_dir || !args.ship || !args.output) {
        vlog("wows-gltf-exporter", "Error: -W, -s, and -o are required.\n");
        return 1;
    }

    wows_ship_export_options opts;
    if (args.gameparams)
        opts.gameparams_path = args.gameparams;
    if (args.assets_bin)
        opts.wows_assets_bin_path = args.assets_bin;
    if (args.hull)
        opts.hull_upgrade = args.hull;
    opts.with_turrets = args.with_turrets;
    opts.with_propellers = false; /* TODO: propeller placement not yet correct */
    opts.with_textures = args.textures;
    opts.max_tex_size = args.tex_size;
    opts.lod_level = args.lod;
    opts.exclude_damage = !args.damage;

    return wows_stitch_export_ship(args.game_dir, args.ship, args.output, opts) ? 0 : 1;
}
