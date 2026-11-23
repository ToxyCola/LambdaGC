"""Builds test/gxv/inc: a copy of libogc's gc/ headers whose vertex writes (wgPipe->...) call the
validator's hooks, and whose cache functions are host no-ops. usage: make_inc.py <libogc>/gc"""
import os
import re
import shutil
import sys

src, here = sys.argv[1], os.path.dirname(os.path.abspath(__file__))
dst = os.path.join(here, "inc")
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(src, dst)
p = os.path.join(dst, "ogc", "gx.h")
s = open(p).read()
s = re.sub(r"wgPipe->(U8|S8|U16|S16|U32|S32|F32)\s*=\s*([^;]+);", lambda m: f"gxv_w_{m.group(1)}({m.group(2)});", s)
decl = ("\nvoid gxv_w_U8(unsigned char); void gxv_w_S8(signed char); void gxv_w_U16(unsigned short);"
        " void gxv_w_S16(short);\nvoid gxv_w_U32(unsigned int); void gxv_w_S32(int); void gxv_w_F32(float);\n")
s = s.replace('#include "gu.h"\n', '#include "gu.h"\n' + decl, 1)
open(p, "w").write(s)
open(os.path.join(dst, "ogc", "cache.h"), "w").write("""#ifndef __OGC_CACHE_H__
#define __OGC_CACHE_H__
#include <gctypes.h>
static inline void DCFlushRange(void *p, u32 n) { (void)p; (void)n; }
static inline void DCInvalidateRange(void *p, u32 n) { (void)p; (void)n; }
static inline void DCStoreRange(void *p, u32 n) { (void)p; (void)n; }
static inline void DCFlushRangeNoSync(void *p, u32 n) { (void)p; (void)n; }
static inline void ICInvalidateRange(void *p, u32 n) { (void)p; (void)n; }
#endif
""")
print("wrote", dst)
