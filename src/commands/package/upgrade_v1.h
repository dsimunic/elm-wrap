#ifndef UPGRADE_V1_H
#define UPGRADE_V1_H

#include "../../install_env.h"
#include "../../elm_json.h"

int upgrade_single_package_v1(const char *package, ElmJson *elm_json, InstallEnv *env, bool major_upgrade, bool major_ignore_test, bool auto_yes);

#endif /* UPGRADE_V1_H */