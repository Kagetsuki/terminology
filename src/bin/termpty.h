#ifndef _TERMPTY_H__
#define _TERMPTY_H__ 1

#include "config.h"

typedef struct _Termpty       Termpty;
typedef struct _Termcell      Termcell;
typedef struct _Termatt       Termatt;
typedef struct _Termstate     Termstate;
typedef struct _Termsave      Termsave;
typedef struct _Termsavecomp  Termsavecomp;
typedef struct _Termblock     Termblock;
typedef struct _Termexp       Termexp;

#define COL_DEF        0
#define COL_BLACK      1
#define COL_RED        2
#define COL_GREEN      3
#define COL_YELLOW     4
#define COL_BLUE       5
#define COL_MAGENTA    6
#define COL_CYAN       7
#define COL_WHITE      8
#define COL_INVIS      9

#define COL_INVERSE   10
#define COL_INVERSEBG 11

#define MOUSE_OFF              0
#define MOUSE_X10              1 // Press only
#define MOUSE_NORMAL           2 // Press+release only
#define MOUSE_NORMAL_BTN_MOVE  3 // Press+release+motion while pressed
#define MOUSE_NORMAL_ALL_MOVE  4 // Press+release+all motion

#define MOUSE_EXT_NONE         0
#define MOUSE_EXT_UTF8         1
#define MOUSE_EXT_SGR          2
#define MOUSE_EXT_URXVT        3

// choose - italic OR double-width support

//#define SUPPORT_ITALIC   1
#define SUPPORT_DBLWIDTH 1

// Only for testing purpose
//#define SUPPORT_80_132_COLUMNS 1

struct _Termatt
{
   unsigned char fg, bg;
   unsigned short bold : 1;
   unsigned short faint : 1;
#if defined(SUPPORT_ITALIC)
   unsigned short italic : 1;
#elif defined(SUPPORT_DBLWIDTH)
   unsigned short dblwidth : 1;
#endif   
   unsigned short underline : 1;
   unsigned short blink : 1; // don't intend to support this currently
   unsigned short blink2 : 1; // don't intend to support this currently
   unsigned short inverse : 1;
   unsigned short invisible : 1;
   unsigned short strike : 1;
   unsigned short fg256 : 1;
   unsigned short bg256 : 1;
   unsigned short fgintense : 1;
   unsigned short bgintense : 1;
   // below used for working out text from selections
   unsigned short autowrapped : 1;
   unsigned short newline : 1;
   unsigned short tab : 1;
   unsigned short fraktur : 1;
#if defined(SUPPORT_80_132_COLUMNS)
   unsigned short is_80_132_mode_allowed : 1;
#endif
};

struct _Termstate
{
   int           cx, cy;
   Termatt       att;
   unsigned char charset;
   unsigned char charsetch;
   unsigned char chset[4];
   int           scroll_y1, scroll_y2;
   int           had_cr_x, had_cr_y;
   int           margin_top; // soon, more to come...
   unsigned int  multibyte : 1;
   unsigned int  alt_kp : 1;
   unsigned int  insert : 1;
   unsigned int  appcursor : 1;
   unsigned int  wrap : 1;
   unsigned int  wrapnext : 1;
   unsigned int  hidecursor : 1;
   unsigned int  crlf : 1;
   unsigned int  had_cr : 1;
   unsigned int  send_bs : 1;
   unsigned int  kbd_lock : 1;
   unsigned int  reverse : 1;
   unsigned int  no_autorepeat : 1;
   unsigned int  cjk_ambiguous_wide : 1;
   unsigned int  bracketed_paste : 1;
};

