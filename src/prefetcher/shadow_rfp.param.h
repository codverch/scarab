#ifndef __SHADOW_RFP_PARAM_H__
#define __SHADOW_RFP_PARAM_H__

#include "globals/global_types.h"

#define DEF_PARAM(name, variable, type, func, def, const) extern const type variable;
#include "shadow_rfp.param.def"
#undef DEF_PARAM

#endif
