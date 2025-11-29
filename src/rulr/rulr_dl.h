#ifndef RULR_DL_H
#define RULR_DL_H

#include "rulr.h"

RulrError rulr_load_dl_file(Rulr *r, const char *path);
RulrError rulr_load_dl_files(Rulr *r, const char *rule_path, const char *fact_path);

#endif /* RULR_DL_H */
