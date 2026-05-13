#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <tiny_gltf.h>

extern "C" {
#include "wows-assets-bin.h"
}
#include "wows-model-exporter.h"
#include "wows-game-params.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <map>
#define vlog(tag, fmt, ...) do { if (wows_stitch_verbose) fprintf(stderr, "[%s] " fmt, tag, ##__VA_ARGS__); } while (0)
#include <set>
#include <string>
#include <vector>

bool wows_stitch_export_ship(const std::string &game_dir, const std::string &ship_name, const std::string &output_path,
                        const wows_ship_export_options &opts) {
    std::string norm_dir = wows_stitch_normalize_slashes(game_dir);

    std::string gameparams_path = opts.gameparams_path;
    if (gameparams_path.empty()) {
        gameparams_path = wows_stitch_find_game_file(norm_dir, "GameParams.data");
        if (gameparams_path.empty()) {
            vlog("GameParams.data", "not found under %s\n"
                 "       Supply opts.gameparams_path to specify it explicitly.\n",
                 norm_dir.c_str());
            return false;
        }
        vlog("GameParams.data", "auto-detected: %s\n", gameparams_path.c_str());
    }

    std::string wows_assets_bin_path = opts.wows_assets_bin_path;
    if (wows_assets_bin_path.empty()) {
        wows_assets_bin_path = wows_stitch_find_game_file(norm_dir, "assets.bin");
        if (!wows_assets_bin_path.empty())
            vlog("assets.bin", "auto-detected: %s\n", wows_assets_bin_path.c_str());
    }

    vlog("GameParams.data", "loading …\n");
    Py_Initialize();

    wows_hull_info hull;
    const char *hull_sel = opts.hull_upgrade.empty() ? nullptr : opts.hull_upgrade.c_str();
    if (!wows_load_hull_info(gameparams_path.c_str(), ship_name.c_str(), hull_sel, hull)) {
        Py_Finalize();
        return false;
    }
    Py_Finalize();

    vlog("GameParams.data", "hull model: %s\n", hull.hull_model.c_str());
    vlog("GameParams.data", "mounts: %zu\n", hull.mounts.size());

    std::vector<std::string> hull_geoms = wows_stitch_find_hull_geoms(hull.hull_model, norm_dir);
    if (hull_geoms.empty()) {
        vlog(wows_stitch_path_basename(hull.hull_model).c_str(), "no geometry files found\n");
        return false;
    }
    vlog("GameParams.data", "hull geometry parts: %zu\n", hull_geoms.size());

    std::map<std::string, wows_mat16d> hp_transforms;
    std::map<std::string, wows_mat16d> bb_corrections;

    if (opts.with_turrets && !wows_assets_bin_path.empty()) {
        vlog("assets.bin", "loading HP transforms …\n");

        for (const auto &gp : hull_geoms) {
            std::string suffix = wows_stitch_geom_to_visual_suffix(gp);
            wows_assets_bin_hp_list_t *hp = wows_assets_bin_get_hp_transforms(wows_assets_bin_path.c_str(), suffix.c_str());
            if (!hp)
                continue;
            for (size_t i = 0; i < hp->count; ++i)
                hp_transforms[hp->entries[i].name] = wows_stitch_float_to_double_mat(hp->entries[i].mat);
            wows_assets_bin_hp_list_free(hp);
        }
        vlog("assets.bin", "%zu HP_ transforms loaded\n", hp_transforms.size());

        std::vector<std::string> unique_models;
        std::set<std::string> seen;
        for (const auto &m : hull.mounts)
            if (!m.model_path.empty() && seen.insert(m.model_path).second)
                unique_models.push_back(m.model_path);

        if (!unique_models.empty()) {
            std::vector<const char *> ptrs;
            for (const auto &s : unique_models)
                ptrs.push_back(s.c_str());
            wows_assets_bin_bb_list_t *bb =
                wows_assets_bin_get_blendbone_corrections(wows_assets_bin_path.c_str(), ptrs.data(), ptrs.size());
            if (bb) {
                for (size_t i = 0; i < bb->count; ++i)
                    bb_corrections[bb->entries[i].model_path] = wows_stitch_float_to_double_mat(bb->entries[i].correction);
                wows_assets_bin_bb_list_free(bb);
            }
        }
    }

    std::vector<wows_glb_part> parts;

    for (const auto &gp : hull_geoms) {
        vlog(wows_stitch_path_basename(gp).c_str(), "loading geometry …\n");
        wows_glb_part part;
        part.mesh_name = wows_stitch_stem(wows_stitch_path_basename(gp));
        part.geom_path = gp;
        if (wows_stitch_geom_to_model(gp, part.model))
            parts.push_back(std::move(part));
    }

    if (parts.empty()) {
        vlog(ship_name.c_str(), "no hull parts could be converted\n");
        return false;
    }

    if (opts.with_turrets) {
        std::map<std::string, std::string> model_to_geom;
        for (const auto &m : hull.mounts) {
            if (model_to_geom.count(m.model_path))
                continue;
            std::string gp = wows_stitch_model_to_geom_path(m.model_path, norm_dir);
            model_to_geom[m.model_path] = wows_stitch_file_exists(gp) ? gp : "";
        }

        std::vector<wows_mount_entry> sorted_mounts = hull.mounts;
        std::sort(sorted_mounts.begin(), sorted_mounts.end(),
                  [](const wows_mount_entry &a, const wows_mount_entry &b) { return a.hp_name < b.hp_name; });

        for (const auto &m : sorted_mounts) {
            const std::string &geom_path = model_to_geom[m.model_path];
            if (geom_path.empty())
                continue;

            wows_mat16d transform;
            auto hp_it = hp_transforms.find(m.hp_name);
            if (hp_it != hp_transforms.end()) {
                transform = hp_it->second;
                auto bb_it = bb_corrections.find(m.model_path);
                if (bb_it != bb_corrections.end())
                    transform = wows_stitch_mat4_mul_d(transform, bb_it->second);
            }

            std::string label = wows_stitch_stem(wows_stitch_path_basename(geom_path)) + " (" + m.hp_name + ")";
            vlog(wows_stitch_path_basename(geom_path).c_str(), "loading turret geometry (%s) …\n", m.hp_name.c_str());

            wows_glb_part part;
            part.mesh_name = label;
            part.geom_path = geom_path;
            part.matrix = transform;
            if (wows_stitch_geom_to_model(geom_path, part.model))
                parts.push_back(std::move(part));
        }
    }

    vlog(ship_name.c_str(), "merging %zu part(s) …\n", parts.size());
    tinygltf::Model merged = wows_stitch_merge_parts(parts);

    if (opts.with_textures && !wows_assets_bin_path.empty()) {
        vlog(ship_name.c_str(), "applying textures (max_size=%d, lod=%d, damage=%s) …\n",
             opts.max_tex_size, opts.lod_level, opts.exclude_damage ? "excluded" : "included");
        wows_assets_bin_pdb_t *pdb = wows_assets_bin_pdb_open(wows_assets_bin_path.c_str());
        if (!pdb) {
            vlog("assets.bin", "failed to open for texture lookup\n");
        } else {
            std::vector<std::string> geom_order;
            for (const auto &p : parts)
                geom_order.push_back(p.geom_path);
            wows_stitch_apply_textures(merged, geom_order, pdb, norm_dir, opts.lod_level, opts.exclude_damage,
                                  opts.max_tex_size);
            wows_assets_bin_pdb_free(pdb);
        }
    } else if (opts.with_textures) {
        vlog("assets.bin", "not found — textures skipped "
             "(use opts.wows_assets_bin_path or supply game_dir)\n");
    }

    wows_stitch_apply_default_material(merged);

    tinygltf::TinyGLTF writer;
    bool ok = writer.WriteGltfSceneToFile(&merged, output_path,
                                          /*embedImages*/ true, /*embedBuffers*/ true,
                                          /*prettyPrint*/ false, /*writeBinary*/ true);

    std::string out_tag_str = wows_stitch_path_basename(output_path);
    const char *out_tag = out_tag_str.c_str();
    if (!ok) {
        vlog(out_tag, "failed to write GLB\n");
        return false;
    }

    struct stat st;
    stat(output_path.c_str(), &st);
    vlog(out_tag, "written %.1f KB\n", st.st_size / 1024.0);
    vlog(out_tag, "  meshes:       %zu\n", merged.meshes.size());
    vlog(out_tag, "  accessors:    %zu\n", merged.accessors.size());
    vlog(out_tag, "  buffer views: %zu\n", merged.bufferViews.size());

    return true;
}
