#include "mirror_manifest.h"
#include "alloc.h"
#include "constants.h"
#include "fileutil.h"
#include "vendor/cJSON.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

MirrorManifest* mirror_manifest_create(void) {
    MirrorManifest *m = arena_calloc(1, sizeof(MirrorManifest));
    if (!m) return NULL;

    m->package_capacity = INITIAL_MEDIUM_CAPACITY;
    m->packages = arena_calloc(m->package_capacity, sizeof(MirrorPackageEntry));
    if (!m->packages) {
        arena_free(m);
        return NULL;
    }

    return m;
}

void mirror_manifest_free(MirrorManifest *m) {
    if (!m) return;

    for (size_t i = 0; i < m->package_count; i++) {
        MirrorPackageEntry *pkg = &m->packages[i];
        if (pkg->author) arena_free(pkg->author);
        if (pkg->name) arena_free(pkg->name);

        for (size_t j = 0; j < pkg->version_count; j++) {
            MirrorVersionEntry *ver = &pkg->versions[j];
            if (ver->version) arena_free(ver->version);
            if (ver->hash) arena_free(ver->hash);
            if (ver->url) arena_free(ver->url);
        }
        if (pkg->versions) arena_free(pkg->versions);
    }

    if (m->packages) arena_free(m->packages);
    if (m->generated) arena_free(m->generated);
    if (m->source) arena_free(m->source);
    arena_free(m);
}

/* Find or create a package entry */
static MirrorPackageEntry* find_or_create_package(MirrorManifest *m,
                                                   const char *author,
                                                   const char *name) {
    /* Search for existing package */
    for (size_t i = 0; i < m->package_count; i++) {
        if (strcmp(m->packages[i].author, author) == 0 &&
            strcmp(m->packages[i].name, name) == 0) {
            return &m->packages[i];
        }
    }

    /* Create new package entry */
    if (m->package_count >= m->package_capacity) {
        size_t new_cap = m->package_capacity * 2;
        MirrorPackageEntry *new_pkgs = arena_realloc(m->packages,
                                                      new_cap * sizeof(MirrorPackageEntry));
        if (!new_pkgs) return NULL;
        m->packages = new_pkgs;
        m->package_capacity = new_cap;
    }

    MirrorPackageEntry *pkg = &m->packages[m->package_count];
    memset(pkg, 0, sizeof(MirrorPackageEntry));

    pkg->author = arena_strdup(author);
    pkg->name = arena_strdup(name);
    if (!pkg->author || !pkg->name) {
        if (pkg->author) arena_free(pkg->author);
        if (pkg->name) arena_free(pkg->name);
        return NULL;
    }

    pkg->version_capacity = INITIAL_SMALL_CAPACITY;
    pkg->versions = arena_calloc(pkg->version_capacity, sizeof(MirrorVersionEntry));
    if (!pkg->versions) {
        arena_free(pkg->author);
        arena_free(pkg->name);
        return NULL;
    }

    m->package_count++;
    return pkg;
}

bool mirror_manifest_add(MirrorManifest *m, const char *author, const char *name,
                         const char *version, const char *hash, const char *url) {
    if (!m || !author || !name || !version || !hash) return false;

    MirrorPackageEntry *pkg = find_or_create_package(m, author, name);
    if (!pkg) return false;

    /* Check if version already exists */
    for (size_t i = 0; i < pkg->version_count; i++) {
        if (strcmp(pkg->versions[i].version, version) == 0) {
            /* Update existing entry */
            if (pkg->versions[i].hash) arena_free(pkg->versions[i].hash);
            if (pkg->versions[i].url) arena_free(pkg->versions[i].url);
            pkg->versions[i].hash = arena_strdup(hash);
            pkg->versions[i].url = url ? arena_strdup(url) : NULL;
            return pkg->versions[i].hash != NULL;
        }
    }

    /* Add new version entry */
    if (pkg->version_count >= pkg->version_capacity) {
        size_t new_cap = pkg->version_capacity * 2;
        MirrorVersionEntry *new_vers = arena_realloc(pkg->versions,
                                                      new_cap * sizeof(MirrorVersionEntry));
        if (!new_vers) return false;
        pkg->versions = new_vers;
        pkg->version_capacity = new_cap;
    }

    MirrorVersionEntry *ver = &pkg->versions[pkg->version_count];
    ver->version = arena_strdup(version);
    ver->hash = arena_strdup(hash);
    ver->url = url ? arena_strdup(url) : NULL;

    if (!ver->version || !ver->hash) {
        if (ver->version) arena_free(ver->version);
        if (ver->hash) arena_free(ver->hash);
        if (ver->url) arena_free(ver->url);
        return false;
    }

    pkg->version_count++;
    return true;
}

const char* mirror_manifest_lookup(MirrorManifest *m, const char *author,
                                   const char *name, const char *version) {
    if (!m || !author || !name || !version) return NULL;

    for (size_t i = 0; i < m->package_count; i++) {
        MirrorPackageEntry *pkg = &m->packages[i];
        if (strcmp(pkg->author, author) == 0 && strcmp(pkg->name, name) == 0) {
            for (size_t j = 0; j < pkg->version_count; j++) {
                if (strcmp(pkg->versions[j].version, version) == 0) {
                    return pkg->versions[j].hash;
                }
            }
            return NULL;  /* Package found but version not present */
        }
    }
    return NULL;  /* Package not found */
}

