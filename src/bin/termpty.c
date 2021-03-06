#include "private.h"
#include <Elementary.h>
#include "termpty.h"
#include "termptyesc.h"
#include "termptyops.h"
#include "termptysave.h"
#include "termio.h"
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>

/* specific log domain to help debug only terminal code parser */
int _termpty_log_dom = -1;

#undef CRITICAL
#undef ERR
#undef WRN
#undef INF
#undef DBG

#define CRITICAL(...) EINA_LOG_DOM_CRIT(_termpty_log_dom, __VA_ARGS__)
#define ERR(...)      EINA_LOG_DOM_ERR(_termpty_log_dom, __VA_ARGS__)
#define WRN(...)      EINA_LOG_DOM_WARN(_termpty_log_dom, __VA_ARGS__)
#define INF(...)      EINA_LOG_DOM_INFO(_termpty_log_dom, __VA_ARGS__)
#define DBG(...)      EINA_LOG_DOM_DBG(_termpty_log_dom, __VA_ARGS__)

void
termpty_init(void)
{
   if (_termpty_log_dom >= 0) return;

   _termpty_log_dom = eina_log_domain_register("termpty", NULL);
   if (_termpty_log_dom < 0)
     EINA_LOG_CRIT("could not create log domain 'termpty'.");
}

void
termpty_shutdown(void)
{
   if (_termpty_log_dom < 0) return;
   eina_log_domain_unregister(_termpty_log_dom);
   _termpty_log_dom = -1;
}

static void
_handle_buf(Termpty *ty, const Eina_Unicode *codepoints, int len)
{
   Eina_Unicode *c, *ce, *b;
   int n, bytes;

   c = (Eina_Unicode *)codepoints;
   ce = &(c[len]);

   if (ty->buf)
     {
        bytes = (ty->buflen + len + 1) * sizeof(int);
        b = realloc(ty->buf, bytes);
        if (!b)
          {
             ERR("memerr");
             return;
          }
        INF("realloc add %i + %i", (int)(ty->buflen * sizeof(int)), (int)(len * sizeof(int)));
        bytes = len * sizeof(Eina_Unicode);
        memcpy(&(b[ty->buflen]), codepoints, bytes);
        ty->buf = b;
        ty->buflen += len;
        ty->buf[ty->buflen] = 0;
        c = ty->buf;
        ce = c + ty->buflen;
        while (c < ce)
          {
             n = _termpty_handle_seq(ty, c, ce);
             if (n == 0)
               {
                  Eina_Unicode *tmp = ty->buf;
                  ty->buf = NULL;
                  ty->buflen = 0;
                  bytes = ((char *)ce - (char *)c) + sizeof(Eina_Unicode);
                  INF("malloc til %i", (int)(bytes - sizeof(Eina_Unicode)));
                  ty->buf = malloc(bytes);
                  if (!ty->buf)
                    {
                       ERR("memerr");
                       return;
                    }
                  bytes = (char *)ce - (char *)c;
                  memcpy(ty->buf, c, bytes);
                  ty->buflen = bytes / sizeof(Eina_Unicode);
                  ty->buf[ty->buflen] = 0;
                  free(tmp);
                  break;
               }
             c += n;
          }
        if (c == ce)
          {
             if (ty->buf)
               {
                  free(ty->buf);
                  ty->buf = NULL;
               }
             ty->buflen = 0;
          }
     }
   else
     {
        while (c < ce)
          {
             n = _termpty_handle_seq(ty, c, ce);
             if (n == 0)
               {
                  bytes = ((char *)ce - (char *)c) + sizeof(Eina_Unicode);
                  ty->buf = malloc(bytes);
                  INF("malloc %i", (int)(bytes - sizeof(Eina_Unicode)));
                  if (!ty->buf)
                    {
                       ERR("memerr");
                    }
                  else
                    {
                       bytes = (char *)ce - (char *)c;
                       memcpy(ty->buf, c, bytes);
                       ty->buflen = bytes / sizeof(Eina_Unicode);
                       ty->buf[ty->buflen] = 0;
                    }
                  break;
               }
             c += n;
          }
     }
}

