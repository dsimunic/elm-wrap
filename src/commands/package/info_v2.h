#ifndef INFO_V2_H
#define INFO_V2_H

#include "../../install_env.h"
#include "../../protocol_v2/solver/v2_registry.h"

int cmd_info_v2(const char *author, const char *name, const char *version_arg, InstallEnv *env, V2PackageEntry *entry);

#endif /* INFO_V2_H */