bool mirror_manifest_has_hash(MirrorManifest *m, const char *hash) {
    if (!m || !hash) return false;

    for (size_t i = 0; i < m->package_count; i++) {
        MirrorPackageEntry *pkg = &m->packages[i];
        for (size_t j = 0; j < pkg->version_count; j++) {
            if (pkg->versions[j].hash && strcmp(pkg->versions[j].hash, hash) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool mirror_manifest_set_generated(MirrorManifest *m, const char *timestamp) {
    if (!m || !timestamp) return false;
    if (m->generated) arena_free(m->generated);
    m->generated = arena_strdup(timestamp);
    return m->generated != NULL;
}

bool mirror_manifest_set_source(MirrorManifest *m, const char *source) {
    if (!m || !source) return false;
    if (m->source) arena_free(m->source);
    m->source = arena_strdup(source);
    return m->source != NULL;
}

bool mirror_manifest_write_json(MirrorManifest *m, const char *path) {
    if (!m || !path) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    /* Add metadata */
    if (m->generated) {
        cJSON_AddStringToObject(root, "generated", m->generated);
    }
    if (m->source) {
        cJSON_AddStringToObject(root, "source", m->source);
    }

    /* Add packages object */
    cJSON *packages = cJSON_CreateObject();
    if (!packages) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddItemToObject(root, "packages", packages);

    for (size_t i = 0; i < m->package_count; i++) {
        MirrorPackageEntry *pkg = &m->packages[i];

        /* Create package key "author/name" */
        char pkg_key[MAX_PACKAGE_NAME_LENGTH];
        snprintf(pkg_key, sizeof(pkg_key), "%s/%s", pkg->author, pkg->name);

        cJSON *pkg_versions = cJSON_CreateObject();
        if (!pkg_versions) {
            cJSON_Delete(root);
            return false;
        }
        cJSON_AddItemToObject(packages, pkg_key, pkg_versions);

        for (size_t j = 0; j < pkg->version_count; j++) {
            MirrorVersionEntry *ver = &pkg->versions[j];

            cJSON *ver_obj = cJSON_CreateObject();
            if (!ver_obj) {
                cJSON_Delete(root);
                return false;
            }
            cJSON_AddItemToObject(pkg_versions, ver->version, ver_obj);

            cJSON_AddStringToObject(ver_obj, "hash", ver->hash);
            if (ver->url) {
                cJSON_AddStringToObject(ver_obj, "url", ver->url);
            }
        }
    }

    /* Write to file */
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) return false;

    size_t len = strlen(json_str);
    bool success = file_write_bytes_atomic(path, json_str, len);
    arena_free(json_str);

    return success;
}

MirrorManifest* mirror_manifest_load_json(const char *path) {
    if (!path) return NULL;

    char *json_str = file_read_contents_bounded(path, MAX_LARGE_BUFFER_LENGTH * 64, NULL);
    if (!json_str) return NULL;

    cJSON *root = cJSON_Parse(json_str);
    arena_free(json_str);

    if (!root) return NULL;

    MirrorManifest *m = mirror_manifest_create();
    if (!m) {
        cJSON_Delete(root);
        return NULL;
    }

    /* Parse metadata */
    cJSON *generated = cJSON_GetObjectItem(root, "generated");
    if (generated && cJSON_IsString(generated)) {
        mirror_manifest_set_generated(m, generated->valuestring);
    }

    cJSON *source = cJSON_GetObjectItem(root, "source");
    if (source && cJSON_IsString(source)) {
        mirror_manifest_set_source(m, source->valuestring);
    }

    /* Parse packages */
    cJSON *packages = cJSON_GetObjectItem(root, "packages");
    if (packages && cJSON_IsObject(packages)) {
        cJSON *pkg_obj = NULL;
        cJSON_ArrayForEach(pkg_obj, packages) {
            const char *pkg_key = pkg_obj->string;
            if (!pkg_key) continue;

            /* Parse "author/name" */
            char *slash = strchr(pkg_key, '/');
            if (!slash) continue;

            size_t author_len = (size_t)(slash - pkg_key);
            char *author = arena_malloc(author_len + 1);
            if (!author) continue;
            memcpy(author, pkg_key, author_len);
            author[author_len] = '\0';

            const char *name = slash + 1;

            /* Parse versions */
            cJSON *ver_obj = NULL;
            cJSON_ArrayForEach(ver_obj, pkg_obj) {
                const char *version = ver_obj->string;
                if (!version || !cJSON_IsObject(ver_obj)) continue;

                cJSON *hash_obj = cJSON_GetObjectItem(ver_obj, "hash");
                cJSON *url_obj = cJSON_GetObjectItem(ver_obj, "url");

                if (hash_obj && cJSON_IsString(hash_obj)) {
                    const char *hash = hash_obj->valuestring;
                    const char *url = (url_obj && cJSON_IsString(url_obj)) ?
                                      url_obj->valuestring : NULL;
                    mirror_manifest_add(m, author, name, version, hash, url);
                }
            }

            arena_free(author);
        }
    }

    cJSON_Delete(root);
    return m;
}
