#ifndef BUILDINFO_H
#define BUILDINFO_H

/* Build metadata - generated at compile time by buildinfo.mk */
extern const char *build_base_version;
extern const char *build_full_version;
extern const char *build_commit_short;
extern const char *build_commit_full;
extern const char *build_timestamp;
extern const char *build_dirty;
extern const char *build_host;
extern const char *build_user;
extern const char *build_os;
extern const char *build_arch;
extern const char *build_compiler;

/* Structured metadata in custom ELF/Mach-O section */
extern const char build_metadata[];

/* Helper function to print detailed version info */
void print_version_info(void);

/* SBOM metadata - generated at compile time by buildinfo.mk */
extern const char *sbom_package_name;
extern const char *sbom_spdx_license;
extern const char *sbom_supplier;
extern const char *sbom_homepage;
extern const char *sbom_dependencies;

/* SBOM metadata in custom ELF/Mach-O section */
extern const char sbom_metadata[];

/* Helper function to print SBOM info (summary) */
void print_sbom_info(void);

/* Helper function to print full SBOM document */
void print_sbom_full(void);

/* Environment variable defaults (from ENV_DEFAULTS file at build time) */
extern const char *env_default_registry_v2_full_index_url;
extern const char *env_default_repository_local_path;

#endif /* BUILDINFO_H */
