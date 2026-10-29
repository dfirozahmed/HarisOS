#include "ui.h"
#include "wm.h"
#include "wm_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static unsigned CORNER_PX;
static int LOG_X[16], LOG_Y[16], LOG_W[16], LOG_H[16], LOG_ID[16]; /* the last uploads */
static int uploaded(int id, int x, int y, int w, int h)
{
  for (int k = 0; k < 16; k++)
    if (LOG_ID[k] == id && LOG_X[k] == x && LOG_Y[k] == y && LOG_W[k] == w && LOG_H[k] == h) return 1;
  return 0;
}
static int s_new(int w, int h){ int id = NEXT_ID++; SW[id] = w; SH[id] = h; LIVE++; return id; }
static void s_free(int id){ FREED_ID = id; LIVE--; }
static int s_upload(int id, const void *px, int pitch, int x, int y, int w, int h, int opaque)
{
  (void)px; (void)pitch;
  assert(x >= 0 && y >= 0 && x + w <= SW[id] && y + h <= SH[id]);
  UPLOADS++; UP_ID = id; UP_X = x; UP_Y = y; UP_W = w; UP_H = h; UP_OPAQUE = opaque;
  if (x == 0 && y == 0 && !opaque) CORNER_PX = ((const unsigned *)px)[0]; /* a whole layer: its top-left corner pixel */
  LOG_X[UPLOADS % 16] = x; LOG_Y[UPLOADS % 16] = y; LOG_W[UPLOADS % 16] = w; LOG_H[UPLOADS % 16] = h; LOG_ID[UPLOADS % 16] = id;
  return 0;
}
static int s_compose(const GfxCompItem *it, int n, const int *r, int nr){ (void)it; (void)n; (void)r; (void)nr; return 0; }
static int SCROLLS, SC_ID, SC_X, SC_Y, SC_W, SC_H, SC_DY;
static int s_scroll(int id, int x, int y, int w, int h, int dy)
{
  assert(x >= 0 && y >= 0 && x + w <= SW[id] && y + h <= SH[id]);
  SCROLLS++; SC_ID = id; SC_X = x; SC_Y = y; SC_W = w; SC_H = h; SC_DY = dy;
  return 0;
}
static const GfxCompOps OPS = { s_new, s_free, s_upload, s_compose, s_scroll, 0, 0 };

static UiRect LAST_BODY; /* the body rectangle the window was last drawn with: apps remember such places for the clicks */
static void draw(Window*w,UiRect b){ LAST_BODY = b; ui_text(b.x+16,b.y+16,"Hello",UI_TEXT_HEADING); (void)w; }
static void draw_edit(Window*w,UiRect b){ ui_edit_draw(ui_rect(b.x+10,b.y+10,120,30),&w->edits[0],"Type",0); }
/* a list that scrolls like File Explorer's: rows moved (gfx_scroll_rect), then the new rows and the scrollbar marked */
static int on_wheel_scroll(Window*w,int steps){
  int sx=w->r.x+20, sy=w->r.y+60; (void)steps;
  if (gfx_scroll_rect(sx,sy,90,80,-16)!=0) return 1;
  wm_add_damage(ui_rect(sx,sy+80-16,90,16)); wm_add_damage(ui_rect(sx+90,sy,10,80));
  return 2;
}

