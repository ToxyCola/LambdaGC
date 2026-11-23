/*
 * gxv: a host-side GX validator. The engine's GX calls are compiled against a copy of libogc's
 * headers whose vertex writes call gxv_w_*; this file keeps the vertex descriptor, attribute
 * formats, arrays and position matrices the way the GPU would, parses every vertex sent between
 * GX_Begin and the next command exactly like the hardware's command processor, and checks:
 *   - each draw sends exactly count * vertex-size bytes (a mismatch desynchronises the FIFO)
 *   - indexed attributes have an array set
 *   - every vertex, once transformed by its position matrix, is finite and within range
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include "gxv.h"

enum { A_PNMTX = 0, A_POS = 9, A_NRM = 10, A_CLR0 = 11, A_CLR1 = 12, A_TEX0 = 13, A_MAX = 21 };

static u8 vcd[A_MAX];                 /* GX_NONE / GX_DIRECT / GX_INDEX8 / GX_INDEX16 */
static struct { u32 cnt, type; u8 frac; } vat[8][A_MAX];
static struct { const u8 *base; u32 stride; } arr[A_MAX];
static f32 posMtx[64][3][4];
static u8 posMtxSet[64];
static u32 curMtx;

/* the draw in progress */
static int inDraw, drawFmt;
static u32 drawCount, drawVerts, vtxBytes, vtxPos, drawPrim;
static u8 gxv_vbuf[256];
static struct { int attr; u32 off, size; } layout[A_MAX];
static int nLayout;
static void *drawCaller;

GxvStats gxv;
const char *gxvTag = "";

static void report(const char *what, void *caller)
{
	gxv.errors++;
	if (gxv.errors <= 20)
		fprintf(stderr, "gxv [%s] %s (draw from %p, prim 0x%x fmt %d count %u)\n", gxvTag, what, caller, drawPrim,
		        drawFmt, drawCount);
}

static u32 comp_size(u32 type)
{
	switch (type) {
	case GX_U8: case GX_S8: return 1;
	case GX_U16: case GX_S16: return 2;
	default: return 4;
	}
}

static u32 attr_direct_size(int fmt, int a)
{
	if (a == A_PNMTX || (a >= 1 && a <= 8))
		return 1;
	if (a == A_POS)
		return (vat[fmt][a].cnt == GX_POS_XY ? 2 : 3) * comp_size(vat[fmt][a].type);
	if (a == A_NRM)
		return 3 * comp_size(vat[fmt][a].type);
	if (a == A_CLR0 || a == A_CLR1) {
		switch (vat[fmt][a].type) {
		case GX_RGB565: case GX_RGBA4: return 2;
		case GX_RGB8: case GX_RGBA6: return 3;
		default: return 4;
		}
	}
	/* tex */
	return (vat[fmt][a].cnt == GX_TEX_S ? 1 : 2) * comp_size(vat[fmt][a].type);
}

static void finish_check(const char *why)
{
	if (!inDraw)
		return;
	if (drawVerts != drawCount || vtxPos != 0) {
		char m[160];
		snprintf(m, sizeof(m), "%s: draw got %u of %u vertices (+%u bytes)", why, drawVerts, drawCount, vtxPos);
		report(m, drawCaller);
	}
	inDraw = 0;
}

void gxv_state_call(void) { finish_check("state change inside a draw"); }

static f32 read_comp(const u8 *p, u32 type, u8 frac)
{
	switch (type) {
	case GX_U8: return p[0] / (f32)(1 << frac);
	case GX_S8: return (s8)p[0] / (f32)(1 << frac);
	case GX_U16: return *(const u16 *)p / (f32)(1 << frac);
	case GX_S16: return *(const s16 *)p / (f32)(1 << frac);
	default: return *(const f32 *)p;
	}
}

