#ifndef PROTOCOL_V2_SOLVER_H
#define PROTOCOL_V2_SOLVER_H

#include "../../solver.h"
#include "../../elm_json.h"
#include "../../pgsolver/solver_common.h"
#include "pg_elm_v2.h"
#include <stdbool.h>

/* V2 Protocol: Run solver with a specific strategy */
SolverResult run_with_strategy_v2(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    const Version *target_version,
    bool is_test_dependency,
    bool upgrade_all,
    SolverStrategy strategy,
    PackageMap *current_packages,
    InstallPlan **out_plan
);

/* V2 Protocol: Upgrade all packages */
SolverResult solver_upgrade_all_v2(
    SolverState *state,
    const ElmJson *elm_json,
    bool major_upgrade,
    InstallPlan **out_plan
);

#endif /* PROTOCOL_V2_SOLVER_H */
