#include "ui.h"
#include "wm.h"
#include "taskbar.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
/* Host-side tests for window resizing (kernel/window): edges, corners, minimum size, screen limits. */
unsigned long long timer_ms(void){return 0;}
int timer_ready(void){return 0;}
void timer_init(void){}
static void draw(Window*w,UiRect b){ ui_text(b.x+16,b.y+16,"Resizable window",UI_TEXT_HEADING); (void)w; }
static void ptr(int x,int y,unsigned b){ wm_pointer(x,y,b); }
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
  /* title drag still works when pressing in the middle of the title bar */
  int ox=w->r.x; ptr(w->r.x+200,w->r.y+20,1); ptr(w->r.x+240,w->r.y+20,1); ptr(w->r.x+240,w->r.y+20,0);
  assert(w->r.x==ox+40);
  printf("wm resize tests passed\n");
  return 0;
}
