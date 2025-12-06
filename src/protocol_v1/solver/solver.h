#ifndef PROTOCOL_V1_SOLVER_H
#define PROTOCOL_V1_SOLVER_H

#include "../../solver.h"
#include "../../elm_json.h"
#include "../../pgsolver/solver_common.h"
#include "../../pgsolver/pg_elm.h"
#include <stdbool.h>

/* V1 Protocol: Run solver with a specific strategy */
SolverResult run_with_strategy_v1(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    bool is_test_dependency,
    bool upgrade_all,
    SolverStrategy strategy,
    PackageMap *current_packages,
    InstallPlan **out_plan
);

/* V1 Protocol: Upgrade all packages */
SolverResult solver_upgrade_all_v1(
    SolverState *state,
    const ElmJson *elm_json,
    bool major_upgrade,
    InstallPlan **out_plan
);

#endif /* PROTOCOL_V1_SOLVER_H */
