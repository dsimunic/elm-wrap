#ifndef PROTOCOL_V2_INDEX_FETCH_H
#define PROTOCOL_V2_INDEX_FETCH_H

#include <stdbool.h>

/**
 * Download the V2 registry index file.
 *
 * The URL is formed as: <base_url>/index_<compiler>_<version>
 * where base_url comes from WRAP_REGISTRY_V2_FULL_INDEX_URL.
 *
 * The downloaded file is saved as "index.dat" in the repository directory.
 * Progress is reported if the download takes longer than 1 second.
 *
 * @param repo_path      The repository directory path to save index.dat into
 * @param compiler       Compiler name (e.g., "elm", "lamdera")
 * @param version        Compiler version (e.g., "0.19.1")
 * @return true on success, false on failure
 */
bool v2_index_fetch(const char *repo_path, const char *compiler, const char *version);

#endif /* PROTOCOL_V2_INDEX_FETCH_H */
