/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __HYBRIDGP_H__
#define __HYBRIDGP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bp/bp.h"

/*************Interface to Scarab***************/
void bp_hybridgp_init(void);
void bp_hybridgp_timestamp(Op*);
uns8 bp_hybridgp_pred(Op*);
void bp_hybridgp_spec_update(Op*);
void bp_hybridgp_update(Op*);
void bp_hybridgp_retire(Op*);
void bp_hybridgp_recover(Recovery_Info*);
uns8 bp_hybridgp_full(uns);

#ifdef __cplusplus
}
#endif

#endif  // __HYBRIDGP_H__
