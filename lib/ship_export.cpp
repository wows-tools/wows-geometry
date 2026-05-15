#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <tiny_gltf.h>

extern "C" {
#include "wows-assets-bin.h"
#include "wows-depack.h"
}
#include "wows-model-exporter.h"
#include "wows-game-params.h"

#include <cstdio>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#define vlog(tag, fmt, ...)                                                                                            \
    do {                                                                                                               \
        if (wows_stitch_verbose)                                                                                       \
            fprintf(stderr, "[%s] " fmt, tag, ##__VA_ARGS__);                                                          \
    } while (0)
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

/* ── depack helpers ─────────────────────────────────────────────── */

/* Open the latest index dir under game_dir as a depack context. */
static WOWS_CONTEXT *ship_open_depack(const std::string &game_dir) {
    char *idx_dir = nullptr;
    if (wows_get_latest_idx_dir(const_cast<char *>(game_dir.c_str()), &idx_dir) != 0 || !idx_dir)
        return nullptr;
    WOWS_CONTEXT *ctx = wows_init_context(WOWS_NO_DEBUG);
    if (!ctx) {
        free(idx_dir);
        return nullptr;
    }
    int ret = wows_parse_index_dir(idx_dir, ctx);
    free(idx_dir);
    if (ret != 0) {
        wows_free_context(ctx);
        return nullptr;
    }
    return ctx;
}

/* Extract archive_path from depack directly into a memory buffer. */
static std::vector<uint8_t> ship_depack_read(WOWS_CONTEXT *ctx, const std::string &archive_path) {
    char *buf = nullptr;
    size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (!fp)
        return {};
    int ret = wows_extract_file_fp(ctx, const_cast<char *>(archive_path.c_str()), fp);
    fclose(fp);
    if (ret != 0) {
        free(buf);
        return {};
    }
    std::vector<uint8_t> result(buf, buf + sz);
    free(buf);
    return result;
}

/* Search archive for pattern; return first match bytes (empty on failure). */
static std::vector<uint8_t> ship_depack_search_read(WOWS_CONTEXT *ctx, const char *pattern_str) {
    int count = 0;
    char **results = nullptr;
    if (wows_search(ctx, const_cast<char *>(pattern_str), WOWS_SEARCH_FULL_PATH, &count, &results) != 0 || count == 0) {
        if (results) {
            for (int i = 0; i < count; i++)
                free(results[i]);
            free(results);
        }
        return {};
    }
    std::string arch_path(results[0]);
    for (int i = 0; i < count; i++)
        free(results[i]);
    free(results);
    return ship_depack_read(ctx, arch_path);
}

/* Strip norm_dir prefix from an absolute path to produce an archive-relative path. */
static std::string ship_to_rel(const std::string &abs_path, const std::string &norm_dir) {
    std::string rel = wows_stitch_normalize_slashes(abs_path);
    if (rel.size() > norm_dir.size() && rel.compare(0, norm_dir.size(), norm_dir) == 0)
        rel = rel.substr(norm_dir.size());
    if (!rel.empty() && rel[0] == '/')
        rel = rel.substr(1);
    return rel;
}

/* ── in-memory export (core implementation) ─────────────────────── */

bool wows_stitch_export_ship_to_glb_mem(const std::string &game_dir, const std::string &ship_name,
                                        std::vector<uint8_t> &glb_out, const wows_ship_export_options &opts,
                                        wows_file_provider_t file_provider) {
    std::string norm_dir = wows_stitch_normalize_slashes(game_dir);

    const bool gp_from_mem = !opts.gameparams_data.empty();
    std::string gameparams_path = opts.gameparams_path;
    if (!gp_from_mem && gameparams_path.empty()) {
        gameparams_path = wows_stitch_find_game_file(norm_dir, "GameParams.data");
        if (!gameparams_path.empty())
            vlog("GameParams.data", "auto-detected: %s\n", gameparams_path.c_str());
    }
    if (!gp_from_mem && gameparams_path.empty()) {
        vlog("GameParams.data",
             "not found under %s\n"
             "       Supply opts.gameparams_path to specify it explicitly.\n",
             norm_dir.c_str());
        return false;
    }

    const bool assets_from_mem = !opts.assets_bin_data.empty();
    std::string assets_bin_path = opts.wows_assets_bin_path;
    if (!assets_from_mem && assets_bin_path.empty()) {
        assets_bin_path = wows_stitch_find_game_file(norm_dir, "assets.bin");
        if (!assets_bin_path.empty())
            vlog("assets.bin", "auto-detected: %s\n", assets_bin_path.c_str());
    }

    auto pdb_deleter = [](wows_assets_bin_pdb_t *p) {
        if (p)
            wows_assets_bin_pdb_free(p);
    };
    std::unique_ptr<wows_assets_bin_pdb_t, decltype(pdb_deleter)> assets_pdb(nullptr, pdb_deleter);
    const bool want_assets_bin = opts.with_turrets || opts.with_textures || opts.exclude_damage || opts.with_propellers;
    if (want_assets_bin && (assets_from_mem || !assets_bin_path.empty())) {
        wows_assets_bin_pdb_t *p =
            assets_from_mem ? wows_assets_bin_pdb_open_memory(opts.assets_bin_data.data(), opts.assets_bin_data.size())
                            : wows_assets_bin_pdb_open(assets_bin_path.c_str());
        assets_pdb.reset(p);
    }
    wows_assets_bin_pdb_t *assets_pdb_ptr = assets_pdb.get();

    vlog("GameParams.data", "loading …\n");
    if (!Py_IsInitialized())
        Py_Initialize();
    wows_hull_info hull;
    const char *hull_sel = opts.hull_upgrade.empty() ? nullptr : opts.hull_upgrade.c_str();
    if (gp_from_mem) {
        if (!wows_load_hull_info_from_memory(opts.gameparams_data.data(), opts.gameparams_data.size(),
                                             ship_name.c_str(), hull_sel, hull))
            return false;
    } else {
        if (!wows_load_hull_info(gameparams_path.c_str(), ship_name.c_str(), hull_sel, hull))
            return false;
    }

    vlog("GameParams.data", "hull model: %s\n", hull.hull_model.c_str());
    vlog("GameParams.data", "mounts: %zu\n", hull.mounts.size());

    /* geometry reader: prefers file_provider, falls back to filesystem */
    auto read_geom = [&](const std::string &gp, tinygltf::Model &m) -> bool {
        if (file_provider) {
            auto buf = file_provider(ship_to_rel(gp, norm_dir));
            if (!buf.empty())
                return wows_stitch_geom_to_model_from_memory(buf.data(), buf.size(), m);
        }
        return wows_stitch_geom_to_model(gp, m);
    };

    auto geom_exists = [&](const std::string &gp) -> bool {
        if (file_provider)
            return !file_provider(ship_to_rel(gp, norm_dir)).empty();
        return wows_stitch_file_exists(gp);
    };

    /* hull geometry parts */
    std::vector<std::string> hull_geoms = wows_stitch_find_hull_geoms(hull.hull_model, norm_dir);

    if (hull_geoms.empty()) {
        /* probe part files via file_provider or filesystem */
        std::string base_gp = wows_stitch_model_to_geom_path(hull.hull_model, norm_dir);
        std::string ship_dir = wows_stitch_path_dirname(base_gp);
        std::string base_name = wows_stitch_stem(wows_stitch_path_basename(base_gp));
        static const char *SUFFIXES[] = {"_Bow", "_BowFront", "_Stern", "_SternBack", "_MidFront", "_MidBack",
                                         "_Mid", "_Mid1",     "_Mid2",  "_Mid3",      nullptr};
        for (const char **s = SUFFIXES; *s; ++s) {
            std::string gp = ship_dir + "/" + base_name + *s + ".geometry";
            if (geom_exists(gp))
                hull_geoms.push_back(gp);
        }
        if (hull_geoms.empty() && geom_exists(base_gp))
            hull_geoms.push_back(base_gp);
    }

    if (hull_geoms.empty()) {
        vlog(ship_name.c_str(), "no hull geometry files found\n");
        return false;
    }
    vlog("GameParams.data", "hull geometry parts: %zu\n", hull_geoms.size());

    std::map<std::string, wows_mat16d> hp_transforms;
    std::map<std::string, wows_mat16d> bb_corrections;

    if (opts.with_turrets && assets_pdb_ptr) {
        vlog("assets.bin", "loading HP transforms …\n");
        for (const auto &gp : hull_geoms) {
            std::string suffix = wows_stitch_geom_to_visual_suffix(gp);
            wows_assets_bin_hp_list_t *hp = wows_assets_bin_get_hp_transforms_pdb(assets_pdb_ptr, suffix.c_str());
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
                wows_assets_bin_get_blendbone_corrections_pdb(assets_pdb_ptr, ptrs.data(), ptrs.size());
            if (bb) {
                for (size_t i = 0; i < bb->count; ++i)
                    bb_corrections[bb->entries[i].model_path] =
                        wows_stitch_float_to_double_mat(bb->entries[i].correction);
                wows_assets_bin_bb_list_free(bb);
            }
        }
    }

    auto report = [&](int pct) {
        if (opts.progress_cb)
            opts.progress_cb(pct);
    };
    size_t n_hull = hull_geoms.size() ? hull_geoms.size() : 1;

    std::vector<wows_glb_part> parts;
    for (size_t i = 0; i < hull_geoms.size(); ++i) {
        const auto &gp = hull_geoms[i];
        vlog(wows_stitch_path_basename(gp).c_str(), "loading geometry …\n");
        wows_glb_part part;
        part.mesh_name = wows_stitch_stem(wows_stitch_path_basename(gp));
        part.geom_path = gp;
        if (read_geom(gp, part.model))
            parts.push_back(std::move(part));
        report((int)((i + 1) * 20 / n_hull));
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
            model_to_geom[m.model_path] = geom_exists(gp) ? gp : "";
        }

        std::vector<wows_mount_entry> sorted_mounts = hull.mounts;
        std::sort(sorted_mounts.begin(), sorted_mounts.end(),
                  [](const wows_mount_entry &a, const wows_mount_entry &b) { return a.hp_name < b.hp_name; });

        for (const auto &m : sorted_mounts) {
            const std::string &geom_path = model_to_geom[m.model_path];
            if (geom_path.empty()) {
                vlog(m.hp_name.c_str(), "skipping mount (geometry not found for model: %s)\n", m.model_path.c_str());
                continue;
            }

            wows_mat16d transform;
            auto hp_it = hp_transforms.find(m.hp_name);
            if (hp_it != hp_transforms.end()) {
                transform = hp_it->second;
                auto bb_it = bb_corrections.find(m.model_path);
                if (bb_it != bb_corrections.end())
                    transform = wows_stitch_mat4_mul_d(transform, bb_it->second);
            }
            vlog(wows_stitch_path_basename(geom_path).c_str(), "loading turret geometry (%s) …\n", m.hp_name.c_str());
            wows_glb_part part;
            part.mesh_name = wows_stitch_stem(wows_stitch_path_basename(geom_path)) + " (" + m.hp_name + ")";
            part.geom_path = geom_path;
            part.matrix = transform;
            if (read_geom(geom_path, part.model))
                parts.push_back(std::move(part));
        }
    }

    /* TODO: propeller placement is not yet correct — Y-offset heuristic (keel-relative
     * vs. visual space) is unreliable and shaft positions are wrong for several ships.
     * This block is disabled at the CLI level until the underlying issues are resolved. */
    if (opts.with_propellers) {
        if (!assets_pdb_ptr) {
            vlog(ship_name.c_str(), "assets.bin unavailable — propellers skipped\n");
        } else {
            std::vector<const char *> geom_ptrs;
            for (const auto &gp : hull_geoms)
                geom_ptrs.push_back(gp.c_str());

            wows_assets_bin_propeller_list_t *prop_list = wows_assets_bin_get_propellers_pdb(
                assets_pdb_ptr, hull.hull_model.c_str(), geom_ptrs.data(), geom_ptrs.size());

            if (prop_list) {
                /* Skel_ext bone Y is measured from the ship's keel (Y=0=keel),
                 * while the visual coordinate system has Y=0=waterline.
                 * Apply correction: visual_Y = skel_ext_Y + hull_keel_Y_visual.
                 * Compute hull keel Y as the minimum Y across all hull part AABBs. */
                float hull_keel_y = 0.0f;
                bool keel_found = false;
                for (const auto &hp : parts) {
                    for (const auto &acc : hp.model.accessors) {
                        if (acc.type == TINYGLTF_TYPE_VEC3 && acc.minValues.size() >= 2) {
                            float y_min = (float)acc.minValues[1];
                            if (!keel_found || y_min < hull_keel_y) {
                                hull_keel_y = y_min;
                                keel_found = true;
                            }
                        }
                    }
                }
                if (keel_found)
                    vlog(ship_name.c_str(), "hull keel Y=%.3f → applying propeller Y correction\n", hull_keel_y);
                for (size_t i = 0; i < prop_list->count; ++i) {
                    /* Only apply keel→visual offset when the bone Y is positive
                     * (keel-relative space, Y=0=keel).  Negative Y means the
                     * coordinate is already in visual/world space (Y=0≈waterline)
                     * and must not be shifted further. */
                    if (keel_found && prop_list->entries[i].mat[13] > 0.05f)
                        prop_list->entries[i].mat[13] += hull_keel_y;
                }

                vlog(ship_name.c_str(), "propellers: %zu\n", prop_list->count);
                for (size_t i = 0; i < prop_list->count; ++i) {
                    const wows_assets_bin_propeller_t &p = prop_list->entries[i];
                    vlog(p.name, "position (%.3f, %.3f, %.3f)\n", p.mat[12], p.mat[13], p.mat[14]);
                    std::string geom_full = norm_dir + "/" + p.geom_path;
                    wows_glb_part part;
                    part.mesh_name = p.name;
                    part.geom_path = geom_full;
                    part.matrix = wows_stitch_float_to_double_mat(p.mat);
                    if (read_geom(geom_full, part.model))
                        parts.push_back(std::move(part));
                }
                wows_assets_bin_propeller_list_free(prop_list);
            } else {
                vlog(ship_name.c_str(), "no propeller data in assets.bin\n");
            }
        }
    }

    vlog(ship_name.c_str(), "merging %zu part(s) …\n", parts.size());
    tinygltf::Model merged = wows_stitch_merge_parts(parts);

    if ((opts.with_textures || opts.exclude_damage) && assets_pdb_ptr) {
        vlog(ship_name.c_str(), "applying visual info (textures=%s, damage=%s) …\n", opts.with_textures ? "yes" : "no",
             opts.exclude_damage ? "excluded" : "included");
        std::vector<std::string> geom_order;
        for (const auto &p : parts)
            geom_order.push_back(p.geom_path);
        int tex_size = opts.with_textures ? opts.max_tex_size : 0;
        wows_stitch_apply_textures(merged, geom_order, assets_pdb_ptr, norm_dir, opts.lod_level, opts.exclude_damage,
                                   tex_size, opts.progress_cb, file_provider);
    } else if (opts.with_textures) {
        if (!assets_from_mem && assets_bin_path.empty())
            vlog("assets.bin", "not found — textures skipped\n");
        else if (!assets_pdb_ptr)
            vlog("assets.bin", "failed to open for visual lookup\n");
    }

    wows_stitch_apply_default_material(merged);

    tinygltf::TinyGLTF writer;
    std::ostringstream oss;
    if (!writer.WriteGltfSceneToStream(&merged, oss, false, true))
        return false;
    const std::string &s = oss.str();
    glb_out.assign(s.begin(), s.end());
    return true;
}

