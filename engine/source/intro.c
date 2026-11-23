#include "intro.h"

#include <malloc.h>
#include <math.h>
#include <string.h>

#include "render.h"
#include "text.h"

/* the two views are copied out of the frame buffer at half size (box filtered) and combined on the CPU,
 * pixel by pixel, the way Source's intro_screenspaceeffect shader does on the GPU */
#define MAXW 320
#define MAXH 264

static u16 *texScene, *texView, *texOut;
static GXTexObj outObj;
static u16 tw, th;

static int alloc_textures(u16 w, u16 h)
{
	if (texScene && tw == w && th == h)
		return 0;
	free(texScene);
	free(texView);
	free(texOut);
	u32 bytes = (u32)w * h * 2;
	texScene = memalign(32, bytes);
	texView = memalign(32, bytes);
	texOut = memalign(32, bytes);
	if (!texScene || !texView || !texOut)
		return -1;
	tw = w;
	th = h;
	return 0;
}

static void copy_efb(u16 *dst, u16 fbW, u16 efbH)
{
	GX_SetTexCopySrc(0, 0, fbW, efbH);
	GX_SetTexCopyDst(fbW / 2, efbH / 2, GX_TF_RGB565, GX_TRUE); /* half size, box filtered */
	DCInvalidateRange(dst, (u32)tw * th * 2);
	GX_CopyTex(dst, GX_TRUE);
	GX_PixModeSync();
}

/* ---- per pixel, 0..255 channels (shader formulas from IntroScreenSpaceEffect_ps2x.fxc) */
static inline void unpack(u16 p, s32 *r, s32 *g, s32 *b)
{
	*r = ((p >> 11) & 31) * 255 / 31;
	*g = ((p >> 5) & 63) * 255 / 63;
	*b = (p & 31) * 255 / 31;
}

