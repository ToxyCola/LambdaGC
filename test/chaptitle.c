/* Host test: chapter titles. Each chapter's first map has an env_message "chapter_title" (from worldspawn
 * "chaptertitle") that shows itself once, a second in, with the localized name and titles.txt's
 * write-out effect; Trainstation 1's own env_message resolves too. Prints the text length and effect only.
 * usage: chaptitle map.hgm (one map per run) */
#define main sim_main
#include "sim.c"
#undef main
#include "ent.h"
static void cl(const char *m, const char *l) { (void)m; (void)l; }
static Player pl;
extern int stubHudMessages;
int main(int argc, char **argv)
{
	int bad = 0;
	for (int a = 1; a < argc; a++) {
		Ent_Free();
		if (load(&P, argv[a])) { printf("%s: missing\n", argv[a]); bad++; continue; }
		CM_Init(&P);
		Player_Spawn(&pl, P.spawnOrigin, 0, P.spawnAngles[1]);
		EntCallbacks cb = {cl};
		Ent_Spawn(&P, &pl, 1, &cb);
		u32 n; Entity *E = Ent_List(&n);
		s32 t = -1;
		for (u32 i = 1; i < n; i++)
			if (E[i].cls == EC_ENV_MESSAGE && (Ent_KV((s32)i, "_auto") || (Ent_KV((s32)i, "message") && !strncasecmp(Ent_KV((s32)i, "message"), "CHAPTER", 7))))
				t = (s32)i;
		if (t < 0) { printf("%s: no chapter title\n", argv[a]); continue; }
		const char *txt = Ent_KV(t, "_text"), *fx = Ent_KV(t, "_fx");
		int before = stubHudMessages;
		UserCmd c = {0};
		for (int k = 0; k < 66 * 2; k++) { Player_Tick(&pl, &c); Ent_Tick(TICK_INTERVAL); }
		int shown = stubHudMessages - before;
		int unresolved = !txt || !strncasecmp(txt, "CHAPTER", 7) || txt[0] == '#';
		printf("%s: title %s (%d characters), effect %s, shown %d time(s) in the first 2 s%s\n", argv[a],
		       unresolved ? "UNRESOLVED" : "resolved", txt ? (int)strlen(txt) : 0, fx ? fx : "-", shown,
		       Ent_KV(t, "_auto") ? " (worldspawn)" : " (map's env_message)");
		bad += unresolved || (Ent_KV(t, "_auto") && shown != 1);
	}
	printf("%d bad\n", bad);
	return bad != 0;
}