static void
_pty_size(Termpty *ty)
{
   struct winsize sz;

   sz.ws_col = ty->w;
   sz.ws_row = ty->h;
   sz.ws_xpixel = 0;
   sz.ws_ypixel = 0;
   if (ioctl(ty->fd, TIOCSWINSZ, &sz) < 0)
     ERR("Size set ioctl failed: %s", strerror(errno));
}

static Eina_Bool
_cb_exe_exit(void *data, int type EINA_UNUSED, void *event)
{
   Ecore_Exe_Event_Del *ev = event;
   Termpty *ty = data;

   if (ev->pid != ty->pid) return ECORE_CALLBACK_PASS_ON;
   ty->exit_code = ev->exit_code;
   
   ty->pid = -1;

   if (ty->hand_exe_exit) ecore_event_handler_del(ty->hand_exe_exit);
   ty->hand_exe_exit = NULL;
   if (ty->hand_fd) ecore_main_fd_handler_del(ty->hand_fd);
   ty->hand_fd = NULL;
   if (ty->fd >= 0) close(ty->fd);
   ty->fd = -1;
   if (ty->slavefd >= 0) close(ty->slavefd);
   ty->slavefd = -1;

   if (ty->cb.exited.func) ty->cb.exited.func(ty->cb.exited.data);
   
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_cb_fd_read(void *data, Ecore_Fd_Handler *fd_handler EINA_UNUSED)
{
   Termpty *ty = data;
   char buf[4097];
   Eina_Unicode codepoint[4097];
   int len, i, j, k, reads;

   // read up to 64 * 4096 bytes
   for (reads = 0; reads < 64; reads++)
     {
        char *rbuf = buf;
        len = sizeof(buf) - 1;

        for (i = 0; i < (int)sizeof(ty->oldbuf) && ty->oldbuf[i] & 0x80; i++)
          {
             *rbuf = ty->oldbuf[i];
             rbuf++;
             len--;
          }
        len = read(ty->fd, rbuf, len);
        if (len <= 0) break;


        for (i = 0; i < (int)sizeof(ty->oldbuf); i++)
          ty->oldbuf[i] = 0;

        len += rbuf - buf;

        /*
        printf(" I: ");
        int jj;
        for (jj = 0; jj < len; jj++)
          {
             if ((buf[jj] < ' ') || (buf[jj] >= 0x7f))
               printf("\033[33m%02x\033[0m", (unsigned char)buf[jj]);
             else
               printf("%c", buf[jj]);
          }
        printf("\n");
        */
        buf[len] = 0;
        // convert UTF8 to codepoint integers
        j = 0;
        for (i = 0; i < len;)
          {
             int g = 0, prev_i = i;

             if (buf[i])
               {
#if (EINA_VERSION_MAJOR > 1) || (EINA_VERSION_MINOR >= 8)
                  g = eina_unicode_utf8_next_get(buf, &i);
                  if ((0xdc80 <= g) && (g <= 0xdcff) &&
                      (len - prev_i) <= (int)sizeof(ty->oldbuf))
#else
                  i = evas_string_char_next_get(buf, i, &g);
                  if (i < 0 &&
                      (len - prev_i) <= (int)sizeof(ty->oldbuf))
#endif
                    {
                       for (k = 0;
                            (k < (int)sizeof(ty->oldbuf)) && 
                            (k < (len - prev_i));
                            k++)
                         {
                            ty->oldbuf[k] = buf[prev_i+k];
                         }
                       DBG("failure at %d/%d/%d", prev_i, i, len);
                       break;
                    }
               }
             else
               {
                  g = 0;
                  i++;
               }
             codepoint[j] = g;
             j++;
          }
        codepoint[j] = 0;
//        DBG("---------------- handle buf %i", j);
        _handle_buf(ty, codepoint, j);
     }
   if (ty->cb.change.func) ty->cb.change.func(ty->cb.change.data);
   return EINA_TRUE;
}

static void
_limit_coord(Termpty *ty, Termstate *state)
{
   state->wrapnext = 0;
   if (state->cx >= ty->w) state->cx = ty->w - 1;
   if (state->cy >= ty->h) state->cy = ty->h - 1;
   if (state->had_cr_x >= ty->w) state->had_cr_x = ty->w - 1;
   if (state->had_cr_y >= ty->h) state->had_cr_y = ty->h - 1;
}

Termpty *
termpty_new(const char *cmd, Eina_Bool login_shell, const char *cd,
            int w, int h, int backscroll, Eina_Bool xterm_256color,
            Eina_Bool erase_is_del, const char *emotion_mod)
{
   Termpty *ty;
   const char *pty;
   int mode;
   struct termios t;

   ty = calloc(1, sizeof(Termpty));
   if (!ty) return NULL;
   ty->w = w;
   ty->h = h;
   ty->backmax = backscroll;

   _termpty_reset_state(ty);
   ty->save = ty->state;
   ty->swap = ty->state;

   ty->screen = calloc(1, sizeof(Termcell) * ty->w * ty->h);
   if (!ty->screen)
     {
        ERR("Allocation of term screen %ix%i", ty->w, ty->h);
        goto err;
     }
   ty->screen2 = calloc(1, sizeof(Termcell) * ty->w * ty->h);
   if (!ty->screen2)
     {
        ERR("Allocation of term screen2 %ix%i", ty->w, ty->h);
        goto err;
     }

   ty->circular_offset = 0;

   ty->fd = posix_openpt(O_RDWR | O_NOCTTY);
   if (ty->fd < 0)
     {
        ERR("posix_openpt failed: %s", strerror(errno));
        goto err;
     }
   if (grantpt(ty->fd) != 0)
     {
        WRN("grantpt failed: %s", strerror(errno));
     }
   if (unlockpt(ty->fd) != 0)
     {
        ERR("unlockpt failed: %s", strerror(errno));
        goto err;
     }
   pty = ptsname(ty->fd);
   ty->slavefd = open(pty, O_RDWR | O_NOCTTY);
   if (ty->slavefd < 0)
     {
        ERR("open of pty '%s' failed: %s", pty, strerror(errno));
        goto err;
     }
   mode = fcntl(ty->fd, F_GETFL, 0);
   if (mode < 0)
     {
        ERR("fcntl on pty '%s' failed: %s", pty, strerror(errno));
        goto err;
     }
   if (!(mode & O_NDELAY))
      if (fcntl(ty->fd, F_SETFL, mode | O_NDELAY))
        {
           ERR("fcntl on pty '%s' failed: %s", pty, strerror(errno));
           goto err;
        }


   tcgetattr(ty->fd, &t);
   t.c_cc[VERASE] =  (erase_is_del) ? 0x7f : 0x8;
#ifdef IUTF8
   t.c_iflag |= IUTF8;
#endif
   tcsetattr(ty->fd, TCSANOW, &t);

   ty->hand_exe_exit = ecore_event_handler_add(ECORE_EXE_EVENT_DEL,
                                               _cb_exe_exit, ty);
   if (!ty->hand_exe_exit)
     {
        ERR("event handler add failed");
        goto err;
     }
   ty->pid = fork();
   if (!ty->pid)
     {
        const char *shell = NULL;
        const char *args[4] = {NULL, NULL, NULL, NULL};
        Eina_Bool needs_shell;
        int i;
        char buf[256];

        if (cd)
          {
             if (chdir(cd) != 0)
               {
                  perror("chdir");
                  ERR("Cannot change to directory '%s'", cd);
                  exit(127);
               }
          }
        
        needs_shell = ((!cmd) ||
                       (strpbrk(cmd, " |&;<>()$`\\\"'*?#") != NULL));
        DBG("cmd='%s' needs_shell=%u", cmd ? cmd : "", needs_shell);

        if (needs_shell)
          {
             shell = getenv("SHELL");
             if (!shell)
               {
                  uid_t uid = getuid();
                  struct passwd *pw = getpwuid(uid);
                  if (pw) shell = pw->pw_shell;
               }
             if (!shell)
               {
                  WRN("Could not find shell, fallback to /bin/sh");
                  shell = "/bin/sh";
               }
          }

        if (!needs_shell)
          args[0] = cmd;
        else
          {
             args[0] = shell;
             if (cmd)
               {
                  args[1] = "-c";
                  args[2] = cmd;
               }
          }

#define NC(x) (args[x] != NULL ? args[x] : "")
        DBG("exec %s %s %s %s", NC(0), NC(1), NC(2), NC(3));
#undef NC

        for (i = 0; i < 100; i++)
          {
             if (i != ty->slavefd) close(i);
          }
        setsid();

        dup2(ty->slavefd, 0);
        dup2(ty->slavefd, 1);
        dup2(ty->slavefd, 2);

        if (ioctl(ty->slavefd, TIOCSCTTY, NULL) < 0) exit(1);
        
        close(ty->fd);
        close(ty->slavefd);
        
        /* TODO: should we reset signals here? */

        /* pretend to be xterm */
        if (xterm_256color)
          {
             putenv("TERM=xterm-256color");
          }
        else
          {
             putenv("TERM=xterm");
          }
        putenv("XTERM_256_COLORS=1");
        if (emotion_mod)
          {
             snprintf(buf, sizeof(buf), "EMOTION_ENGINE=%s", emotion_mod);
             putenv(buf);
          }
        if (!login_shell)
          execvp(args[0], (char *const *)args);
        else
          {
             char *cmdfile, *cmd0;
             
             cmdfile = (char *)args[0];
             cmd0 = alloca(strlen(cmdfile) + 2);
             cmd0[0] = '-';
             strcpy(cmd0 + 1, cmdfile);
             args[0] = cmd0;
             execvp(cmdfile, (char *const *)args);
          }
        exit(127); /* same as system() for failed commands */
     }
   ty->hand_fd = ecore_main_fd_handler_add(ty->fd, ECORE_FD_READ,
                                           _cb_fd_read, ty,
                                           NULL, NULL);
   close(ty->slavefd);
   ty->slavefd = -1;
   _pty_size(ty);
   termpty_save_register(ty);
   return ty;
err:
   if (ty->screen) free(ty->screen);
   if (ty->screen2) free(ty->screen2);
   if (ty->fd >= 0) close(ty->fd);
   if (ty->slavefd >= 0) close(ty->slavefd);
   free(ty);
   return NULL;
}

void
termpty_free(Termpty *ty)
{
   Termexp *ex;

   termpty_save_unregister(ty);
   EINA_LIST_FREE(ty->block.expecting, ex) free(ex);
   if (ty->block.blocks) eina_hash_free(ty->block.blocks);
   if (ty->block.chid_map) eina_hash_free(ty->block.chid_map);
   if (ty->block.active) eina_list_free(ty->block.active);
   if (ty->fd >= 0) close(ty->fd);
   if (ty->slavefd >= 0) close(ty->slavefd);
   if (ty->pid >= 0)
     {
        int i;
        
        // in case someone stopped the child - cont it
        kill(ty->pid, SIGCONT);
        // signpipe for shells
        kill(ty->pid, SIGPIPE);
        // try 400 time (sleeping for 1ms) to check for death of child
        for (i = 0; i < 400; i++)
          {
             int status = 0;

             // poll exit of child pid
             if (waitpid(ty->pid, &status, WNOHANG) == ty->pid)
               {
                  // if child exited - break loop and mark pid as done
                  ty->pid = -1;
                  break;
               }
             // after 100ms set sigint
             if      (i == 100) kill(ty->pid, SIGINT);
             // after 200ms send term signal
             else if (i == 200) kill(ty->pid, SIGTERM);
             // after 300ms send quit signal
             else if (i == 300) kill(ty->pid, SIGQUIT);
             usleep(1000); // sleep 1ms
          }
        // so 400ms and child not gone - KILL!
        if (ty->pid >= 0)
          {
             kill(ty->pid, SIGKILL);
             ty->pid = -1;
          }
     }
   if (ty->hand_exe_exit) ecore_event_handler_del(ty->hand_exe_exit);
   if (ty->hand_fd) ecore_main_fd_handler_del(ty->hand_fd);
   if (ty->prop.title) eina_stringshare_del(ty->prop.title);
   if (ty->prop.icon) eina_stringshare_del(ty->prop.icon);
   if (ty->back)
     {
        int i;

        for (i = 0; i < ty->backmax; i++)
          {
             if (ty->back[i])
               {
                  termpty_save_free(ty->back[i]);
                  ty->back[i] = NULL;
               }
          }
        free(ty->back);
        ty->back = NULL;
     }
   if (ty->screen) free(ty->screen);
   if (ty->screen2) free(ty->screen2);
   if (ty->buf) free(ty->buf);
   memset(ty, 0, sizeof(Termpty));
   free(ty);
}

void
termpty_cellcomp_freeze(Termpty *ty EINA_UNUSED)
{
   termpty_save_freeze();
}

void
termpty_cellcomp_thaw(Termpty *ty EINA_UNUSED)
{
   termpty_save_thaw();
}

Termcell *
termpty_cellrow_get(Termpty *ty, int y, int *wret)
{
   Termsave *ts, **tssrc;

   if (y >= 0)
     {
        if (y >= ty->h) return NULL;
        *wret = ty->w;
	/* fprintf(stderr, "getting: %i (%i, %i)\n", y, ty->circular_offset, ty->h); */
        return &(TERMPTY_SCREEN(ty, 0, y));
     }
   if ((y < -ty->backmax) || !ty->back) return NULL;
   tssrc = &(ty->back[(ty->backmax + ty->backpos + y) % ty->backmax]);
   ts = termpty_save_extract(*tssrc);
   if (!ts) return NULL;
   *tssrc = ts;
   *wret = ts->w;
   return ts->cell;
}
   
void
termpty_write(Termpty *ty, const char *input, int len)
{
   if (ty->fd < 0) return;
   if (write(ty->fd, input, len) < 0) ERR("write %s", strerror(errno));
}

ssize_t
termpty_line_length(const Termcell *cells, ssize_t nb_cells)
{
   ssize_t len = nb_cells;

   for (len = nb_cells - 1; len >= 0; len--)
     {
        const Termcell *cell = cells + len;

        if ((cell->codepoint != 0) &&
            (cell->att.bg != COL_INVIS))
          return len + 1;
     }

   return 0;
}

static int
termpty_line_find_top(Termpty *ty, int y_end)
{
   int y_start = y_end;
   Termsave *ts;

   while (y_start > 0)
     {
        if (TERMPTY_SCREEN(ty, ty->w - 1, y_start - 1).att.autowrapped)
          y_start--;
        else
          return y_start;
     }
   while (-y_start < ty->backscroll_num)
     {
        ts = termpty_save_extract(ty->back[(y_start + ty->backpos - 1 +
                                            ty->backmax) % ty->backmax]);
        ty->back[(y_start + ty->backpos - 1 + ty->backmax) % ty->backmax] = ts;
        if (ts->cell[ts->w - 1].att.autowrapped)
          y_start--;
        else
          return y_start;
     }
   return y_start;
}

static int
termpty_line_rewrap(Termpty *ty, int y_start, int y_end,
                    Termcell *screen2, Termsave **back2,
                    int w2, int y2_end)
{
   int x, x2, y, y2, y2_start;
   int len, len_last, len_remaining, copy_width, ts2_width;
   Termsave *ts, *ts2;
   Termcell *line, *line2 = NULL;

   if (y_end >= 0)
     {
        len_last = termpty_line_length(&TERMPTY_SCREEN(ty, 0, y_end), ty->w);
     }
   else
     {
        ts = termpty_save_extract(ty->back[(y_end + ty->backpos +
                                            ty->backmax) % ty->backmax]);
        ty->back[(y_end + ty->backpos + ty->backmax) % ty->backmax] = ts;
        len_last = ts->w;
     }
   len_remaining = len_last + (y_end - y_start) * ty->w;
   y2_start = y2_end;
   if (len_remaining)
     {
        y2_start -= (len_remaining + w2 - 1) / w2 - 1;
     }
   else
     {
        if (y2_start < 0)
          back2[y2_start + ty->backmax] = termpty_save_new(0);
        return y2_start;
     }
   if (-y2_start > ty->backmax)
     {
        y_start += ((-y2_start - ty->backmax) * w2) / ty->w;
        x = ((-y2_start - ty->backmax) * w2) % ty->w;
        len_remaining -= (-y2_start - ty->backmax) * w2;
        y2_start = -ty->backmax;
     }
   else
     {
        x = 0;
     }
   y = y_start;
   x2 = 0;
   y2 = y2_start;

   while (y <= y_end)
     {
        if (y >= 0)
          {
             line = &TERMPTY_SCREEN(ty, 0, y);
          }
        else
          {
             ts = termpty_save_extract(ty->back[(y + ty->backpos +
                                                 ty->backmax) % ty->backmax]);
             ty->back[(y + ty->backpos + ty->backmax) % ty->backmax] = ts;
             line = ts->cell;
          }
        if (y == y_end)
          len = len_last;
        else
          len = ty->w;
        line[len - 1].att.autowrapped = 0;
        while (x < len)
          {
             copy_width = MIN(len - x, w2 - x2);
             if (x2 == 0)
               {
                  if (y2 >= 0)
                    {
                       line2 = screen2 + (y2 * w2);
                    }
                  else
                    {
                       ts2_width = MIN(len_remaining, w2);
                       ts2 = termpty_save_new(ts2_width);
                       line2 = ts2->cell;
                       back2[y2 + ty->backmax] = ts2;
                    }
               }
             if (line2)
               {
                  termpty_cell_copy(ty, line + x, line2 + x2, copy_width);
                  x += copy_width;
                  x2 += copy_width;
                  len_remaining -= copy_width;
                  if ((x2 == w2) && (y2 != y2_end))
                    {
                       line2[x2 - 1].att.autowrapped = 1;
                       x2 = 0;
                       y2++;
                    }
               }
          }
        x = 0;
        y++;
     }
   return y2_start;
}


void
termpty_resize(Termpty *ty, int new_w, int new_h)
{
   Termcell *new_screen;
   Termsave **new_back;
   int y_start, y_end, new_y_start = 0, new_y_end;
   int i, altbuf = 0;

   if ((ty->w == new_w) && (ty->h == new_h)) return;
   if ((new_w == new_h) && (new_w == 1)) return; // FIXME: something weird is
                                                 // going on at term init

   termpty_save_freeze();

   if (ty->altbuf)
     {
        termpty_screen_swap(ty);
        altbuf = 1;
     }

   new_screen = calloc(1, sizeof(Termcell) * new_w * new_h);
   if (!new_screen)
     {
        ERR("memerr");
        return;
     }
   free(ty->screen2);
   ty->screen2 = calloc(1, sizeof(Termcell) * new_w * new_h);
   if (!ty->screen2)
     {
        ERR("memerr");
        free(new_screen);
        return;
     }
   new_back = calloc(sizeof(Termsave *), ty->backmax);

   y_end = ty->state.cy;
   new_y_end = new_h - 1;
   while ((y_end >= -ty->backscroll_num) && (new_y_end >= -ty->backmax))
     {
        y_start = termpty_line_find_top(ty, y_end);
        new_y_start = termpty_line_rewrap(ty, y_start, y_end, new_screen,
                                        new_back, new_w, new_y_end);
        y_end = y_start - 1;
        new_y_end = new_y_start - 1;
     }

   free(ty->screen);
   for (i = 1; i <= ty->backscroll_num; i++)
     termpty_save_free(ty->back[(ty->backpos - i + ty->backmax) % ty->backmax]);
   free(ty->back);

   ty->w = new_w;
   ty->h = new_h;
   ty->state.cy = MIN((new_h - 1) - new_y_start, new_h - 1);
   ty->state.cx = termpty_line_length(new_screen + ((new_h - 1) * new_w),
                                      new_w);
   ty->circular_offset = MAX(new_y_start, 0);
   ty->backpos = 0;
   ty->backscroll_num = MAX(-new_y_start, 0);
   ty->state.had_cr = 0;
   ty->screen = new_screen;
   ty->back = new_back;

   if (altbuf) termpty_screen_swap(ty);

   _limit_coord(ty, &(ty->state));
   _limit_coord(ty, &(ty->swap));
   _limit_coord(ty, &(ty->save));

   _pty_size(ty);

   termpty_save_thaw();
}

void
termpty_backscroll_set(Termpty *ty, int size)
{
   int i;

   if (ty->backmax == size) return;
   
   termpty_save_freeze();

   if (ty->back)
     {
        for (i = 0; i < ty->backmax; i++)
          {
             if (ty->back[i]) termpty_save_free(ty->back[i]);
          }
        free(ty->back);
     }
   if (size > 0)
     ty->back = calloc(1, sizeof(Termsave *) * size);
   else
     ty->back = NULL;
   ty->backscroll_num = 0;
   ty->backpos = 0;
   ty->backmax = size;
   termpty_save_thaw();
}

pid_t
termpty_pid_get(const Termpty *ty)
{
   return ty->pid;
}

void
termpty_block_free(Termblock *tb)
{
   char *s;
   if (tb->path) eina_stringshare_del(tb->path);
   if (tb->link) eina_stringshare_del(tb->link);
   if (tb->chid) eina_stringshare_del(tb->chid);
   if (tb->obj) evas_object_del(tb->obj);
   EINA_LIST_FREE(tb->cmds, s) free(s);
   free(tb);
}

Termblock *
termpty_block_new(Termpty *ty, int w, int h, const char *path, const char *link)
{
   Termblock *tb;
   int id;
   
   id = ty->block.curid;
   if (!ty->block.blocks)
     ty->block.blocks = eina_hash_int32_new((Eina_Free_Cb)termpty_block_free);
   if (!ty->block.blocks) return NULL;
   tb = eina_hash_find(ty->block.blocks, &id);
   if (tb)
     {
        if (tb->active)
          ty->block.active = eina_list_remove(ty->block.active, tb);
        eina_hash_del(ty->block.blocks, &id, tb);
     }
   tb = calloc(1, sizeof(Termblock));
   if (!tb) return NULL;
   tb->pty = ty;
   tb->id = id;
   tb->w = w;
   tb->h = h;
   tb->path = eina_stringshare_add(path);
   if (link) tb->link = eina_stringshare_add(link);
   eina_hash_add(ty->block.blocks, &id, tb);
   ty->block.curid++;
   if (ty->block.curid >= 8192) ty->block.curid = 0;
   return tb;
}

void
termpty_block_insert(Termpty *ty, int ch, Termblock *blk)
{
   // bit 0-8 = y (9b 0->511)
   // bit 9-17 = x (9b 0->511)
   // bit 18-30 = id (13b 0->8191)
   // bit 31 = 1
   // 
   // fg/bg = 8+8bit unused. (use for extra id bits? so 16 + 13 == 29bit?)
   // 
   // cp = (1 << 31) | ((id 0x1fff) << 18) | ((x & 0x1ff) << 9) | (y & 0x1ff);
   Termexp *ex;
   
   ex = calloc(1, sizeof(Termexp));
   if (!ex) return;
   ex->ch = ch;
   ex->left = blk->w * blk->h;
   ex->id = blk->id;
   ex->w = blk->w;
   ex->h = blk->h;
   ty->block.expecting = eina_list_append(ty->block.expecting, ex);
}

int
termpty_block_id_get(Termcell *cell, int *x, int *y)
{
   int id;
   
   if (!(cell->codepoint & 0x80000000)) return -1;
   id = (cell->codepoint >> 18) & 0x1fff;
   *x = (cell->codepoint >> 9) & 0x1ff;
   *y = cell->codepoint & 0x1ff;
   return id;
}

Termblock *
termpty_block_get(Termpty *ty, int id)
{
   if (!ty->block.blocks) return NULL;
   return eina_hash_find(ty->block.blocks, &id);
}

void
termpty_block_chid_update(Termpty *ty, Termblock *blk)
{
   if (!blk->chid) return;
   if (!ty->block.chid_map)
     ty->block.chid_map = eina_hash_string_superfast_new(NULL);
   if (!ty->block.chid_map) return;
   eina_hash_add(ty->block.chid_map, blk->chid, blk);
}

Termblock *
termpty_block_chid_get(Termpty *ty, const char *chid)
{
   Termblock *tb;
   
   tb = eina_hash_find(ty->block.chid_map, chid);
   return tb;
}

static void
_handle_block_codepoint_overwrite_heavy(Termpty *ty, int oldc, int newc)
{
   Termblock *tb;
   int ido = 0, idn = 0;

   if (oldc & 0x80000000) ido = (oldc >> 18) & 0x1fff;
   if (newc & 0x80000000) idn = (newc >> 18) & 0x1fff;
   if (((oldc & 0x80000000) && (newc & 0x80000000)) && (idn == ido)) return;
   
   if (oldc & 0x80000000)
     {
        tb = termpty_block_get(ty, ido);
        if (!tb) return;
        tb->refs--;
        if (tb->refs == 0)
          {
             if (tb->active)
               ty->block.active = eina_list_remove(ty->block.active, tb);
             eina_hash_del(ty->block.blocks, &ido, tb);
          }
     }
   
   if (newc & 0x80000000)
     {
        tb = termpty_block_get(ty, idn);
        if (!tb) return;
        tb->refs++;
     }
}

/* Try to trick the compiler into inlining the first test */
static inline void
_handle_block_codepoint_overwrite(Termpty *ty, Eina_Unicode oldc, Eina_Unicode newc)
{
   if (!((oldc | newc) & 0x80000000)) return;
   _handle_block_codepoint_overwrite_heavy(ty, oldc, newc);
}

void
termpty_cell_copy(Termpty *ty, Termcell *src, Termcell *dst, int n)
{
   int i;
   
   for (i = 0; i < n; i++)
     {
        _handle_block_codepoint_overwrite(ty, dst[i].codepoint, src[i].codepoint);
        dst[i] = src[i];
     }
}

void
termpty_screen_swap(Termpty *ty)
{
   Termcell *tmp_screen;
   int tmp_circular_offset;
   int tmp_appcursor = ty->state.appcursor;

   tmp_screen = ty->screen;
   ty->screen = ty->screen2;
   ty->screen2 = tmp_screen;

   if (ty->altbuf)
      ty->state = ty->swap;
   else
      ty->swap = ty->state;

   tmp_circular_offset = ty->circular_offset;
   ty->circular_offset = ty->circular_offset2;
   ty->circular_offset2 = tmp_circular_offset;

   ty->state.appcursor = tmp_appcursor;

   ty->altbuf = !ty->altbuf;

   if (ty->cb.cancel_sel.func)
     ty->cb.cancel_sel.func(ty->cb.cancel_sel.data);
}

void
termpty_cell_fill(Termpty *ty, Termcell *src, Termcell *dst, int n)
{
   int i;

   if (src)
     {
        for (i = 0; i < n; i++)
          {
             _handle_block_codepoint_overwrite(ty, dst[i].codepoint, src[0].codepoint);
             dst[i] = src[0];
          }
     }
   else
     {
        for (i = 0; i < n; i++)
          {
             _handle_block_codepoint_overwrite(ty, dst[i].codepoint, 0);
             memset(&(dst[i]), 0, sizeof(*dst));
          }
     }
}

void
termpty_cell_codepoint_att_fill(Termpty *ty, Eina_Unicode codepoint,
                                Termatt att, Termcell *dst, int n)
{
   Termcell local = { codepoint, att };
   int i;
   
   for (i = 0; i < n; i++)
     {
        _handle_block_codepoint_overwrite(ty, dst[i].codepoint, codepoint);
        dst[i] = local;
     }
}

Config *
termpty_config_get(const Termpty *ty)
{
   return termio_config_get(ty->obj);
}
