/* File Explorer: the state every part of the app shares (see explorer.h). */
#include "explorer.h"

Window *EX_WIN;

int EX_CUR = VFS_ROOT;
int EX_ITEMS[VFS_MAX_NODES];
int EX_NITEMS;
int EX_SEL = -1;
int EX_SCROLL;
int EX_HIST[HIST_MAX];
int EX_HIST_N;
int EX_FWD[HIST_MAX];
int EX_FWD_N;
const char *EX_NOTE;
int EX_SIDE_W = SIDE_W_DEFAULT;
int EX_SIDE_DRAG;
int EX_LAST_ROW = -1;
unsigned long long EX_LAST_CLICK_MS;
int EX_HOVER_ROW = -1;
int EX_HOVER_PLACE = -1;
int EX_SORT_KEY = VFS_SORT_NAME;
int EX_SORT_DESC;
int EX_TIP_BTN = -1;
unsigned long long EX_TIP_SINCE;
int EX_TIP_SHOWN;
int EX_CLIP_ID = -1;
int EX_CLIP_CUT;
int EX_RENAME_ID = -1;
int EX_ADDR_EDIT;
int EX_HOVER_CRUMB = -1;
Menu EX_MENU;
