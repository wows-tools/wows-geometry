/* wows-list-ships: enumerate all ships in GameParams.data */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <argp.h>

#include "wows-game-params.h"
#include "wows-model-exporter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

const char *argp_program_version     = "wows-list-ships " BFD_VERSION;
const char *argp_program_bug_address = "https://github.com/kakwa/wows-depack/issues";

static char doc[] =
    "\nList all ships available in GameParams.data.\n"
    "\n"
    "GameParams.data is auto-detected from -d if not given\n"
    "(recursive scan up to 3 directory levels; newest version wins).";

static struct argp_option options[] = {
    {"gameparams", 'g', "FILE", 0, "GameParams.data (auto-detected from -d if omitted)"},
    {"game-dir",   'd', "DIR",  0, "Root game directory"},
    {"nation",     'n', "STR",  0, "Filter by nation (substring, case-insensitive)"},
    {"type",       't', "STR",  0, "Filter by ship type (substring, case-insensitive)"},
    {0}};

struct Args {
    char *gameparams = nullptr;
    char *game_dir   = nullptr;
    char *nation     = nullptr;
    char *type       = nullptr;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    Args *a = static_cast<Args *>(state->input);
    switch (key) {
    case 'g': a->gameparams = arg; break;
    case 'd': a->game_dir   = arg; break;
    case 'n': a->nation     = arg; break;
    case 't': a->type       = arg; break;
    default:  return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc};

static std::string str_lower(const std::string &s) {
    std::string r = s;
    for (auto &c : r)
        c = (char)tolower((unsigned char)c);
    return r;
}

int main(int argc, char **argv) {
    Args args;
    argp_parse(&argp, argc, argv, 0, nullptr, &args);

    if (!args.gameparams && !args.game_dir) {
        fprintf(stderr, "Error: either -d or -g is required.\n");
        return 1;
    }

    std::string gameparams_path;
    if (args.gameparams) {
        gameparams_path = args.gameparams;
    } else {
        gameparams_path = wows_stitch_find_game_file(args.game_dir, "GameParams.data");
        if (gameparams_path.empty()) {
            fprintf(stderr, "GameParams.data not found under %s\n", args.game_dir);
            return 1;
        }
    }

    Py_Initialize();
    std::vector<wows_ship_entry> ships;
    bool ok = wows_list_ships(gameparams_path.c_str(), ships);
    Py_Finalize();

    if (!ok)
        return 1;

    std::string nation_filter = args.nation ? str_lower(args.nation) : "";
    std::string type_filter   = args.type   ? str_lower(args.type)   : "";

    std::sort(ships.begin(), ships.end(), [](const wows_ship_entry &a, const wows_ship_entry &b) {
        if (a.nation != b.nation) return a.nation < b.nation;
        if (a.type   != b.type)   return a.type   < b.type;
        return a.key < b.key;
    });

    printf("%-20s %-12s %-20s %-20s\n", "Key", "Index", "Nation", "Type");
    printf("%-20s %-12s %-20s %-20s\n",
           "--------------------", "------------",
           "--------------------", "--------------------");

    for (const auto &s : ships) {
        if (!nation_filter.empty() &&
            str_lower(s.nation).find(nation_filter) == std::string::npos)
            continue;
        if (!type_filter.empty() &&
            str_lower(s.type).find(type_filter) == std::string::npos)
            continue;
        printf("%-20s %-12s %-20s %-20s\n",
               s.key.c_str(), s.index.c_str(),
               s.nation.c_str(), s.type.c_str());
    }

    return 0;
}