int main(void){
  int W=1024,H=640; unsigned*fb=calloc(W*H,4);
  unsigned long long bytes = (unsigned long long)W*H*4;
  void *a = malloc(bytes), *b = malloc(bytes);
  GfxCompItem items[8];
  UiRect dmg[WM_DAMAGE_MAX];
  ui_set_theme(&ui_theme_dark); gfx_init(fb,W,H,W,1);
  int M = 0, R = ui_theme()->radius_l + 2;   /* M: the layer has no margin any more (the GPU draws the shadow) */

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
  assert(wm_comp_items(items,8)==2 && items[1].surface==1 && items[1].x==200 && items[1].y==100 && items[1].layer==1);
  assert((CORNER_PX >> 24) == 255 && (CORNER_PX & 0xFFFFFFu) == 0);   /* outside the round corner: see-through, no shadow baked in */
  { int blur, dy, st; ui_window_shadow(1, &blur, &dy, &st);   /* its shadow first: drawn by the GPU, from the same numbers as ui_window */
    assert(items[0].layer==2 && items[0].surface==blur && items[0].alpha==st && items[0].x==200-blur/2 && items[0].y==100+dy-blur/2 &&
           items[0].w==400+blur && items[0].h==300+blur); }
  /* nothing changed: nothing is drawn */
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==1);
  /* moved by its title bar: no drawing, only the place */
  wm_pointer(350,115,1); wm_pointer(410,155,1); wm_pointer(410,155,0);
  assert(w->r.x==260 && w->r.y==140);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==1);
  assert(wm_comp_items(items,8)==2 && items[1].x==260 && items[1].y==140);
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
  assert(UP_X==1+M && UP_Y==w->r.h-60+M && UP_W==R-1 && UP_H==60-(R-1));   /* the left strip, above the corner */
  { unsigned cf, cp; wm_comp_counts(&cf,&cp); assert(cf==0 && cp==1); }
  /* the taskbar or a menu changed (screen only): no window is drawn */
  wm_add_screen_damage(ui_rect(w->r.x,w->r.y,w->r.w,w->r.h));
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==4);
  /* a second window: new surface; the first one loses the focus, so it is drawn again too */
  Window*w2=wm_create("Two",500,300,300,200,draw,0);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && LIVE==2 && UPLOADS==6);
  assert(wm_comp_items(items,8)==4 && items[1].surface==1 && items[3].surface==2 && items[2].layer==2);   /* back to front, each after its shadow */
  /* raising the first one: both change their look (focus) */
  wm_raise(w);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==8);
  assert(wm_comp_items(items,8)==4 && items[3].surface==1);
  /* closing the second: its surface is freed */
  wm_close(w2);
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && LIVE==1 && FREED_ID==2);
  { unsigned cf, cp; wm_comp_counts(&cf,&cp); assert(cf==4 && cp==0); }  /* two windows drawn whole twice (a new one; the focus) */
  int up = UPLOADS;
  /* an app asks for everything again: its look is the same, so one pass and the three opaque pieces, no shadow */
  wm_invalidate();
  assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==up+3 && UP_OPAQUE);
  assert(UP_X==w->r.w-R+M && UP_Y==(R-1)+M && UP_W==R-1 && UP_H==w->r.h-2*(R-1));   /* the right strip */
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
  /* the report: where the requests came from, and which window took the drawing */
  { unsigned why[WM_WHY_COUNT]; WmBusy bz[3];
    wm_comp_why(why); wm_comp_busiest(bz,3);                      /* start from 0 */
    wm_add_damage(ui_rect(w->r.x+50,w->r.y+80,10,10)); wm_invalidate();
    wm_comp_why(why);
    assert(why[WM_WHY_AREA]==1 && why[WM_WHY_INVALIDATE]==1 && why[WM_WHY_KEY]==0 && why[WM_WHY_TICK]==0);
    assert(strcmp(wm_why_name(WM_WHY_TICK),"tick")==0 && strcmp(wm_why_name(99),"?")==0);
    assert(wm_comp_update(&OPS,a,b,bytes)==0);
    assert(wm_comp_busiest(bz,3)==1 && strcmp(bz[0].title,"One")==0 && bz[0].redraws==1 &&
           bz[0].pixels==(unsigned long long)w->r.w*w->r.h);
    /* and by reason: the app's 10 x 10 part, then everything (invalidate); its look did not change */
    assert(bz[0].why_n[WM_WHY_AREA]==1 && bz[0].why_px[WM_WHY_AREA]==100);
    assert(bz[0].why_n[WM_WHY_INVALIDATE]==1 && bz[0].why_px[WM_WHY_INVALIDATE]==(unsigned long long)w->r.w*w->r.h);
    assert(bz[0].why_n[WM_WHY_KEY]==0 && bz[0].why_n[WM_WHY_COUNT]==0 && strcmp(wm_why_name(WM_WHY_COUNT),"look")==0);
    assert(wm_comp_busiest(bz,3)==0);                              /* taken: 0 again */
  }
  /* moved without being drawn: where its text box and its buttons are moves with it (clicks land there) */
  { Window*e=wm_create("Edit",100,120,300,200,draw_edit,0);
    e->edit_count=1;
    int bi=wm_add_button(e,"OK",UI_BTN_PRIMARY,ui_rect(20,120,80,30),0,0);
    assert(bi>=0);
    assert(wm_comp_update(&OPS,a,b,bytes)==0);
    UiRect f0=e->edits[0].field, b0=e->btn[bi].r;
    assert(f0.w==120 && f0.x==e->r.x+1+10);
    int up2=UPLOADS;
    wm_pointer(e->r.x+150,e->r.y+15,1); wm_pointer(e->r.x+150+70,e->r.y+15+30,1); wm_pointer(e->r.x+150,e->r.y+15,0);
    assert(e->r.x==170 && e->r.y==150);
    assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==up2);   /* not drawn */
    assert(e->edits[0].field.x==f0.x+70 && e->edits[0].field.y==f0.y+30);
    assert(e->btn[bi].r.x==b0.x+70 && e->btn[bi].r.y==b0.y+30);
  }
  /* scrolling inside a window: the GPU moves the rows in its surface; only the screen is marked */
  { Window*t=wm_top();
    assert(wm_comp_update(&OPS,a,b,bytes)==0);
    wm_take_damage_rects(dmg,WM_DAMAGE_MAX);
    int sx=t->r.x+20, sy=t->r.y+60, up=UPLOADS;
    gfx_backbuffer_detach(1); gfx_scroll_hook(wm_comp_scroll);
    assert(gfx_scroll_rect(sx,sy,100,80,-16)==0 && SCROLLS==1);
    assert(SC_X==20+M && SC_Y==60+M && SC_W==100 && SC_H==80 && SC_DY==-16 && UPLOADS==up);
    assert(wm_take_damage_rects(dmg,WM_DAMAGE_MAX)==1 && dmg[0].x==sx && dmg[0].y==sy && dmg[0].w==100 && dmg[0].h==80);
    /* the window then marks the new rows and its scrollbar: two parts, drawn and uploaded each on its own */
    unsigned cf, cp; wm_comp_counts(&cf,&cp);
    wm_add_damage(ui_rect(sx,sy+80-16,90,16)); wm_add_damage(ui_rect(sx+90,sy,10,80));
    assert(wm_comp_update(&OPS,a,b,bytes)==0);
    wm_comp_counts(&cf,&cp);
    assert(UPLOADS-up==5 && cf==0 && cp==2);   /* from outside a handler: every window there (the one under it too) */
    /* from the window's own wheel handler (as File Explorer does it): only that window, its two parts */
    t->on_wheel=on_wheel_scroll;
    assert(wm_comp_update(&OPS,a,b,bytes)==0); up=UPLOADS;
    wm_wheel(sx+30,sy+30,1);
    assert(SCROLLS==2 && SC_DY==-16 && SC_W==90);
    assert(wm_comp_update(&OPS,a,b,bytes)==0 && UPLOADS==up+2 && UP_W==10 && UP_H==80);
    wm_comp_counts(&cf,&cp); assert(cf==0 && cp==1);
    /* a part changed but not drawn yet where the rows are (a hover highlight): its old pixels moved with the rows, so it is
     * drawn where it was and where they went */
    wm_add_damage(ui_rect(sx+5,sy+40,10,10));
    assert(gfx_scroll_rect(sx,sy,100,80,-16)==0 && SCROLLS==3);
    memset(LOG_ID,0,sizeof LOG_ID);
    assert(wm_comp_update(&OPS,a,b,bytes)==0);
    { int n=wm_comp_items(items,8), s=items[n-1].surface;   /* the top window */
      assert(uploaded(s, 25+M, 100+M, 10, 10) && uploaded(s, 25+M, 100-16+M, 10, 10)); }
    /* nothing there: no window holds the rectangle */
    assert(gfx_scroll_rect(1000,630,10,5,2)==-1 && SCROLLS==3);
    gfx_scroll_hook(0); gfx_backbuffer_detach(0);
  }
  /* the pointer on a title-bar button (hover look): one pass over the title bar, not the whole layer twice */
  { Window*t=wm_top(); unsigned cf, cp;
    wm_pointer(0,0,0);
    assert(wm_comp_update(&OPS,a,b,bytes)==0); wm_comp_counts(&cf,&cp);
    int up=UPLOADS, tb=ui_theme()->titlebar_h;
    wm_pointer(t->r.x+t->r.w-22, t->r.y+tb/2, 0);   /* on the close button */
    assert(wm_comp_update(&OPS,a,b,bytes)==0); wm_comp_counts(&cf,&cp);
    assert(cf==0 && cp==1 && UPLOADS>up && UP_OPAQUE);
    assert(UP_Y>=M && UP_Y+UP_H<=M+tb);                       /* inside the title bar */
    wm_pointer(0,0,0);                                         /* off it again: the same */
    assert(wm_comp_update(&OPS,a,b,bytes)==0); wm_comp_counts(&cf,&cp);
    assert(cf==0 && cp==1);
  }
  /* "look" split by what changed: a new window; moved past the screen's left edge it is not drawn again */
  { WmBusy bz[3]; wm_comp_busiest(bz,3);
    Window*n=wm_create("Look",300,200,200,150,draw,0);
    assert(wm_comp_update(&OPS,a,b,bytes)==0);
    int nb=wm_comp_busiest(bz,3), k=0;
    for (; k<nb && strcmp(bz[k].title,"Look"); k++) {}
    assert(k<nb && bz[k].look_n[WM_LOOK_NEW]==1 && bz[k].look_n[WM_LOOK_SIZE]==0);
    int up3=UPLOADS;
    wm_pointer(n->r.x+100,n->r.y+15,1); wm_pointer(n->r.x+100-280,n->r.y+15,1); wm_pointer(0,0,0);   /* x 20: its shadow past the left edge */
    assert(n->r.x==20);
    assert(wm_comp_update(&OPS,a,b,bytes)==0);
    nb=wm_comp_busiest(bz,3);
    for (k=0; k<nb && strcmp(bz[k].title,"Look"); k++) {}
    assert(UPLOADS==up3 && (k==nb || bz[k].look_n[WM_LOOK_OTHER]==0));   /* past the left edge: not drawn again (the layer holds it whole) */
    assert(strcmp(wm_look_name(WM_LOOK_SIZE),"size")==0 && strcmp(wm_look_name(42),"?")==0);
  }
  puts("wm compositor tests passed");
  return 0;
}
