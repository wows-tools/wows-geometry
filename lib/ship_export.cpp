#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <tiny_gltf.h>

extern "C" {
#include "assets_bin.h"
}
#include "stitch.h"
#include "game_params.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

bool stitch_export_ship(const std::string &game_dir,
                        const std::string &ship_name,
                        const std::string &output_path,
                        const ShipExportOptions &opts) {

    std::string norm_dir = stitch_normalize_slashes(game_dir);

    std::string gameparams_path = opts.gameparams_path;
    if (gameparams_path.empty()) {
        gameparams_path = stitch_find_game_file(norm_dir, "GameParams.data");
        if (gameparams_path.empty()) {
            fprintf(stderr,
                    "Error: GameParams.data not found under %s.\n"
                    "       Supply opts.gameparams_path to specify it explicitly.\n",
                    norm_dir.c_str());
            return false;
        }
        stitch_vlog("Auto-detected GameParams: %s\n", gameparams_path.c_str());
    }

    std::string assets_bin_path = opts.assets_bin_path;
    if (assets_bin_path.empty()) {
        assets_bin_path = stitch_find_game_file(norm_dir, "assets.bin");
        if (!assets_bin_path.empty())
            stitch_vlog("Auto-detected assets.bin: %s\n", assets_bin_path.c_str());
    }

    stitch_vlog("Loading GameParams …\n");
    Py_Initialize();

    HullInfo hull;
    const char *hull_sel = opts.hull_upgrade.empty() ? nullptr
                                                      : opts.hull_upgrade.c_str();
    if (!load_hull_info(gameparams_path.c_str(), ship_name.c_str(), hull_sel, hull)) {
        Py_Finalize();
        return false;
    }
    Py_Finalize();

    stitch_vlog("Hull model: %s\n", hull.hull_model.c_str());
    stitch_vlog("Mounts:     %zu\n", hull.mounts.size());

    std::vector<std::string> hull_geoms =
        stitch_find_hull_geoms(hull.hull_model, norm_dir);
    if (hull_geoms.empty()) {
        fprintf(stderr, "Error: no hull geometry files found for %s\n",
                hull.hull_model.c_str());
        return false;
    }
    stitch_vlog("Hull parts: %zu\n", hull_geoms.size());

    std::map<std::string, Mat16d> hp_transforms;
    std::map<std::string, Mat16d> bb_corrections;

    if (opts.with_turrets && !assets_bin_path.empty()) {
        stitch_vlog("Loading HP transforms from assets.bin …\n");

        for (const auto &gp : hull_geoms) {
            std::string suffix = stitch_geom_to_visual_suffix(gp);
            assets_bin_hp_list_t *hp =
                assets_bin_get_hp_transforms(assets_bin_path.c_str(), suffix.c_str());
            if (!hp) continue;
            for (size_t i = 0; i < hp->count; ++i)
                hp_transforms[hp->entries[i].name] =
                    stitch_float_to_double_mat(hp->entries[i].mat);
            assets_bin_hp_list_free(hp);
        }
        stitch_vlog("  %zu HP_ transforms.\n", hp_transforms.size());

        std::vector<std::string> unique_models;
        std::set<std::string>    seen;
        for (const auto &m : hull.mounts)
            if (!m.model_path.empty() && seen.insert(m.model_path).second)
                unique_models.push_back(m.model_path);

        if (!unique_models.empty()) {
            std::vector<const char *> ptrs;
            for (const auto &s : unique_models) ptrs.push_back(s.c_str());
            assets_bin_bb_list_t *bb = assets_bin_get_blendbone_corrections(
                assets_bin_path.c_str(), ptrs.data(), ptrs.size());
            if (bb) {
                for (size_t i = 0; i < bb->count; ++i)
                    bb_corrections[bb->entries[i].model_path] =
                        stitch_float_to_double_mat(bb->entries[i].correction);
                assets_bin_bb_list_free(bb);
            }
        }
    }

    std::vector<GlbPart> parts;

    for (const auto &gp : hull_geoms) {
        stitch_vlog("  Hull part: %s …\n", stitch_path_basename(gp).c_str());
        GlbPart part;
        part.mesh_name = stitch_stem(stitch_path_basename(gp));
        part.geom_path = gp;
        if (stitch_geom_to_model(gp, part.model))
            parts.push_back(std::move(part));
    }

    if (parts.empty()) {
        fprintf(stderr, "Error: no hull parts could be converted.\n");
        return false;
    }

    if (opts.with_turrets) {
        std::map<std::string, std::string> model_to_geom;
        for (const auto &m : hull.mounts) {
            if (model_to_geom.count(m.model_path)) continue;
            std::string gp = stitch_model_to_geom_path(m.model_path, norm_dir);
            model_to_geom[m.model_path] = stitch_file_exists(gp) ? gp : "";
        }

        std::vector<MountEntry> sorted_mounts = hull.mounts;
        std::sort(sorted_mounts.begin(), sorted_mounts.end(),
                  [](const MountEntry &a, const MountEntry &b) {
                      return a.hp_name < b.hp_name;
                  });

        for (const auto &m : sorted_mounts) {
            const std::string &geom_path = model_to_geom[m.model_path];
            if (geom_path.empty()) continue;

            Mat16d transform;
            auto hp_it = hp_transforms.find(m.hp_name);
            if (hp_it != hp_transforms.end()) {
                transform = hp_it->second;
                auto bb_it = bb_corrections.find(m.model_path);
                if (bb_it != bb_corrections.end())
                    transform = stitch_mat4_mul_d(transform, bb_it->second);
            }

            std::string label = stitch_stem(stitch_path_basename(geom_path))
                                + " (" + m.hp_name + ")";
            stitch_vlog("  Turret: %s …\n", label.c_str());

            GlbPart part;
            part.mesh_name = label;
            part.geom_path = geom_path;
            part.matrix    = transform;
            if (stitch_geom_to_model(geom_path, part.model))
                parts.push_back(std::move(part));
        }
    }

    stitch_vlog("Merging %zu part(s) …\n", parts.size());
    tinygltf::Model merged = stitch_merge_parts(parts);

    if (opts.with_textures && !assets_bin_path.empty()) {
        stitch_vlog("Applying textures (size=%d, lod=%d, damage=%s) …\n",
                    opts.max_tex_size, opts.lod_level,
                    opts.exclude_damage ? "excluded" : "included");
        assets_bin_pdb_t *pdb = assets_bin_pdb_open(assets_bin_path.c_str());
        if (!pdb) {
            fprintf(stderr, "Warning: failed to open assets.bin for textures.\n");
        } else {
            std::vector<std::string> geom_order;
            for (const auto &p : parts) geom_order.push_back(p.geom_path);
            stitch_apply_textures(merged, geom_order, pdb, norm_dir,
                                  opts.lod_level, opts.exclude_damage,
                                  opts.max_tex_size);
            assets_bin_pdb_free(pdb);
        }
    } else if (opts.with_textures) {
        fprintf(stderr,
                "Warning: textures enabled but assets.bin not found "
                "(use opts.assets_bin_path or supply game_dir).\n");
    }

    tinygltf::TinyGLTF writer;
    bool ok = writer.WriteGltfSceneToFile(
        &merged, output_path,
        /*embedImages*/ true, /*embedBuffers*/ true,
        /*prettyPrint*/ false, /*writeBinary*/ true);

    if (!ok) {
        fprintf(stderr, "Error: failed to write GLB to %s\n", output_path.c_str());
        return false;
    }

    struct stat st;
    stat(output_path.c_str(), &st);
    fprintf(stderr, "Written: %s (%.1f KB)\n", output_path.c_str(),
            st.st_size / 1024.0);
    fprintf(stderr, "  Meshes:      %zu\n", merged.meshes.size());
    fprintf(stderr, "  Accessors:   %zu\n", merged.accessors.size());
    fprintf(stderr, "  BufferViews: %zu\n", merged.bufferViews.size());

    return true;
}