struct _Termpty
{
   Evas_Object *obj;
   Ecore_Event_Handler *hand_exe_exit;
   Ecore_Fd_Handler *hand_fd;
   struct {
      struct {
         void (*func) (void *data);
         void *data;
      } change, set_title, set_icon, cancel_sel, exited, bell, command;
   } cb;
   struct {
      const char *title, *icon;
   } prop;
   const char *cur_cmd;
   Termcell *screen, *screen2;
   Termsave **back;
   unsigned char oldbuf[4];
   int *buf;
   int buflen;
   int w, h;
   int fd, slavefd;
   int circular_offset;
   int circular_offset2;
   int backmax, backpos;
   int backscroll_num;
   struct {
      int curid;
      Eina_Hash *blocks;
      Eina_Hash *chid_map;
      Eina_List *active;
      Eina_List *expecting;
      Eina_Bool on : 1;
   } block;
   struct {
      struct {
         int x, y;
      } start, end, start_backup, end_backup;
      Eina_Bool is_active : 1;
      Eina_Bool is_box    : 1;
      Eina_Bool makesel   : 1;
   } selection;
   Termstate state, save, swap;
   int exit_code;
   pid_t pid;
   unsigned int altbuf     : 1;
   unsigned int mouse_mode : 3;
   unsigned int mouse_ext  : 2;
};

struct _Termcell
{
   Eina_Unicode   codepoint;
   Termatt        att;
};

struct _Termsave
{
   unsigned int   gen  : 8;
   unsigned int   comp : 1;
   unsigned int   z    : 1;
   unsigned int   w    : 22;
   Termcell       cell[1];
};

struct _Termsavecomp
{
   unsigned int   gen  : 8;
   unsigned int   comp : 1;
   unsigned int   z    : 1;
   unsigned int   w    : 22; // compressed size in bytes
   unsigned int   wout; // output width in Termcells
};

struct _Termblock
{
   Termpty     *pty;
   const char  *path, *link, *chid;
   Evas_Object *obj;
   Eina_List   *cmds;
   int          id;
   int          type;
   int          refs;
   short        w, h;
   short        x, y;
   Eina_Bool    scale_stretch : 1;
   Eina_Bool    scale_center : 1;
   Eina_Bool    scale_fill : 1;
   Eina_Bool    thumb : 1;
   Eina_Bool    edje : 1;
   
   Eina_Bool    active : 1;
   Eina_Bool    was_active : 1;
   Eina_Bool    was_active_before : 1;
};

struct _Termexp
{
   int ch, left, id;
   int x, y, w, h;
};

void       termpty_init(void);
void       termpty_shutdown(void);

Termpty   *termpty_new(const char *cmd, Eina_Bool login_shell, const char *cd,
                      int w, int h, int backscroll, Eina_Bool xterm_256color,
                      Eina_Bool erase_is_del, const char *emotion_mod);
void       termpty_free(Termpty *ty);
void       termpty_cellcomp_freeze(Termpty *ty);
void       termpty_cellcomp_thaw(Termpty *ty);
Termcell  *termpty_cellrow_get(Termpty *ty, int y, int *wret);
void       termpty_write(Termpty *ty, const char *input, int len);
void       termpty_resize(Termpty *ty, int w, int h);
void       termpty_backscroll_set(Termpty *ty, int size);

pid_t      termpty_pid_get(const Termpty *ty);
void       termpty_block_free(Termblock *tb);
Termblock *termpty_block_new(Termpty *ty, int w, int h, const char *path, const char *link);
void       termpty_block_insert(Termpty *ty, int ch, Termblock *blk);
int        termpty_block_id_get(Termcell *cell, int *x, int *y);
Termblock *termpty_block_get(Termpty *ty, int id);
void       termpty_block_chid_update(Termpty *ty, Termblock *blk);
Termblock *termpty_block_chid_get(Termpty *ty, const char *chid);

void       termpty_cell_copy(Termpty *ty, Termcell *src, Termcell *dst, int n);
void       termpty_cell_fill(Termpty *ty, Termcell *src, Termcell *dst, int n);
void       termpty_cell_codepoint_att_fill(Termpty *ty, Eina_Unicode codepoint, Termatt att, Termcell *dst, int n);
void       termpty_screen_swap(Termpty *ty);

ssize_t termpty_line_length(const Termcell *cells, ssize_t nb_cells);

Config *termpty_config_get(const Termpty *ty);

extern int _termpty_log_dom;

#define TERMPTY_SCREEN(Tpty, X, Y) \
  Tpty->screen[X + (((Y + Tpty->circular_offset) % Tpty->h) * Tpty->w)]
#define TERMPTY_FMTCLR(Tatt) \
   (Tatt).autowrapped = (Tatt).newline = (Tatt).tab = 0

#endif
