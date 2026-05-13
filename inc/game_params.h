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
#include "stitch.h"

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
bool load_hull_info(const char *gameparams_path, const char *ship_name, const char *hull_sel, HullInfo &out);
