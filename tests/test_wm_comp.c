#include "ui.h"
#include "wm.h"
#include "wm_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
/* Host-side tests for windows as compositor surfaces (kernel/window/wm_comp.c), with a pretend GPU driver that only
 * records what it is asked: which surfaces exist, which parts of them are uploaded, and whether as opaque pixels. */
static unsigned long long NOW;
unsigned long long timer_ms(void){return NOW;}
unsigned long long timer_us(void){return NOW * 1000;}
int timer_ready(void){return 0;}
void timer_init(void){}
void *kmalloc(unsigned long long n){ return malloc((size_t)n); }
void kfree(void *p){ free(p); }
void *kcalloc(unsigned long long n, unsigned long long s){ return calloc((size_t)n, (size_t)s); }
void *krealloc(void *p, unsigned long long n){ return realloc(p, (size_t)n); }

static int NEXT_ID = 1, LIVE, FREED_ID;
static int SW[100], SH[100];
static int UPLOADS, UP_ID, UP_X, UP_Y, UP_W, UP_H, UP_OPAQUE;
static int s_new(int w, int h){ int id = NEXT_ID++; SW[id] = w; SH[id] = h; LIVE++; return id; }
static void s_free(int id){ FREED_ID = id; LIVE--; }
static int s_upload(int id, const void *px, int pitch, int x, int y, int w, int h, int opaque)
{
  (void)px; (void)pitch;
  assert(x >= 0 && y >= 0 && x + w <= SW[id] && y + h <= SH[id]);
  UPLOADS++; UP_ID = id; UP_X = x; UP_Y = y; UP_W = w; UP_H = h; UP_OPAQUE = opaque;
  return 0;
}
static int s_compose(const GfxCompItem *it, int n, const int *r, int nr){ (void)it; (void)n; (void)r; (void)nr; return 0; }
static const GfxCompOps OPS = { s_new, s_free, s_upload, s_compose, 0 };

static UiRect LAST_BODY; /* the body rectangle the window was last drawn with: apps remember such places for the clicks */
static void draw(Window*w,UiRect b){ LAST_BODY = b; ui_text(b.x+16,b.y+16,"Hello",UI_TEXT_HEADING); (void)w; }

int main(void){
  int W=1024,H=640; unsigned*fb=calloc(W*H,4);
  unsigned long long bytes = (unsigned long long)W*H*4;
  void *a = malloc(bytes), *b = malloc(bytes);
  GfxCompItem items[8];
  UiRect dmg[WM_DAMAGE_MAX];
  ui_set_theme(&ui_theme_dark); gfx_init(fb,W,H,W,1);
  int M = 48, R = ui_theme()->radius_l + 2;

  Window*w=wm_create("One",200,100,400,300,draw,0);
  wm_comp_set(1);
  assert(wm_comp_on());
  /* first time: a surface of the window and its shadow margin, drawn whole as a packed layer */
  assert(wm_comp_update(&OPS,a,b,bytes)==0);
  assert(LIVE==1 && SW[1]==400+2*M && SH[1]==300+2*M);
  assert(LAST_BODY.x==200+1 && LAST_BODY.y>100);          /* drawn into the surface, but in screen coordinates */
  assert(gfx_width()==(unsigned)W && gfx_height()==(unsigned)H);
  { unsigned cf, cp; wm_comp_counts(&cf,&cp); assert(cf==1 && cp==0); wm_comp_counts(&cf,&cp); assert(cf==0 && cp==0); }
  assert(UPLOADS==1 && UP_ID==1 && UP_X==0 && UP_Y==0 && UP_W==400+2*M && UP_H==300+2*M && !UP_OPAQUE);
  assert(wm_comp_items(items,8)==1 && items[0].surface==1 && items[0].x==200-M && items[0].y==100-M && items[0].layer==1);
  /* nothing changed: nothing is drawn */
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==1);
  /* moved by its title bar: no drawing, only the place */
  wm_pointer(350,115,1); wm_pointer(410,155,1); wm_pointer(410,155,0);
  assert(w->r.x==260 && w->r.y==140);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==1);
  assert(wm_comp_items(items,8)==1 && items[0].x==260-M && items[0].y==140-M);
  wm_take_damage_rects(dmg,WM_DAMAGE_MAX);
  /* the app changed a part inside its opaque body: one pass, only that part, as opaque pixels */
  wm_add_damage(ui_rect(w->r.x+50,w->r.y+80,100,40));
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==2);
  assert(UP_OPAQUE && UP_X==50+M && UP_Y==80+M && UP_W==100 && UP_H==40);
  assert(LAST_BODY.x==w->r.x+1);                             /* the part was drawn where the window is now */
  { unsigned cf, cp; wm_comp_counts(&cf,&cp); assert(cf==0 && cp==1); }
  /* a part reaching the bottom-left corner: still one pass; only its opaque pieces go up (the middle, the left strip) */
  wm_add_damage(ui_rect(w->r.x,w->r.y+w->r.h-60,60,60));
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==4 && UP_OPAQUE);
  assert(UP_X==1+M && UP_Y==w->r.h-60+M && UP_W==R-1 && UP_H==60-R);   /* the left strip, above the corner */
  { unsigned cf, cp; wm_comp_counts(&cf,&cp); assert(cf==0 && cp==1); }
  /* the taskbar or a menu changed (screen only): no window is drawn */
  wm_add_screen_damage(ui_rect(w->r.x,w->r.y,w->r.w,w->r.h));
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==4);
  /* a second window: new surface; the first one loses the focus, so it is drawn again too */
  Window*w2=wm_create("Two",500,300,300,200,draw,0);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && LIVE==2 && UPLOADS==6);
  assert(wm_comp_items(items,8)==2 && items[1].surface==2);   /* back to front */
  /* raising the first one: both change their look (focus) */
  wm_raise(w);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==8);
  assert(wm_comp_items(items,8)==2 && items[1].surface==1);
  /* closing the second: its surface is freed */
  wm_close(w2);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && LIVE==1 && FREED_ID==2);
  { unsigned cf, cp; wm_comp_counts(&cf,&cp); assert(cf==4 && cp==0); }  /* two windows drawn whole twice (a new one; the focus) */
  int up = UPLOADS;
  /* an app asks for everything again: its look is the same, so one pass and the three opaque pieces, no shadow */
  wm_invalidate();
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==up+3 && UP_OPAQUE);
  assert(UP_X==w->r.w-R+M && UP_Y==R+M && UP_W==R-1 && UP_H==w->r.h-2*R);   /* the right strip */
  { unsigned cf, cp; wm_comp_counts(&cf,&cp); assert(cf==0 && cp==1); }
  /* resized: a new surface of the new size */
  wm_pointer(w->r.x+w->r.w-2,w->r.y+w->r.h-2,1); wm_pointer(w->r.x+w->r.w+38,w->r.y+w->r.h+18,1); wm_pointer(0,0,0);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && LIVE==1 && SW[NEXT_ID-1]==w->r.w+2*M && SH[NEXT_ID-1]==w->r.h+2*M);
  /* drawing buffers too small for the window: the caller must draw without the compositor */
  wm_invalidate();
  assert(wm_comp_update(&OPS,a,b,1000)==-1);
  /* off: content changes are not tracked; on again: everything is drawn once */
  wm_comp_drop(&OPS);
  assert(LIVE==0 && !wm_comp_on());
  wm_comp_set(1);
  up = UPLOADS;
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==up+1 && LIVE==1);
  puts("wm compositor tests passed");
  return 0;
}
