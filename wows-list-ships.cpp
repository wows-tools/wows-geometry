/* wows-list-ships: enumerate all ships in GameParams.data */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <argp.h>

extern "C" {
#include "wows-depack.h"
}
#include "wows-game-params.h"
#include "wows-model-exporter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

const char *argp_program_version     = "wows-list-ships " BFD_VERSION;
const char *argp_program_bug_address = "https://github.com/kakwa/wows-depack/issues";

static char doc[] =
    "\nList all ships available in GameParams.data.\n"
    "\n"
    "Examples:\n"
    "  wows-list-ships -W /path/to/World\\ of\\ Warships\n"
    "  wows-list-ships -W /path/to/World\\ of\\ Warships -n japan -t Battleship\n";

static struct argp_option options[] = {
    {"gameparams", 'g', "FILE", OPTION_HIDDEN, "GameParams.data"},
    {"wows-dir",   'W', "DIR",  0, "Root game directory"},
    {"nation",     'n', "STR",  0, "Filter by nation (substring, case-insensitive)"},
    {"type",       't', "STR",  0, "Filter by ship type (substring, case-insensitive)"},
    {"tier",       'r', "INT",  0, "Filter by tier (exact match)"},
    {0}};

struct Args {
    char *gameparams = nullptr;
    char *game_dir   = nullptr;
    char *nation     = nullptr;
    char *type       = nullptr;
    int   tier       = -1;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    Args *a = static_cast<Args *>(state->input);
    switch (key) {
    case 'g': a->gameparams = arg; break;
    case 'W': a->game_dir   = arg; break;
    case 'n': a->nation     = arg; break;
    case 't': a->type       = arg; break;
    case 'r': a->tier       = atoi(arg); break;
    default:  return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc};

static WOWS_CONTEXT *open_depack(const char *game_dir) {
    char *idx_dir = nullptr;
    if (wows_get_latest_idx_dir(const_cast<char *>(game_dir), &idx_dir) != 0 || !idx_dir)
        return nullptr;
    WOWS_CONTEXT *ctx = wows_init_context(WOWS_NO_DEBUG);
    if (!ctx) { free(idx_dir); return nullptr; }
    int ret = wows_parse_index_dir(idx_dir, ctx);
    free(idx_dir);
    if (ret != 0) { wows_free_context(ctx); return nullptr; }
    return ctx;
}

static std::vector<uint8_t> depack_search_read(WOWS_CONTEXT *ctx, const char *pattern) {
    int count = 0;
    char **results = nullptr;
    if (wows_search(ctx, const_cast<char *>(pattern), WOWS_SEARCH_FULL_PATH, &count, &results) != 0 || count == 0) {
        if (results) {
            for (int i = 0; i < count; i++) free(results[i]);
            free(results);
        }
        return {};
    }
    std::string path(results[0]);
    for (int i = 0; i < count; i++) free(results[i]);
    free(results);

    char *buf = nullptr;
    size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (!fp) return {};
    int ret = wows_extract_file_fp(ctx, const_cast<char *>(path.c_str()), fp);
    fclose(fp);
    if (ret != 0) { free(buf); return {}; }
    std::vector<uint8_t> data(buf, buf + sz);
    free(buf);
    return data;
}

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
        fprintf(stderr, "Error: either -W or -g is required.\n");
        return 1;
    }

    std::string gameparams_path;
    std::vector<uint8_t> gameparams_mem;

    if (args.gameparams) {
        gameparams_path = args.gameparams;
    } else {
        gameparams_path = wows_stitch_find_game_file(args.game_dir, "GameParams.data");
        if (gameparams_path.empty()) {
            WOWS_CONTEXT *ctx = open_depack(args.game_dir);
            if (ctx) {
                gameparams_mem = depack_search_read(ctx, ".*GameParams\\.data");
                wows_free_context(ctx);
            }
            if (gameparams_mem.empty()) {
                fprintf(stderr, "GameParams.data not found under %s (filesystem or archive)\n", args.game_dir);
                return 1;
            }
        }
    }

    Py_Initialize();
    std::vector<wows_ship_entry> ships;
    bool ok;
    if (!gameparams_mem.empty())
        ok = wows_list_ships_from_memory(gameparams_mem.data(), gameparams_mem.size(), ships);
    else
        ok = wows_list_ships(gameparams_path.c_str(), ships);
    Py_Finalize();

    if (!ok)
        return 1;

    std::string nation_filter = args.nation ? str_lower(args.nation) : "";
    std::string type_filter   = args.type   ? str_lower(args.type)   : "";

    std::sort(ships.begin(), ships.end(), [](const wows_ship_entry &a, const wows_ship_entry &b) {
        if (a.nation != b.nation) return a.nation < b.nation;
        if (a.type   != b.type)   return a.type   < b.type;
        if (a.tier   != b.tier)   return a.tier   < b.tier;
        return a.key < b.key;
    });

    int tier_filter = args.tier;

    printf("%-20s %-12s %-20s %-20s %-5s\n", "Key", "Index", "Nation", "Type", "Tier");
    printf("%-20s %-12s %-20s %-20s %-5s\n",
           "--------------------", "------------",
           "--------------------", "--------------------", "-----");

    for (const auto &s : ships) {
        if (!nation_filter.empty() &&
            str_lower(s.nation).find(nation_filter) == std::string::npos)
            continue;
        if (!type_filter.empty() &&
            str_lower(s.type).find(type_filter) == std::string::npos)
            continue;
        if (tier_filter >= 0 && s.tier != tier_filter)
            continue;
        char tier_str[8];
        if (s.tier > 0)
            snprintf(tier_str, sizeof(tier_str), "%d", s.tier);
        else
            snprintf(tier_str, sizeof(tier_str), "?");
        printf("%-20s %-12s %-20s %-20s %-5s\n",
               s.key.c_str(), s.index.c_str(),
               s.nation.c_str(), s.type.c_str(), tier_str);
    }

    return 0;
}
