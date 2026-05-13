#pragma once
#include "stitch.h"

/* Load hull info for ship_name from GameParams.data.
 * hull_sel: upgrade name substring, or nullptr for the latest hull. */
bool load_hull_info(const char *gameparams_path,
                    const char *ship_name,
                    const char *hull_sel,
                    HullInfo &out);
