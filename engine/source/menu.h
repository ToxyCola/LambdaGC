/*
 * hl2gc main menu, laid out like the PC version's: the chapter's background map behind it, the game
 * title in the game's logo font, and the menu items (labels from the install's gameui strings).
 * Laid out and animated like the original Xbox release's menu (hl2OGXboxMenu.swf): the grey panel
 * slides in and flickers, the focused item gets the orange bar and ">", Options moves the panel
 * left into GAME / CONTROLLER / BUTTONS, a legend bar shows the GameCube's A / B, with the Xbox
 * menu's sounds and looping music. The SWF's static background is not used: the live map shows.
 * New Game opens chapter select with the chapter thumbnails; Options has captions, reverse look,
 * look sensitivity and volume. In game it also has Cheats: no clipping, god mode, infinite ammo, all weapons. Start in game opens it as the pause menu.
 */
#ifndef HL2GC_MENU_H
#define HL2GC_MENU_H

#include <gctypes.h>

enum { MA_NONE, MA_RESUME, MA_NEWGAME, MA_QUIT, MA_GIVEALL };

/* remappable actions; each is played as the button it has in the default layout (BIND_DEFAULT) */
enum { BIND_JUMP, BIND_SPRINT, BIND_USE, BIND_RELOAD, BIND_CROUCH, BIND_FIRE, BIND_ALTFIRE, BIND_LIGHT, NUM_BINDS };

typedef struct {
	int captions, reverseLook;
	f32 sensitivity; /* 0.5 .. 2 */
	int volume;      /* 0 .. 10 */
	u32 bind[NUM_BINDS]; /* the physical button (PAD_*) for each action */
} Options;

/* the pad's buttons as the game reads them: each action's button moved to where the default layout has it */
u32 Options_Remap(u32 buttons);
void Options_DefaultBinds(void);

extern Options gOptions;

int Menu_Load(u8 *data, u32 size);
/* the original Xbox menu's sounds and music (xmenu.bin, built from hl2OGXboxMenu.swf) */
int Menu_LoadXbox(u8 *data, u32 size);
void Menu_Open(int inGame);
void Menu_Close(void);
/* a message box like the PC's error dialogs (title, wrapped text, OK). game: shown over a level in
 * progress (OK returns to it) rather than over the main menu. */
void Menu_ShowError(int game, const char *title, const char *text);
int Menu_IsOpen(void);
/* Feeds one frame of input (PAD_ButtonsDown, stick); returns an action. For MA_NEWGAME, *map is the
 * chapter's first map. */
int Menu_Update(u32 down, s8 stickY, f32 dt, const char **map);
void Menu_Draw(u16 width, u16 height);
/* background map for the menu (chapterbackgrounds.txt), for the chapter reached so far */
const char *Menu_Background(int chapter);

#endif