static void vertex_done(void)
{
	/* matrix */
	u32 m = curMtx;
	f32 pos[3] = {0, 0, 0};
	int havePos = 0;
	for (int i = 0; i < nLayout; i++) {
		int a = layout[i].attr;
		const u8 *d = gxv_vbuf + layout[i].off;
		if (a == A_PNMTX)
			m = d[0];
		if (a != A_POS)
			continue;
		u32 cnt = vat[drawFmt][a].cnt == GX_POS_XY ? 2 : 3;
		u32 type = vat[drawFmt][a].type;
		u8 frac = vat[drawFmt][a].frac;
		const u8 *src;
		if (vcd[a] == GX_DIRECT) {
			src = d;
		} else {
			u32 idx = vcd[a] == GX_INDEX8 ? d[0] : *(const u16 *)d;
			if (!arr[a].base) {
				report("indexed position without an array", drawCaller);
				return;
			}
			src = arr[a].base + (size_t)idx * arr[a].stride;
		}
		for (u32 k = 0; k < cnt; k++)
			pos[k] = read_comp(src + k * comp_size(type), type, frac);
		havePos = 1;
	}
	if (!havePos)
		return;
	if (m >= 64 || !posMtxSet[m]) {
		report("vertex uses a position matrix that was never loaded", drawCaller);
		return;
	}
	f32 e[3];
	for (int r = 0; r < 3; r++)
		e[r] = posMtx[m][r][0] * pos[0] + posMtx[m][r][1] * pos[1] + posMtx[m][r][2] * pos[2] + posMtx[m][r][3];
	gxv.vertices++;
	f32 dist = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
	if (!isfinite(dist) || dist > gxv.maxDist) {
		gxv.farVerts++;
		if (gxv.farVerts <= 10) {
			char msg[160];
			snprintf(msg, sizeof(msg), "vertex %.0f units from the eye (model pos %.1f %.1f %.1f, mtx %u, vertex %u, %s)", dist, pos[0],
			         pos[1], pos[2], m, drawVerts, vcd[A_POS] == GX_DIRECT ? "direct" : "indexed");
			report(msg, drawCaller);
		}
	}
}

static void put(const void *src, u32 n)
{
	if (!inDraw) {
		gxv.strayBytes += n;
		if (gxv.strayBytes == n)
			report("vertex data outside any draw", __builtin_return_address(1));
		return;
	}
	if (drawVerts >= drawCount) {
		gxv.overflow += n;
		if (gxv.overflow == n)
			report("more vertex data than the draw's count", drawCaller);
		return;
	}
	memcpy(gxv_vbuf + vtxPos, src, n);
	vtxPos += n;
	if (vtxPos > vtxBytes) {
		report("attribute written past the vertex size (descriptor mismatch)", drawCaller);
		vtxPos = 0;
		drawVerts++;
		return;
	}
	if (vtxPos == vtxBytes) {
		vertex_done();
		vtxPos = 0;
		drawVerts++;
	}
}

/* host order in the buffer: reads use native types */
void gxv_w_U8(unsigned char v) { put(&v, 1); }
void gxv_w_S8(signed char v) { put(&v, 1); }
void gxv_w_U16(unsigned short v) { put(&v, 2); }
void gxv_w_S16(short v) { put(&v, 2); }
void gxv_w_U32(unsigned int v) { put(&v, 4); }
void gxv_w_S32(int v) { put(&v, 4); }
void gxv_w_F32(float v) { put(&v, 4); }

/* the hardware's attribute order */
static const int order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, A_POS, A_NRM, A_CLR0, A_CLR1, 13, 14, 15, 16, 17, 18, 19, 20};

void GX_Begin(u8 primitve, u8 vtxfmt, u16 vtxcnt)
{
	finish_check("new draw");
	drawCaller = __builtin_return_address(0);
	drawPrim = primitve;
	drawFmt = vtxfmt;
	drawCount = vtxcnt;
	drawVerts = 0;
	vtxPos = 0;
	nLayout = 0;
	u32 off = 0;
	for (int i = 0; i < A_MAX; i++) {
		int a = order[i];
		if (vcd[a] == GX_NONE)
			continue;
		u32 sz = vcd[a] == GX_DIRECT ? attr_direct_size(vtxfmt, a) : vcd[a] == GX_INDEX8 ? 1 : 2;
		if (vcd[a] != GX_DIRECT && !arr[a].base && a != A_PNMTX)
			report("indexed attribute without an array", drawCaller);
		layout[nLayout].attr = a;
		layout[nLayout].off = off;
		layout[nLayout].size = sz;
		nLayout++;
		off += sz;
	}
	vtxBytes = off;
	inDraw = vtxcnt > 0 && off > 0;
	gxv.draws++;
}

void gxv_end_frame(void) { finish_check("end of frame"); }

void GX_ClearVtxDesc(void)
{
	gxv_state_call();
	memset(vcd, 0, sizeof(vcd));
}

void GX_SetVtxDesc(u8 attr, u8 type)
{
	gxv_state_call();
	if (attr < A_MAX)
		vcd[attr] = type;
}

void GX_SetVtxAttrFmt(u8 vtxfmt, u32 vtxattr, u32 comptype, u32 compsize, u32 frac)
{
	gxv_state_call();
	if (vtxfmt < 8 && vtxattr < A_MAX) {
		vat[vtxfmt][vtxattr].cnt = comptype;
		vat[vtxfmt][vtxattr].type = compsize;
		vat[vtxfmt][vtxattr].frac = (u8)frac;
	}
}

