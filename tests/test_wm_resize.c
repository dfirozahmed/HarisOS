#include "ui.h"
#include "wm.h"
#include "taskbar.h"
#include "wm_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
/* Host-side tests for window resizing (kernel/window): edges, corners, minimum size, screen limits. */
unsigned long long timer_ms(void){return 0;}
int timer_ready(void){return 0;}
void timer_init(void){}
static void draw(Window*w,UiRect b){ ui_text(b.x+16,b.y+16,"Resizable window",UI_TEXT_HEADING); (void)w; }
static void ptr(int x,int y,unsigned b){ wm_pointer(x,y,b); }
static int wheel_steps, wheel_ret;
static int on_wheel_cb(Window*w,int steps){ (void)w; wheel_steps+=steps; return wheel_ret; }
static int hov_calls, hov_x, hov_y, rp_calls, rp_x;
static int on_hover_cb(Window*w,int x,int y){ (void)w; hov_calls++; hov_x=x; hov_y=y; return 0; }
static int on_rpress_cb(Window*w,int x,int y){ (void)w; (void)y; rp_calls++; rp_x=x; return 0; }
int main(void){
  int W=1024,H=640; unsigned*fb=calloc(W*H,4);
  ui_set_theme(&ui_theme_dark); gfx_init(fb,W,H,W,1);
  Window*w=wm_create("Test",200,100,400,300,draw,0);
  assert(w->min_w==300 && w->min_h==225);
  /* hover hints */
  assert(wm_resize_hint(200+400-2,100+300-2)==(WM_EDGE_RIGHT|WM_EDGE_BOTTOM));
  assert(wm_resize_hint(201,100+150)==WM_EDGE_LEFT);
  assert(wm_resize_hint(200+200,100+299)==WM_EDGE_BOTTOM);
  assert(wm_resize_hint(200+200,100+150)==0);
  assert(wm_resize_hint(5,5)==0);
  assert(wm_resize_hint(200+3,100+3)==(WM_EDGE_LEFT|WM_EDGE_TOP));
  assert(wm_resize_hint(200+399-12,100+3)==(WM_EDGE_TOP|WM_EDGE_RIGHT));
  /* drag bottom-right corner */
  ptr(598,398,1); assert(wm_dragging());
  ptr(698,448,1); assert(w->r.x==200&&w->r.y==100&&w->r.w==500&&w->r.h==350);
  ptr(698,448,0); assert(!wm_dragging());
  /* drag left edge to the right too far: min width, right edge stays */
  ptr(201,300,1); ptr(900,300,1);
  assert(w->r.w==w->min_w && w->r.x+w->r.w==700);
  ptr(900,300,0);
  /* top edge up: grows, bottom stays */
  int bottom=w->r.y+w->r.h; ptr(w->r.x+100,w->r.y+2,1); ptr(w->r.x+100,w->r.y-50,1);
  assert(w->r.y+w->r.h==bottom && w->r.y==48);
  ptr(w->r.x+100,w->r.y-50,0);
  /* drag beyond the screen: clamped */
  ptr(w->r.x+w->r.w-2,w->r.y+w->r.h-2,1); ptr(5000,5000,1);
  assert(w->r.x+w->r.w<=W && w->r.y+w->r.h<=H-TASKBAR_HEIGHT);
  ptr(5000,5000,0);
  /* fixed size and dialogs do not resize */
  wm_set_resizable(w,0); assert(wm_resize_hint(w->r.x+w->r.w-2,w->r.y+w->r.h-2)==0); wm_set_resizable(w,1);
  /* maximized: no resize edges */
  wm_set_maximized(w,1); assert(wm_resize_hint(W-2,H-TASKBAR_HEIGHT-2)==0); wm_set_maximized(w,0);
  /* title-bar buttons: hover and pressed feedback (and nothing is closed when released elsewhere) */
  {
    int tb = ui_theme()->titlebar_h, cy = w->r.y + tb / 2;
    ptr(w->r.x + 22, cy, 0);
    assert(wm_titlebtn_hover(w) == 0 && wm_titlebtn_pressed(w) == -1);
    ptr(w->r.x + 42, cy, 0);
    assert(wm_titlebtn_hover(w) == 1);
    ptr(w->r.x + 62, cy, 0);
    assert(wm_titlebtn_hover(w) == 2);
    ptr(w->r.x + 62, cy, 1);
    assert(wm_titlebtn_pressed(w) == 2 && wm_titlebtn_hover(w) == 2);
    ptr(w->r.x + 200, cy + 100, 1); /* dragged away: still held, no longer hovered */
    assert(wm_titlebtn_hover(w) == -1);
    ptr(w->r.x + 200, cy + 100, 0);
    assert(wm_titlebtn_pressed(w) == -1 && !w->maximized);
    ptr(w->r.x + 120, cy, 0);
    assert(wm_titlebtn_hover(w) == -1);
  }
  /* title drag still works when pressing in the middle of the title bar */
  int ox=w->r.x; ptr(w->r.x+200,w->r.y+20,1); ptr(w->r.x+240,w->r.y+20,1); ptr(w->r.x+240,w->r.y+20,0);
  assert(w->r.x==ox+40);
  /* mouse wheel: goes to the window under the pointer, without focusing it */
  {
    wm_pointer(5000,5000,0);
    w->r = ui_rect(300,200,400,300); wm_layout_internal(w);
    Window*other=wm_create("Other",20,20,100,100,draw,0); /* on top and focused, away from w */
    wheel_steps=0; wheel_ret=1;
    w->on_wheel=on_wheel_cb;
    assert(wm_top()==other);
    assert(wm_wheel(w->r.x+50,w->r.y+100,2)!=0);
    assert(wheel_steps==2 && wm_top()==other);              /* delivered, focus unchanged */
    wheel_steps=0; wm_wheel(5000,5000,3); assert(wheel_steps==0);   /* nothing under the pointer */
    wm_wheel(other->r.x+10,other->r.y+10,3); assert(wheel_steps==0); /* other has no on_wheel */
    wm_wheel(w->r.x+50,w->r.y+100,0); assert(wheel_steps==0);        /* zero steps ignored */
    wheel_ret=0; wm_take_damage(&(UiRect){0}); /* clear damage */
    wm_wheel(w->r.x+50,w->r.y+100,-1);
    assert(wheel_steps==-1 && !WM_HAS_DMG);                 /* handler said "no change": no redraw */
    wm_close(other);
  }
  /* hover and right press */
  {
    wm_pointer(5000,5000,0);
    w->r = ui_rect(300,200,400,300); wm_layout_internal(w);
    Window*other=wm_create("Other",20,20,100,100,draw,0);
    hov_calls=0; rp_calls=0; w->on_hover=on_hover_cb; w->on_rpress=on_rpress_cb;
    wm_pointer(w->r.x+50,w->r.y+100,0);
    assert(hov_calls==1 && hov_x==w->r.x+50);                /* pointer entered */
    wm_pointer(w->r.x+60,w->r.y+100,0);
    assert(hov_calls==2 && hov_x==w->r.x+60);                /* moved inside */
    wm_pointer(5000,5000,0);
    assert(hov_calls==3 && hov_x==-1 && hov_y==-1);          /* left: told once */
    wm_pointer(5001,5000,0);
    assert(hov_calls==3);                                    /* not told again */
    wm_pointer(w->r.x+50,w->r.y+100,0);
    int c=hov_calls; wm_pointer(w->r.x+50,w->r.y+100,2);     /* right button down inside */
    assert(rp_calls==1 && rp_x==w->r.x+50 && wm_top()==w);   /* delivered, window raised */
    wm_pointer(w->r.x+50,w->r.y+100,2); assert(rp_calls==1); /* held: no repeat */
    wm_pointer(w->r.x+50,w->r.y+100,0);
    wm_pointer(w->r.x+50,w->r.y+10,2); assert(rp_calls==1);  /* title bar: not delivered */
    wm_pointer(w->r.x+50,w->r.y+10,0);
    (void)c;
    wm_close(other);
  }
  printf("wm resize tests passed\n");
  return 0;
}
