/**
 * @file game_params.h
 * @brief GameParams.data loader for ship hull and mount information.
 *
 * `GameParams.data` is a Python-pickled binary file that contains the full
 * ship configuration tree for the game client.  This module uses the embedded
 * CPython interpreter to unpickle the file and extract the hull geometry and
 * mount point data for a requested ship.
 */

#pragma once
#include "wows-model-exporter.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief One ship entry returned by wows_list_ships().
 */
struct wows_ship_entry {
    std::string key;    /**< GameParams key, e.g. "PJSB007". */
    std::string index;  /**< Short code, e.g. "JSB007". */
    std::string nation; /**< Nation string, e.g. "Japan". */
    std::string type;   /**< Ship type string, e.g. "Battleship". */
};

/**
 * @brief Load hull and mount information for a ship from `GameParams.data`.
 *
 * The function unpickles @p gameparams_path via CPython and walks the ship
 * configuration tree to find the best matching hull for @p ship_name.
 *
 * @p hull_sel is matched as a substring against hull upgrade names
 * (e.g. `"HULL_A"`, `"HULL_B"`).  Pass `nullptr` to select the latest
 * (highest-tier) hull automatically.
 *
 * @param gameparams_path  Filesystem path to `GameParams.data`.
 * @param ship_name        Ship identifier as stored in the params (e.g. `"PJSB009"`).
 * @param hull_sel         Hull upgrade name substring, or `nullptr` for the latest hull.
 * @param out              Output parameter filled with hull model path and mount entries.
 * @return `true` on success; errors are printed to stderr.
 */
bool wows_load_hull_info(const char *gameparams_path, const char *ship_name, const char *hull_sel, wows_hull_info &out);

/**
 * @brief Same as ::wows_load_hull_info but reads pickled `GameParams.data` from a memory buffer.
 */
bool wows_load_hull_info_from_memory(const uint8_t *gameparams_data, size_t gameparams_size, const char *ship_name,
                                     const char *hull_sel, wows_hull_info &out);

/**
 * @brief Enumerate all ships in `GameParams.data`, grouped by nation and type.
 *
 * Each returned entry carries the GameParams key, short index code, nation, and
 * ship type as extracted from the `typeinfo` block inside each ship record.
 *
 * @param gameparams_path  Filesystem path to `GameParams.data`.
 * @param out              Output vector filled with one wows_ship_entry per ship.
 * @return `true` on success; errors are printed to stderr.
 */
bool wows_list_ships(const char *gameparams_path, std::vector<wows_ship_entry> &out);