void GX_SetArray(u32 attr, void *ptr, u8 stride)
{
	gxv_state_call();
	if (attr < A_MAX) {
		arr[attr].base = ptr;
		arr[attr].stride = stride;
	}
}

void GX_LoadPosMtxImm(Mtx mt, u32 pnidx)
{
	gxv_state_call();
	if (pnidx < 64) {
		memcpy(posMtx[pnidx], mt, sizeof(posMtx[0]));
		posMtxSet[pnidx] = 1;
	}
}

void GX_SetCurrentMtx(u32 mtx)
{
	gxv_state_call();
	curMtx = mtx;
}

/* everything else only has to end any draw in progress */
#define STUB(name, args) \
	void name args { gxv_state_call(); }
STUB(GX_CopyTex, (void *dest, u8 clear))
STUB(GX_SetTevSwapMode, (u8 tevstage, u8 ras_sel, u8 tex_sel))
STUB(GX_SetTevSwapModeTable, (u8 swapid, u8 r, u8 g, u8 b, u8 a))
STUB(GX_SetChanAmbColor, (s32 channel, GXColor color))
STUB(GX_SetChanMatColor, (s32 channel, GXColor color))
STUB(GX_LoadNrmMtxImm, (Mtx mt, u32 pnidx))
STUB(GX_InitLightPos, (GXLightObj *l, f32 x, f32 y, f32 z))
STUB(GX_InitLightColor, (GXLightObj *l, GXColor c))
STUB(GX_InitLightDir, (GXLightObj *l, f32 x, f32 y, f32 z))
STUB(GX_LoadLightObj, (const GXLightObj *l, u8 id))
STUB(GX_InitLightSpot, (GXLightObj *l, f32 cutoff, u8 fn))
STUB(GX_InitLightDistAttn, (GXLightObj *l, f32 ref_dist, f32 ref_brite, u8 dist_fn))
STUB(GX_InitLightAttn, (GXLightObj *l, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2))
STUB(GX_InvalidateTexAll, (void))
STUB(GX_LoadProjectionMtx, (Mtx44 mt, u8 type))
STUB(GX_LoadTexMtxImm, (Mtx mt, u32 texidx, u8 type))
STUB(GX_LoadTexObj, (GXTexObj * obj, u8 mapid))
STUB(GX_PixModeSync, (void))
STUB(GX_SetAlphaCompare, (u8 comp0, u8 ref0, u8 aop, u8 comp1, u8 ref1))
STUB(GX_SetBlendMode, (u8 type, u8 src_fact, u8 dst_fact, u8 op))
STUB(GX_SetChanCtrl, (s32 channel, u8 enable, u8 ambsrc, u8 matsrc, u8 litmask, u8 diff_fn, u8 attn_fn))
STUB(GX_SetColorUpdate, (u8 enable))
STUB(GX_SetCullMode, (u8 mode))
STUB(GX_SetFog, (u8 type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor col))
STUB(GX_SetIndTexCoordScale, (u8 indtexid, u8 scale_s, u8 scale_t))
STUB(GX_SetIndTexMatrix, (u8 indtexmtx, f32 offset_mtx[2][3], s8 scale_exp))
STUB(GX_SetIndTexOrder, (u8 indtexstage, u8 texcoord, u8 texmap))
STUB(GX_SetNumChans, (u8 num))
STUB(GX_SetNumIndStages, (u8 nstages))
STUB(GX_SetNumTevStages, (u8 num))
STUB(GX_SetNumTexGens, (u32 nr))
STUB(GX_SetScissor, (u32 xOrigin, u32 yOrigin, u32 wd, u32 ht))
STUB(GX_SetTevAlphaIn, (u8 tevstage, u8 a, u8 b, u8 c, u8 d))
STUB(GX_SetTevAlphaOp, (u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid))
STUB(GX_SetTevColor, (u8 tev_regid, GXColor color))
STUB(GX_SetTevColorIn, (u8 tevstage, u8 a, u8 b, u8 c, u8 d))
STUB(GX_SetTevColorOp, (u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid))
STUB(GX_SetTevDirect, (u8 tevstage))
STUB(GX_SetTevIndirect, (u8 tevstage, u8 indtexid, u8 format, u8 bias, u8 mtxid, u8 wrap_s, u8 wrap_t, u8 addprev,
                         u8 utclod, u8 a))
