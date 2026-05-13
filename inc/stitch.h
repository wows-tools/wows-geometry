#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <tiny_gltf.h>
#include "assets_bin.h"

/* verbose flag — set to true before calling library functions */
extern bool g_stitch_verbose;
#define stitch_vlog(...) do { if (g_stitch_verbose) fprintf(stderr, __VA_ARGS__); } while(0)

/* column-major 4×4 matrix as a flat double vector */
using Mat16d = std::vector<double>;

struct MountEntry {
    std::string hp_name;
    std::string model_path;
};

struct HullInfo {
    std::string hull_model;
    std::vector<MountEntry> mounts;
};

struct GlbPart {
    tinygltf::Model model;
    std::string     mesh_name;
    std::string     geom_path;
    Mat16d          matrix;    /* empty → identity */
};

/* ── path / file utilities ───────────────────────────────────────── */
std::string stitch_path_basename(const std::string &p);
std::string stitch_path_dirname(const std::string &p);
std::string stitch_stem(const std::string &filename);
std::string stitch_normalize_slashes(std::string s);
bool        stitch_file_exists(const std::string &p);

std::string stitch_model_to_geom_path(const std::string &model,
                                       const std::string &game_dir);
std::string stitch_geom_to_visual_suffix(const std::string &geom_path);
std::vector<std::string> stitch_find_hull_geoms(const std::string &hull_model,
                                                 const std::string &game_dir);
std::string stitch_find_game_file(const std::string &game_dir,
                                   const std::string &filename);

/* ── math helpers ────────────────────────────────────────────────── */
Mat16d stitch_mat4_mul_d(const Mat16d &a, const Mat16d &b);
Mat16d stitch_float_to_double_mat(const float m[16]);

/* ── geometry I/O ────────────────────────────────────────────────── */
bool            stitch_geom_to_model(const std::string &geom_path,
                                      tinygltf::Model &model_out);
tinygltf::Model stitch_merge_parts(std::vector<GlbPart> &parts);

/* ── DDS decoding ────────────────────────────────────────────────── */
std::vector<uint8_t> stitch_decode_dds(const uint8_t *d, size_t sz,
                                        int *W, int *H);
std::vector<uint8_t> stitch_dds_to_png(const std::string &path, int max_sz);

/* ── texture application ─────────────────────────────────────────── */
void stitch_apply_textures(tinygltf::Model &model,
                            const std::vector<std::string> &geom_order,
                            assets_bin_pdb_t *pdb,
                            const std::string &game_dir,
                            int lod_level, bool excl_damage, int max_tex);
