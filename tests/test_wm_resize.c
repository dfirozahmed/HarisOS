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
static int tick_calls, tick_ret;
static int drag_calls, drag_ret;
static int on_press_cb(Window*w,int x,int y){ (void)w; (void)x; (void)y; return 0; }
static int on_drag_cb(Window*w,int x,int y,unsigned b){ (void)w; (void)x; (void)y; (void)b; drag_calls++; return drag_ret; }
static void on_btn_cb(UiButton*b,void*u){ (void)b; (void)u; }
static int on_tick_cb(Window*w){ (void)w; tick_calls++; return tick_ret; }
static int on_hover_cb(Window*w,int x,int y){ (void)w; hov_calls++; hov_x=x; hov_y=y; return 0; }
static int on_rpress_cb(Window*w,int x,int y){ (void)w; (void)y; rp_calls++; rp_x=x; return 0; }
static int close_req_calls, close_req_veto, closed_calls;
static int on_close_req_cb(Window*w){ (void)w; close_req_calls++; return close_req_veto; }
static void on_closed_cb(Window*w){ (void)w; closed_calls++; }
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
    ptr(w->r.x + w->r.w - 22, cy, 0);
    assert(wm_titlebtn_hover(w) == 0 && wm_titlebtn_pressed(w) == -1);
    ptr(w->r.x + w->r.w - 62, cy, 0);
    assert(wm_titlebtn_hover(w) == 1);
    ptr(w->r.x + w->r.w - 42, cy, 0);
    assert(wm_titlebtn_hover(w) == 2);
    ptr(w->r.x + w->r.w - 42, cy, 1);
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
    wheel_ret=2; wm_wheel(w->r.x+50,w->r.y+100,1);
    assert(!WM_HAS_DMG);                                    /* 2 = "I marked my own area": the window is not damaged again */
    wheel_ret=1; wm_wheel(w->r.x+50,w->r.y+100,1);
    assert(WM_HAS_DMG);                                     /* 1 = the whole window */
    wm_take_damage(&(UiRect){0});
    wm_close(other);
  }
  /* on_drag: 1 = redraw the whole window, 2 = the handler marked its own area */
  {
    wm_pointer(5000,5000,0);
    w->r = ui_rect(300,200,400,300); wm_layout_internal(w);
    w->on_press=on_press_cb; w->on_drag=on_drag_cb;
    wm_take_damage(&(UiRect){0});
    drag_ret=2; wm_pointer(w->r.x+50,w->r.y+100,1);               /* press inside the content: starts the drag handler */
    wm_take_damage(&(UiRect){0});
    wm_pointer(w->r.x+60,w->r.y+100,1);                           /* held: on_drag says 2 */
    assert(drag_calls>=1 && !WM_HAS_DMG);
    drag_ret=1; wm_pointer(w->r.x+70,w->r.y+100,1);
    assert(WM_HAS_DMG);                                           /* 1: the whole window */
    wm_pointer(w->r.x+70,w->r.y+100,0);
    w->on_press=0; w->on_drag=0;
    wm_take_damage(&(UiRect){0});
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
  /* tick: windows with on_tick are asked each pass; a change is reported and marked for redraw */
  {
    tick_calls=0; tick_ret=0;
    w->on_tick=on_tick_cb;
    wm_take_damage(&(UiRect){0});
    assert(wm_tick()==0 && tick_calls==1 && !WM_HAS_DMG);
    tick_ret=1;
    assert(wm_tick()==1 && tick_calls==2 && WM_HAS_DMG);
    wm_take_damage(&(UiRect){0});
    tick_ret=2;
    assert(wm_tick()==1 && tick_calls==3 && !WM_HAS_DMG);   /* 2 = changed, but the handler marks its own area */
    tick_ret=1;
    wm_minimize(w); tick_calls=0; wm_tick(); assert(tick_calls==0);   /* minimized windows are skipped */
    wm_activate(w);
  }
  /* buttons: a hover or press highlight redraws only the button; a click or any callback redraws the window */
  {
    UiButtonSet s; UiButton a=UI_BUTTON("A",UI_BTN_SECONDARY,0,0), b=UI_BUTTON("B",UI_BTN_SECONDARY,0,0);
    UiRect d={0,0,0,0};
    ui_buttons_init(&s);
    a.r=ui_rect(10,10,60,30); b.r=ui_rect(100,10,60,30);
    ui_buttons_add(&s,&a); ui_buttons_add(&s,&b);
    assert(ui_buttons_pointer_dirty(&s,500,500,0,&d)==0);                       /* nowhere near: nothing */
    assert(ui_buttons_pointer_dirty(&s,20,20,0,&d)==2);                         /* enter A */
    assert(d.x<=10 && d.y<=10 && d.x+d.w>=70 && d.y+d.h>=40 && d.x+d.w<100);   /* covers A, not B */
    assert(ui_buttons_pointer_dirty(&s,20,21,0,&d)==0);                         /* still on A */
    assert(ui_buttons_pointer_dirty(&s,110,20,0,&d)==2);                        /* A -> B: both */
    assert(d.x<=10 && d.x+d.w>=160);
    assert(ui_buttons_pointer_dirty(&s,110,20,1,&d)==2);                        /* press B (no on_press) */
    assert(ui_buttons_pointer_dirty(&s,110,20,0,&d)==1);                        /* release on B = click: all */
    b.on_hover=on_btn_cb; a.on_hover=on_btn_cb;
    assert(ui_buttons_pointer_dirty(&s,20,20,0,&d)==1);                         /* a hover callback may change anything */
    assert(ui_buttons_pointer(&s,500,500,0)==1);                                 /* the old call: any change is 1 */
  }
  /* damage: overlapping areas become separate non-overlapping rectangles that cover exactly their union */
  {
    UiRect r[WM_DAMAGE_MAX]; int n;
    wm_take_damage_rects(r,WM_DAMAGE_MAX);
    wm_add_damage(ui_rect(100,100,200,100));
    wm_add_damage(ui_rect(150,150,200,100));       /* diagonal move: overlaps the first at a corner */
    n=wm_take_damage_rects(r,WM_DAMAGE_MAX);
    assert(n>=2 && !WM_HAS_DMG);
    {
      long area=0; unsigned char*g=calloc(W*H,1);
      for(int i=0;i<n;i++){
        area+=(long)r[i].w*r[i].h;
        for(int y=r[i].y;y<r[i].y+r[i].h;y++) for(int x=r[i].x;x<r[i].x+r[i].w;x++) g[y*W+x]++;
      }
      assert(area==200*100+200*100-150*50);         /* union area, not the bounding box */
      for(int y=0;y<H;y++) for(int x=0;x<W;x++){
        int in=(x>=100&&x<300&&y>=100&&y<200)||(x>=150&&x<350&&y>=150&&y<250);
        assert(g[y*W+x]==(in?1:0));                 /* covered once, nothing else */
      }
      free(g);
    }
    wm_add_damage(ui_rect(100,100,200,100));
    wm_add_damage(ui_rect(120,120,50,50));          /* inside the first: nothing new */
    assert(wm_take_damage_rects(r,WM_DAMAGE_MAX)==1);
    for(int i=0;i<20;i++) wm_add_damage(ui_rect(i*40,i*20,10,10)); /* too many pieces: falls back to one box */
    n=wm_take_damage_rects(r,WM_DAMAGE_MAX); assert(n>=1 && n<=WM_DAMAGE_MAX);
    wm_add_damage(ui_rect(10,10,20,20)); wm_add_damage(ui_rect(500,300,20,20));
    { UiRect b; assert(wm_take_damage(&b) && b.x==10 && b.y==10 && b.w==510 && b.h==310); }
  }
  /* close request: a window may refuse a close the user asked for; wm_close() never asks */
  {
    int before=wm_count();
    Window*c=wm_create("Closer",300,300,200,150,draw,0);
    assert(wm_count()==before+1);
    assert(wm_request_close(c)==1 && wm_count()==before);                 /* no hook: it closes */
    assert(wm_request_close(c)==0);                                       /* already closed: nothing */
    c=wm_create("Closer",300,300,200,150,draw,0);
    c->on_close_request=on_close_req_cb; c->on_close=on_closed_cb;
    close_req_calls=0; closed_calls=0; close_req_veto=1;
    assert(wm_request_close(c)==0 && wm_count()==before+1 && close_req_calls==1 && closed_calls==0);
    /* the title bar's close button asks too (press and release on it) */
    int bx=c->r.x+c->r.w-22, by=c->r.y+ui_theme()->titlebar_h/2;
    ptr(bx,by,1); ptr(bx,by,0);
    assert(close_req_calls==2 && wm_count()==before+1 && closed_calls==0);
    close_req_veto=0;                                                     /* now it agrees */
    ptr(bx,by,1); ptr(bx,by,0);
    assert(close_req_calls==3 && wm_count()==before && closed_calls==1);
    c=wm_create("Closer",300,300,200,150,draw,0);
    c->on_close_request=on_close_req_cb; c->on_close=on_closed_cb;
    close_req_calls=0; closed_calls=0; close_req_veto=1;
    wm_close(c);                                                          /* the program closes it: not asked */
    assert(close_req_calls==0 && closed_calls==1 && wm_count()==before);
  }
  printf("wm resize tests passed\n");
  return 0;
}