static inline s32 clamp255(s32 v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline s32 blend_ramp(s32 lum)
{
	/* modes 3/4: 0..0.4 ramps up, 0.4..0.7 is 1, 0.7..1 ramps down */
	if (lum < 102)
		return lum * 255 / 102;
	if (lum > 178)
		return 255 - (lum - 178) * 255 / 77;
	return 255;
}

static void mode_pixel(int mode, s32 sr, s32 sg, s32 sb, s32 gr, s32 gg, s32 gb, s32 *r, s32 *g, s32 *b)
{
	s32 sl = (sr + sg + sb) / 3, gl = (gr + gg + gb) / 3;
	switch (mode) {
	case 0: /* negative greyscale of the scene times the view */
		*r = (255 - sl) * gr / 255;
		*g = (255 - sl) * gg / 255;
		*b = (255 - sl) * gb / 255;
		break;
	case 1:
		if (gl < 77) {
			*r = 255 - gr;
			*g = 255 - gg;
			*b = 255 - gb;
		} else {
			*r = (255 - gr) * sl / 255;
			*g = (255 - gg) * sl / 255;
			*b = (255 - gb) * sl / 255;
		}
		break;
	case 2:
		*r = gl < sr ? gl : sr;
		*g = gl < sg ? gl : sg;
		*b = gl < sb ? gl : sb;
		break;
	case 3: {
		s32 k = blend_ramp(gl);
		*r = (gl * (255 - k) + sr * k) / 255;
		*g = (gl * (255 - k) + sg * k) / 255;
		*b = (gl * (255 - k) + sb * k) / 255;
		break;
	}
	case 4: {
		s32 k = blend_ramp(gl);
		*r = (gr * (255 - k) + sr * k) / 255;
		*g = (gg * (255 - k) + sg * k) / 255;
		*b = (gb * (255 - k) + sb * k) / 255;
		break;
	}
	case 5:
		if (sr > 0) {
			*r = sr;
			*g = sg;
			*b = sb;
		} else {
			/* HSV: value scaled by 1 + (v - .5), saturation by 1 - (v - .5) */
			s32 v = gr > gg ? (gr > gb ? gr : gb) : (gg > gb ? gg : gb);
			f32 bl = v / 255.0f - 0.5f, m = 1 + bl, k = 1 - bl;
			*r = (s32)(m * (v - k * (v - gr)));
			*g = (s32)(m * (v - k * (v - gg)));
			*b = (s32)(m * (v - k * (v - gb)));
		}
		break;
	case 6:
		*r = sr + gr;
		*g = sg + gg;
		*b = sb + gb;
		break;
	case 7:
		*r = sr;
		*g = sg;
		*b = sb;
		break;
	case 8:
		*r = gr;
		*g = gg;
		*b = gb;
		break;
	default: { /* 9 */
		s32 l1 = (sr + sg + sb) / 3;
		s32 k = 255 - l1 * 2;
		k = k < 0 ? 0 : k;
		*r = sr * sr * 2 / 255 + gr * k / 255;
		*g = sg * sg * 2 / 255 + gg * k / 255;
		*b = sb * sb * 2 / 255 + gb * k / 255;
		break;
	}
	}
}

static void compose(const IntroView *iv)
{
	u32 n = (u32)tw * th;
	s32 a0 = (s32)(iv->alpha[0] * 256), a1 = iv->passes > 1 ? (s32)(iv->alpha[1] * 256) : 0;
	for (u32 i = 0; i < n; i++) {
		s32 sr, sg, sb, gr, gg, gb, r, g, b, r2, g2, b2;
		unpack(texScene[i], &sr, &sg, &sb);
		unpack(texView[i], &gr, &gg, &gb);
		mode_pixel(iv->mode[0], sr, sg, sb, gr, gg, gb, &r, &g, &b);
		/* each pass is added with its alpha (SRC_ALPHA, ONE) over black */
		r = clamp255(r) * a0 >> 8;
		g = clamp255(g) * a0 >> 8;
		b = clamp255(b) * a0 >> 8;
		if (a1) {
			mode_pixel(iv->mode[1], sr, sg, sb, gr, gg, gb, &r2, &g2, &b2);
			r += clamp255(r2) * a1 >> 8;
			g += clamp255(g2) * a1 >> 8;
			b += clamp255(b2) * a1 >> 8;
		}
		r = clamp255(r);
		g = clamp255(g);
		b = clamp255(b);
		texOut[i] = (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
	}
}

/* ---- the combination on the GPU. The two views are copied to RGB565 textures and combined in TEV
 * stages, the way intro_screenspaceeffect does it in a pixel shader. Luminance is Source's (r+g+b)/3,
 * summed from the texture's red, green and blue through TEV swap tables (an I8 EFB copy would give
 * the video Y instead: 16..235 and green-heavy, which shifted modes 1, 3 and 5's thresholds and let
 * a black scene through mode 5). Maps: 0 the camera's view ("scene"), 1 the normal view ("gman"). */

static GXTexObj tScene, tView;

static void copy_view(u16 *rgb, u16 fbW, u16 efbH)
{
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetTexCopySrc(0, 0, fbW, efbH);
	GX_SetTexCopyDst(fbW / 2, efbH / 2, GX_TF_RGB565, GX_TRUE); /* half size, box filtered */
	GX_CopyTex(rgb, GX_TRUE); /* and clear for what comes next */
	GX_PixModeSync();
}

static u8 cin(u8 reg) /* a register as a colour input */
{
	return reg == GX_TEVREG0 ? GX_CC_C0 : reg == GX_TEVREG1 ? GX_CC_C1 : reg == GX_TEVREG2 ? GX_CC_C2 : GX_CC_CPREV;
}

/* one stage: reg = d + lerp(a, b, c) (op, scale, clamped); map < 0 for none */
static void cstage(u8 st, int map, u8 a, u8 b, u8 c, u8 d, u8 op, u8 scale, u8 reg)
{
	if (map >= 0)
		GX_SetTevOrder(st, GX_TEXCOORD0, (u32)map, GX_COLOR0A0);
	else
		GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevSwapMode(st, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GX_SetTevColorIn(st, a, b, c, d);
	GX_SetTevColorOp(st, op, GX_TB_ZERO, scale, GX_TRUE, reg);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKColorSel(st, GX_TEV_KCSEL_K0);
}

/* reg = (r + g + b) / 3 of a map: each channel spread to all three by swap tables 1-3, x 1/3 (K3) */
static u8 lum_stages(u8 st, int map, u8 reg)
{
	for (int k = 0; k < 3; k++) {
		cstage(st + k, map, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, k ? cin(reg) : GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1, reg);
		GX_SetTevSwapMode(st + k, GX_TEV_SWAP0, GX_TEV_SWAP1 + k);
		GX_SetTevKColorSel(st + k, GX_TEV_KCSEL_K3);
	}
	return st + 3;
}

/* modes 3/4's ramp k(l) from l in C0: up over 0..0.4, 1 to 0.7, down to 1; leaves k in PREV */
static u8 ramp_stages(u8 st)
{
	cstage(st, -1, GX_CC_ZERO, GX_CC_C0, GX_CC_KONST, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_4, GX_TEVREG1); /* 2.5 l */
	GX_SetTevKColorSel(st, GX_TEV_KCSEL_K1);
	cstage(st + 1, -1, GX_CC_KONST, GX_CC_ZERO, GX_CC_C0, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_4, GX_TEVREG2); /* 3.33 (1 - l) */
	GX_SetTevKColorSel(st + 1, GX_TEV_KCSEL_K2);
	cstage(st + 2, -1, GX_CC_ZERO, GX_CC_C1, GX_CC_C2, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV); /* up x down */
	return st + 3;
}

/* sets up TEV for one blend mode (IntroScreenSpaceEffect_ps2x.fxc) */
static void mode_tev(int mode)
{
	u8 n = 0;
	switch (mode) {
	case 0: /* (1 - lum(scene)) * gman */
		n = lum_stages(0, GX_TEXMAP0, GX_TEVREG0);
		cstage(n++, GX_TEXMAP1, GX_CC_TEXC, GX_CC_ZERO, GX_CC_C0, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	case 1: /* lum(gman) < 0.3 ? 1 - gman : (1 - gman) * lum(scene) */
		n = lum_stages(0, GX_TEXMAP1, GX_TEVREG0);
		n = lum_stages(n, GX_TEXMAP0, GX_TEVREG1);
		cstage(n++, -1, GX_CC_KONST, GX_CC_C0, GX_CC_ONE, GX_CC_ZERO, GX_TEV_COMP_R8_GT, GX_CS_SCALE_1,
		       GX_TEVREG2); /* dark = 0.3 > lum(gman) */
		cstage(n++, -1, GX_CC_C1, GX_CC_ONE, GX_CC_C2, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1,
		       GX_TEVREG1); /* x = dark ? 1 : lum(scene) */
		cstage(n++, GX_TEXMAP1, GX_CC_C1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1,
		       GX_TEVPREV); /* x * (1 - gman) */
		break;
	case 2: /* min(lum(gman), scene) per channel */
		n = lum_stages(0, GX_TEXMAP1, GX_TEVREG0);
		cstage(n++, GX_TEXMAP0, GX_CC_C0, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO, GX_TEV_COMP_RGB8_GT, GX_CS_SCALE_1,
		       GX_TEVREG1); /* lum > scene */
		cstage(n++, GX_TEXMAP0, GX_CC_C0, GX_CC_TEXC, GX_CC_C1, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	case 3: /* lerp(lum(gman), scene, k(lum(gman))) */
	case 4: /* lerp(gman, scene, k(lum(gman))) */
		n = lum_stages(0, GX_TEXMAP1, GX_TEVREG0);
		n = ramp_stages(n);
		if (mode == 4)
			cstage(n++, GX_TEXMAP1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVREG0);
		cstage(n++, GX_TEXMAP0, GX_CC_C0, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	case 5: /* the scene where its red isn't 0, the G-Man's view elsewhere */
		cstage(n++, GX_TEXMAP0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ONE, GX_CC_ZERO, GX_TEV_COMP_R8_GT, GX_CS_SCALE_1,
		       GX_TEVREG0);
		cstage(n++, GX_TEXMAP1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVREG1);
		cstage(n++, GX_TEXMAP0, GX_CC_C1, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	case 6: /* scene + gman */
		cstage(n++, GX_TEXMAP0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		cstage(n++, GX_TEXMAP1, GX_CC_CPREV, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	case 7: /* scene */
		cstage(n++, GX_TEXMAP0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	case 8: /* gman */
		cstage(n++, GX_TEXMAP1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	default: /* 9: saturate(2 scene^2) + gman * saturate(1 - 2 lum(scene)) */
		n = lum_stages(0, GX_TEXMAP0, GX_TEVREG0);
		cstage(n, -1, GX_CC_ONE, GX_CC_ZERO, GX_CC_C0, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_2, GX_TEVREG0);
		GX_SetTevColorOp(n++, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_2, GX_TRUE, GX_TEVREG0); /* (1-l-.5)*2 */
		cstage(n++, GX_TEXMAP1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVREG1);
		cstage(n++, GX_TEXMAP0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_TEXC, GX_CC_ZERO, GX_TEV_ADD, GX_CS_SCALE_2, GX_TEVPREV);
		cstage(n++, -1, GX_CC_CPREV, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C1, GX_TEV_ADD, GX_CS_SCALE_1, GX_TEVPREV);
		break;
	}
	GX_SetNumTevStages(n);
}

int Intro_Render(const IntroView *iv, const Pack *pack, World *world, const Camera *cam, Mtx view, f32 proj[4][4],
                 u16 fbW, u16 efbH, RenderStats *st)
{
	u16 w = fbW / 2, h = efbH / 2;
	if (w > MAXW || h > MAXH || alloc_textures(w, h) != 0)
		return 0;

	/* the camera entity's view (Source: drawn only if the script set one; black otherwise) */
	if (iv->hasCamera) {
		Camera cc = {0};
		cc.origin = (guVector){iv->camOrigin[0], iv->camOrigin[1], iv->camOrigin[2]};
		cc.pitch = iv->camAngles[0];
		cc.yaw = iv->camAngles[1];
		cc.fovx = iv->fov > 0 ? iv->fov : cam->fovx;
		if (cc.fovx > 170)
			cc.fovx = 170;
		cc.fovy = 2.0f * atanf(tanf(cc.fovx * (f32)M_PI / 360.0f) * 0.75f) * 180.0f / (f32)M_PI;
		Camera_Update(&cc);
		Mtx cv;
		Mtx44 cp;
		guVector ct = {cc.origin.x + cc.forward.x, cc.origin.y + cc.forward.y, cc.origin.z + cc.forward.z};
		guLookAt(cv, &cc.origin, &cc.up, &ct);
		guPerspective(cp, cc.fovy, 4.0f / 3.0f, 4.0f, 16384.0f);
		GX_LoadProjectionMtx(cp, GX_PERSPECTIVE);
		World_Collect(world, &cc, 0);
		Render_Frame(pack, world, &cc, cv, st);
	}
	/* (without a camera this copies the cleared, black frame) */
	copy_view(texScene, fbW, efbH);
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);

	/* the normal view (the cutscene camera on the G-Man) */
	World_Collect(world, cam, 0);
	Render_Frame(pack, world, cam, view, st);
	copy_view(texView, fbW, efbH);

	GX_InitTexObj(&tScene, texScene, w, h, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&tView, texView, w, h, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InvalidateTexAll();

	/* each pass is added with its alpha over black (SRC_ALPHA, ONE), as Source draws them */
	Text_Begin(fbW, efbH);
	GX_LoadTexObj(&tScene, GX_TEXMAP0);
	GX_LoadTexObj(&tView, GX_TEXMAP1);
	GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
	GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
	GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
	GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){77, 77, 77, 255});    /* mode 1's 0.3 */
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){160, 160, 160, 255}); /* x4: 2.5 = 1 / 0.4 */
	GX_SetTevKColor(GX_KCOLOR2, (GXColor){213, 213, 213, 255}); /* x4: 3.33 = 1 / 0.3 */
	GX_SetTevKColor(GX_KCOLOR3, (GXColor){85, 85, 85, 255});    /* 1/3 */
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
	for (int pass = 0; pass < iv->passes && pass < 2; pass++) {
		f32 a = iv->alpha[pass];
		if (a <= 0.001f)
			continue;
		mode_tev(iv->mode[pass]);
		u8 a8 = (u8)(a >= 1 ? 255 : a * 255);
		GX_Begin(GX_QUADS, GX_VTXFMT4, 4);
		GX_Position2f32(0, 0);
		GX_Color4u8(255, 255, 255, a8);
		GX_TexCoord2f32(0, 0);
		GX_Position2f32(fbW, 0);
		GX_Color4u8(255, 255, 255, a8);
		GX_TexCoord2f32(1, 0);
		GX_Position2f32(fbW, efbH);
		GX_Color4u8(255, 255, 255, a8);
		GX_TexCoord2f32(1, 1);
		GX_Position2f32(0, efbH);
		GX_Color4u8(255, 255, 255, a8);
		GX_TexCoord2f32(0, 1);
		GX_End();
	}
	/* back to what the 2D code expects */
	for (int k = 0; k < 16; k++)
		GX_SetTevSwapMode(k, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	Text_End(proj);
	(void)compose;
	return 1;
}

void Intro_Init(u16 fbW, u16 efbH)
{
	u16 w = fbW / 2, h = efbH / 2;
	if (w <= MAXW && h <= MAXH)
		alloc_textures(w, h);
}
