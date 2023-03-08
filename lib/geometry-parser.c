#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <libgen.h>
#include <linux/limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "wows-geometry.h"
#include "hashmap.h"

int geometry_entry_compare(const void *a, const void *b, void *udata) {
    const wows_geometry_entry *ea = *(wows_geometry_entry **)a;
    const wows_geometry_entry *eb = *(wows_geometry_entry **)b;
    return strcmp(ea->name, eb->name);
}

uint64_t geometry_entry_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const wows_geometry_entry *entry = *(wows_geometry_entry **)item;
    return hashmap_sip(entry->name, strlen(entry->name), seed0, seed1);
}

int wows_geometry_entry_print(wows_geometry_entry *entry) {
    printf("* name_len: %d\n", entry->name_len);
    printf("* name:     %s\n", entry->name);
    printf("* x:        %f\n", entry->x);
    printf("* y:        %f\n", entry->y);
    printf("* z:        %f\n", entry->z);
    printf("* dx:       %f\n", entry->dx);
    printf("* dy:       %f\n", entry->dy);
    printf("* dz:       %f\n", entry->dz);
    return 0;
}

int wows_geometry_print(wows_geometry *geometry) {
    printf("* number_entries: %d\n", geometry->entry_count);
    for (int i = 0; i < geometry->entry_count; i++) {
        printf("====================\n");
        wows_geometry_entry_print(geometry->entries[i]);
    }
    return 0;
}

int wows_parse_geometry_fp(FILE *fp, wows_geometry **geometry_content) {
    // TODO error handling in case the file is sketchy

    int ret = 0;

    wows_geometry *geometry = calloc(sizeof(wows_geometry), 1);

    fread(&(geometry->entry_count), sizeof(uint32_t), 1, fp);

    geometry->entry_map =
        hashmap_new(sizeof(wows_geometry_entry **), 0, 0, 0, geometry_entry_hash, geometry_entry_compare, NULL, NULL);

    geometry->entries = calloc(sizeof(wows_geometry_entry), geometry->entry_count);

    for (int i = 0; i < geometry->entry_count; i++) {
        wows_geometry_entry *entry = calloc(sizeof(wows_geometry_entry), 1);
        fread(&(entry->name_len), sizeof(uint32_t), 1, fp);
        entry->name = calloc(sizeof(char), entry->name_len + 1);
        fread(entry->name, sizeof(char), entry->name_len, fp);
        fread(&(entry->x), sizeof(float), 1, fp);
        fread(&(entry->y), sizeof(float), 1, fp);
        fread(&(entry->z), sizeof(float), 1, fp);
        fread(&(entry->dx), sizeof(float), 1, fp);
        fread(&(entry->dy), sizeof(float), 1, fp);
        fread(&(entry->dz), sizeof(float), 1, fp);
        hashmap_set(geometry->entry_map, &entry);
        geometry->entries[i] = entry;
    }

    ret = feof(fp);
    *geometry_content = geometry;
    return ret;
}

int wows_parse_geometry(char *input, wows_geometry **geometry_content) {
    int ret = 0;

    // Open the index file
    FILE *fp = fopen(input, "ro");
    if (fp <= 0) {
        return -1;
    }
    ret = wows_parse_geometry_fp(fp, geometry_content);
    fclose(fp);
    return ret;
}

int wows_geometry_free(wows_geometry *geometry) {
    for (int i = 0; i < geometry->entry_count; i++) {
        wows_geometry_entry *entry = geometry->entries[i];
        free(entry->name);
        free(entry);
    }
    free(geometry->entries);
    hashmap_free(geometry->entry_map);
    free(geometry);
    return 0;
}
