/*
 * hl2gc apploader: the small loader the GameCube boot ROM (or Dolphin) runs from a disc.
 * It reads the main.dol named in the disc header and returns its entry point.
 * Loaded at 0x81200000; built with no libraries and no small-data/FPU use.
 */
typedef unsigned int u32;
typedef void (*report_t)(const char *, ...);

static u32 state, section, dolOffset;
static u32 discInfo[8] __attribute__((aligned(32)));  /* disc header bytes 0x420..0x43F */
static u32 dolHeader[64] __attribute__((aligned(32))); /* DOL header (0x100 bytes) */

static void ap_init(report_t report)
{
	(void)report;
	state = 0;
	section = 0;
}

/* Called repeatedly: fill in one read request and return 1, or return 0 when done. */
static int ap_main(void **dst, u32 *size, u32 *offset)
{
	if (state == 0) {
		*dst = discInfo;
		*size = 32;
		*offset = 0x420;
		state = 1;
		return 1;
	}
	if (state == 1) {
		dolOffset = discInfo[0];
		*dst = dolHeader;
		*size = 0x100;
		*offset = dolOffset;
		state = 2;
		return 1;
	}
	/* DOL header: section file offsets [0..17], load addresses [18..35], sizes [36..53] */
	while (section < 18) {
		u32 i = section++;
		u32 sz = dolHeader[36 + i];
		if (!sz)
			continue;
		*dst = (void *)dolHeader[18 + i];
		*size = (sz + 31) & ~31u;
		*offset = dolOffset + dolHeader[i];
		return 1;
	}
	return 0;
}

static void *ap_close(void)
{
	return (void *)dolHeader[56]; /* entry point */
}

void apploader_entry(void (**init)(report_t), int (**main)(void **, u32 *, u32 *), void *(**close)(void))
{
	*init = ap_init;
	*main = ap_main;
	*close = ap_close;
}