/* ── file export (thin wrapper around the in-memory variant) ─────── */

bool wows_stitch_export_ship(const std::string &game_dir, const std::string &ship_name, const std::string &output_path,
                             const wows_ship_export_options &opts) {
    std::string norm_dir = wows_stitch_normalize_slashes(game_dir);

    /* resolve GameParams.data and assets.bin from filesystem */
    std::string gameparams_path = opts.gameparams_path;
    if (gameparams_path.empty()) {
        gameparams_path = wows_stitch_find_game_file(norm_dir, "GameParams.data");
        if (!gameparams_path.empty())
            vlog("GameParams.data", "auto-detected: %s\n", gameparams_path.c_str());
    }

    std::string assets_bin_path = opts.wows_assets_bin_path;
    if (assets_bin_path.empty()) {
        assets_bin_path = wows_stitch_find_game_file(norm_dir, "assets.bin");
        if (!assets_bin_path.empty())
            vlog("assets.bin", "auto-detected: %s\n", assets_bin_path.c_str());
    }

    /* open depack archive index */
    WOWS_CONTEXT *dctx = ship_open_depack(norm_dir);
    if (dctx)
        vlog("depack", "archive index opened\n");

    std::vector<uint8_t> gameparams_mem;
    std::vector<uint8_t> assets_bin_mem;

    /* load GameParams.data / assets.bin from archive index if not on disk */
    if (dctx && gameparams_path.empty() && opts.gameparams_data.empty()) {
        gameparams_mem = ship_depack_search_read(dctx, ".*GameParams\\.data");
        if (!gameparams_mem.empty())
            vlog("GameParams.data", "loaded from archive (%zu bytes)\n", gameparams_mem.size());
    }
    if (dctx && assets_bin_path.empty() && opts.assets_bin_data.empty()) {
        assets_bin_mem = ship_depack_search_read(dctx, ".*assets\\.bin");
        if (!assets_bin_mem.empty())
            vlog("assets.bin", "loaded from archive (%zu bytes)\n", assets_bin_mem.size());
    }

    auto cleanup = [&]() {
        if (dctx) {
            wows_free_context(dctx);
            dctx = nullptr;
        }
    };

    /* populate resolved options and delegate to the in-memory variant */
    wows_ship_export_options resolved_opts = opts;
    resolved_opts.gameparams_path = gameparams_path;
    resolved_opts.wows_assets_bin_path = assets_bin_path;
    if (!gameparams_mem.empty())
        resolved_opts.gameparams_data = std::move(gameparams_mem);
    if (!assets_bin_mem.empty())
        resolved_opts.assets_bin_data = std::move(assets_bin_mem);

    if (resolved_opts.gameparams_path.empty() && resolved_opts.gameparams_data.empty()) {
        vlog("GameParams.data",
             "not found under %s (filesystem or archive)\n"
             "       Supply opts.gameparams_path to specify it explicitly.\n",
             norm_dir.c_str());
        cleanup();
        return false;
    }

    /* build file_provider backed by the depack context */
    wows_file_provider_t file_provider = nullptr;
    if (dctx) {
        WOWS_CONTEXT *ctx = dctx;
        file_provider = [ctx](const std::string &rel) -> std::vector<uint8_t> { return ship_depack_read(ctx, rel); };
    }

    std::vector<uint8_t> glb;
    bool ok = wows_stitch_export_ship_to_glb_mem(game_dir, ship_name, glb, resolved_opts, file_provider);
    cleanup();
    if (!ok)
        return false;

    /* write GLB to output file */
    FILE *f = fopen(output_path.c_str(), "wb");
    if (!f) {
        vlog(wows_stitch_path_basename(output_path).c_str(), "failed to open for writing\n");
        return false;
    }
    fwrite(glb.data(), 1, glb.size(), f);
    fclose(f);

    vlog(wows_stitch_path_basename(output_path).c_str(), "written %.1f KB\n", glb.size() / 1024.0);
    return true;
}