STUB(GX_SetTevKAlphaSel, (u8 tevstage, u8 sel))
STUB(GX_SetTevKColor, (u8 id, GXColor col))
STUB(GX_SetTevKColorSel, (u8 tevstage, u8 sel))
STUB(GX_SetTevOp, (u8 tevstage, u8 mode))
STUB(GX_SetTevOrder, (u8 tevstage, u8 texcoord, u32 texmap, u8 color))
STUB(GX_SetTexCoordGen, (u16 texcoord, u32 tgen_typ, u32 tgen_src, u32 mtxsrc))
STUB(GX_SetTexCopyDst, (u16 wd, u16 ht, u32 fmt, u8 mipmap))
STUB(GX_SetTexCopySrc, (u16 left, u16 top, u16 wd, u16 ht))
STUB(GX_SetViewport, (f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ))
STUB(GX_SetZCompLoc, (u8 before_tex))
STUB(GX_SetZMode, (u8 enable, u8 func, u8 update_enable))
void GX_InitTexObj(GXTexObj *obj, void *img_ptr, u16 wd, u16 ht, u8 fmt, u8 wrap_s, u8 wrap_t, u8 mipmap)
{
	(void)img_ptr; (void)wd; (void)ht; (void)fmt; (void)wrap_s; (void)wrap_t; (void)mipmap;
	memset(obj, 0, sizeof(*obj));
}
void GX_InitTexObjLOD(GXTexObj *obj, u8 minfilt, u8 magfilt, f32 minlod, f32 maxlod, f32 lodbias, u8 biasclamp,
                      u8 edgelod, u8 maxaniso)
{
	(void)obj; (void)minfilt; (void)magfilt; (void)minlod; (void)maxlod; (void)lodbias; (void)biasclamp;
	(void)edgelod; (void)maxaniso;
}

/* ---- gu math (libogc's C versions, which the GEKKO build maps to paired-single code) */
void ps_guMtxIdentity(Mtx mt)
{
	memset(mt, 0, sizeof(Mtx));
	mt[0][0] = mt[1][1] = mt[2][2] = 1;
}
void ps_guMtxCopy(const Mtx src, Mtx dst) { memmove(dst, src, sizeof(Mtx)); }
void ps_guMtxConcat(const Mtx a, const Mtx b, Mtx ab)
{
	Mtx t;
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++)
			t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0);
	}
	memcpy(ab, t, sizeof(Mtx));
}
void guPerspective(Mtx44 mt, f32 fovy, f32 aspect, f32 n, f32 f)
{
	f32 cot = 1.0f / tanf(fovy * 0.5f * (f32)M_PI / 180.0f), tmp = 1.0f / (f - n);
	memset(mt, 0, sizeof(Mtx44));
	mt[0][0] = cot / aspect;
	mt[1][1] = cot;
	mt[2][2] = -n * tmp;
	mt[2][3] = -(f * n) * tmp;
	mt[3][2] = -1;
}
void guOrtho(Mtx44 mt, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
	memset(mt, 0, sizeof(Mtx44));
	mt[0][0] = 2 / (r - l);
	mt[0][3] = -(r + l) / (r - l);
	mt[1][1] = 2 / (t - b);
	mt[1][3] = -(t + b) / (t - b);
	mt[2][2] = -1 / (f - n);
	mt[2][3] = -f / (f - n);
	mt[3][3] = 1;
}
void guLookAt(Mtx mt, const guVector *camPos, const guVector *camUp, const guVector *target)
{
	guVector look = {camPos->x - target->x, camPos->y - target->y, camPos->z - target->z};
	f32 l = sqrtf(look.x * look.x + look.y * look.y + look.z * look.z);
	look.x /= l; look.y /= l; look.z /= l;
	guVector right = {camUp->y * look.z - camUp->z * look.y, camUp->z * look.x - camUp->x * look.z,
	                  camUp->x * look.y - camUp->y * look.x};
	l = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
	right.x /= l; right.y /= l; right.z /= l;
	guVector up = {look.y * right.z - look.z * right.y, look.z * right.x - look.x * right.z,
	               look.x * right.y - look.y * right.x};
	mt[0][0] = right.x; mt[0][1] = right.y; mt[0][2] = right.z;
	mt[0][3] = -(camPos->x * right.x + camPos->y * right.y + camPos->z * right.z);
	mt[1][0] = up.x; mt[1][1] = up.y; mt[1][2] = up.z;
	mt[1][3] = -(camPos->x * up.x + camPos->y * up.y + camPos->z * up.z);
	mt[2][0] = look.x; mt[2][1] = look.y; mt[2][2] = look.z;
	mt[2][3] = -(camPos->x * look.x + camPos->y * look.y + camPos->z * look.z);
}
void GX_InvVtxCache(void) { gxv_state_call(); }
