/* LogiFn: keeps Logitech wireless keyboards in function-key mode.
 *
 * Copyright (C) 2026 David Sharp
 * https://github.com/davidpsharp/LogiFn
 *
 * Built 100% by Claude, Anthropic's AI (Claude Code), from research to
 * code and testing, entirely from a Mac: it was tested on a Raspberry Pi
 * over the network with Rorth (https://github.com/davidpsharp/Rorth). No
 * RISC OS emulator was used, and nobody sat at a RISC OS machine to develop
 * it; David Sharp set the goal and pressed keys on the K400 Plus to test it.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License (the LICENSE file) for more details.
 *
 * Keyboards such as the K400 Plus send media/shortcut actions from F1-F12
 * unless Fn is held. The keyboard has an HID++ 2.0 "Fn inversion" feature
 * (0x40A2 on the K400 Plus; 0x40A0 and 0x40A3 on others) that swaps this,
 * but it forgets the setting whenever it is switched off or loses its link.
 *
 * LogiFn talks to the Unifying (or similar) receiver through its separate
 * HID++ interface, which RISC OS's own keyboard and mouse drivers leave
 * alone. It asks the receiver to report devices connecting and
 * disconnecting, and each time a keyboard links up it finds the keyboard's
 * Fn-inversion feature and sets it.
 *
 * Requests go out as HID SET_REPORT control transfers (DeviceFS_CallDevice,
 * USB Control Request); replies and notifications come back on the
 * interface's interrupt IN endpoint, read through a DeviceFS stream.
 *
 * Everything runs in a transient callback queued by a 10 cs ticker; the
 * ticker itself does nothing else. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"
#include "ModHdr.h"

#define Service_DeviceDead      0x79
#define Service_USB             0xD2
#define USB_Attach              0
#define USB_Connected           1
#define USB_DriverStarting      3
#define USB_DriverDying         4

#define DeviceCall_USBRequest   0x80000000u

#define LOGITECH_VID    0x046D
#define MAX_RECEIVERS   4
#define SLOTS           6         /* devices 1-6 on a receiver */
#define SWID            0x0B      /* our software id in HID++ 2.0 requests */
#define TICK_CS         10
#define REPLY_CS        200       /* a reply must come within this */
#define TRIES           3
#define SETTLE_CS       30        /* after a keyboard links up, before talking to it */
#define OPEN_DELAY_CS   50        /* after a receiver attaches, before opening it */

#define HIDPP_SHORT     0x10      /* report IDs and lengths */
#define HIDPP_LONG      0x11
#define DJ_SHORT        0x20
#define DJ_LONG         0x21
#define RECEIVER        0xFF      /* device index of the receiver itself */
#define SET_REG         0x80      /* HID++ 1.0 sub-ids */
#define GET_REG         0x81
#define ERR_10          0x8F      /* HID++ 1.0 error */
#define ERR_20          0xFF      /* HID++ 2.0 error */
#define CONNECTION      0x41      /* device connection notification */
#define ERR_UNKNOWN_DEV 0x08      /* HID++ 1.0 errors that mean "not reachable now" */
#define ERR_RESOURCE    0x09

enum { MODE_FKEYS = 0, MODE_MEDIA = 1 };

enum {                            /* what a slot is doing */
  SL_EMPTY,                       /* nothing heard of */
  SL_OFFLINE,                     /* paired, but not linked or asleep */
  SL_PROBE,                       /* to find its Fn feature */
  SL_SET,                         /* to set the mode */
  SL_DONE,                        /* set */
  SL_NOFN,                        /* has no Fn feature we know */
  SL_OTHER                        /* not a keyboard */
};
static const char *const sl_names[] = {
  "empty", "offline", "looking", "setting", "set", "no Fn feature", "not a keyboard"
};

enum { RQ_GETNOTIF, RQ_SETNOTIF, RQ_ANNOUNCE, RQ_GETFEAT, RQ_SETFN };

static const unsigned fn_features[] = { 0x40A2, 0x40A0, 0x40A3 };
#define N_FEATURES (int)(sizeof fn_features / sizeof fn_features[0])

typedef struct {
  int state;
  int kind;                       /* device type from the connection notification */
  unsigned wpid;                  /* wireless product id */
  int online;
  int probe;                      /* which of fn_features is being asked for */
  int fidx;                       /* its feature index, 0 if unknown */
  unsigned fid;
  unsigned due;                   /* when to send the next request */
  int fails;
  int mode;                       /* mode last set */
  unsigned sets;                  /* times set */
} slot;

typedef struct {
  int used;
  int dead;                       /* the device has gone; close when safe */
  char name[20];                  /* DeviceFS name, e.g. USB4 */
  unsigned product;
  int iface, ep;
  int handle;                     /* FileSwitch handle of the stream, 0 if closed */
  unsigned open_at;
  int open_fails;
  int setup;                      /* next setup request (RQ_GETNOTIF..RQ_ANNOUNCE), or 3 when done */
  unsigned char notif[3];         /* the receiver's notification flags */
  unsigned char rx[64];
  int rxlen;
  struct {
    int active, kind, slot, tries;
    unsigned char msg[7];
    unsigned sent_at;
  } req;
  slot sl[SLOTS + 1];
} receiver;

static receiver rcv[MAX_RECEIVERS];
static void *g_pw;
static volatile int cb_pending;
static int in_callback;
static int scan_wanted;
static unsigned scan_at;
static int want_mode = MODE_FKEYS;

/* -------------------------------------------------------------------------
 * Event log, for *LogiFn */

#define LOG_LINES 16
#define LOG_LEN   80
static char log_text[LOG_LINES][LOG_LEN];
static unsigned log_time[LOG_LINES];
static int log_next, log_count;

static unsigned now_cs(void)
{
  unsigned t = 0;
  _swix(OS_ReadMonotonicTime, _OUT(0), &t);
  return t;
}

static int reached(unsigned t) { return (int)(now_cs() - t) >= 0; }

static void note(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(log_text[log_next], LOG_LEN, fmt, ap);
  va_end(ap);
  log_time[log_next] = now_cs();
  log_next = (log_next + 1) % LOG_LINES;
  if (log_count < LOG_LINES) log_count++;
}

/* -------------------------------------------------------------------------
 * Scheduling */

static void schedule(void)
{
  if (cb_pending) return;
  cb_pending = 1;
  if (_swix(OS_AddCallBack, _INR(0,1), lf_callback_entry, g_pw)) cb_pending = 0;
}

static int busy(void)
{
  int i;
  if (scan_wanted) return 1;
  for (i = 0; i < MAX_RECEIVERS; i++) if (rcv[i].used) return 1;
  return 0;
}

_kernel_oserror *lf_ticker_handler(_kernel_swi_regs *r, void *pw)
{
  (void)r; (void)pw;
  if (busy()) schedule();
  return NULL;
}

/* -------------------------------------------------------------------------
 * Receivers */

static int same_name(const char *a, const char *b)
{
  for (;; a++, b++) {
    int x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return 0;
    if (x < ' ') return 1;
  }
}

static void close_receiver(receiver *r)
{
  if (r->handle) _swix(OS_Find, _INR(0,1), 0, r->handle);
  r->handle = 0;
  r->used = 0;
}

/* A USBServiceCall block (USBDevFS.h): the device's name at 4, its
 * descriptors from 32, the whole block sclen bytes long. Looks for a
 * Logitech device with an HID interface of class 3/0/0 (not a boot keyboard
 * or mouse) with an interrupt IN endpoint big enough for HID++ long reports. */
static void consider(const unsigned char *svc, unsigned delay)
{
  unsigned len = svc[0] | svc[1] << 8, pos = 32;
  const char *name = (const char *)svc + 4;
  unsigned vid, pid;
  int iface = -1, i;

  if (len < 32 + 18) return;
  vid = svc[32 + 8] | svc[32 + 9] << 8;
  pid = svc[32 + 10] | svc[32 + 11] << 8;
  if (vid != LOGITECH_VID) return;
  for (i = 0; i < MAX_RECEIVERS; i++)
    if (rcv[i].used && same_name(rcv[i].name, name)) return;

  while (pos + 2 <= len) {
    const unsigned char *d = svc + pos;
    if (d[0] < 2 || pos + d[0] > len) break;
    if (d[1] == 4 && d[0] >= 9) {                 /* interface */
      iface = (d[5] == 3 && d[6] == 0 && d[7] == 0) ? d[2] : -1;
    } else if (d[1] == 5 && d[0] >= 7 && iface >= 0) {   /* endpoint */
      unsigned maxp = d[4] | d[5] << 8;
      if ((d[2] & 0x80) && (d[3] & 3) == 3 && maxp >= 20) {
        for (i = 0; i < MAX_RECEIVERS && rcv[i].used; i++) ;
        if (i == MAX_RECEIVERS) return;
        memset(&rcv[i], 0, sizeof rcv[i]);
        strncpy(rcv[i].name, name, sizeof rcv[i].name - 1);
        rcv[i].product = pid;
        rcv[i].iface = iface;
        rcv[i].ep = d[2] & 0x0F;
        rcv[i].open_at = now_cs() + delay;
        rcv[i].used = 1;
        return;
      }
    }
    pos += d[0];
  }
}

/* Asks USBDriver for every device connected now. */
static void scan(void)
{
  _kernel_swi_regs r;
  const unsigned char *p;
  memset(&r, 0, sizeof r);
  r.r[0] = USB_Connected;
  r.r[1] = Service_USB;
  r.r[2] = 0;
  _kernel_swi(OS_ServiceCall, &r, &r);
  p = (const unsigned char *)r.r[2];
  while (p) {
    const unsigned char *next = *(const unsigned char *const *)p;
    consider(p + 4, 0);
    _swix(OS_Module, _INR(0,2), 7, 0, p);
    p = next;
  }
}

/* -------------------------------------------------------------------------
 * HID++ */

static int send(receiver *r, int kind, int slotno, unsigned char a, unsigned char b,
                unsigned char c, unsigned char d, unsigned char e)
{
  unsigned char *m = r->req.msg;
  _kernel_oserror *err;
  m[0] = HIDPP_SHORT; m[1] = (unsigned char)slotno; m[2] = a; m[3] = b;
  m[4] = c; m[5] = d; m[6] = e;
  r->req.kind = kind;
  r->req.slot = slotno;
  r->req.tries = 0;
  r->req.active = 1;
  /* SET_REPORT (class, interface): output report 0x10, 7 bytes */
  err = _swix(DeviceFS_CallDevice, _INR(0,1)|_INR(3,6), DeviceCall_USBRequest, r->name,
              0x21 | 0x09 << 8 | (0x0200 | HIDPP_SHORT) << 16, r->iface | 7 << 16, m, 0);
  r->req.sent_at = now_cs();
  if (err) {
    note("%s: send failed: %s", r->name, err->errmess);
    return 0;
  }
  return 1;
}

static void resend(receiver *r)
{
  int tries = r->req.tries;
  unsigned char *m = r->req.msg;
  send(r, r->req.kind, r->req.slot, m[2], m[3], m[4], m[5], m[6]);
  r->req.tries = tries + 1;
}

static void set_fn(receiver *r, int n)
{
  slot *s = &r->sl[n];
  unsigned char fn = 0x10 | SWID;                 /* function 1: set */
  if (s->fid == 0x40A3)                           /* host index first: 0xFF is this host */
    send(r, RQ_SETFN, n, (unsigned char)s->fidx, fn, 0xFF, (unsigned char)want_mode, 0);
  else
    send(r, RQ_SETFN, n, (unsigned char)s->fidx, fn, (unsigned char)want_mode, 0, 0);
}

static void get_feature(receiver *r, int n)
{
  unsigned f = fn_features[r->sl[n].probe];
  send(r, RQ_GETFEAT, n, 0x00, 0x00 | SWID, (unsigned char)(f >> 8), (unsigned char)f, 0);
}

static void connection(receiver *r, const unsigned char *p)
{
  int n = p[1];
  slot *s = &r->sl[n];
  int up = !(p[4] & 0x40);
  unsigned wpid = p[5] | p[6] << 8;
  int kind = p[4] & 0x0F;

  if (!up) {
    if (s->online) note("%s.%d (%04X): link lost", r->name, n, wpid);
    s->online = 0;
    if (s->state == SL_EMPTY || wpid != s->wpid) {
      s->wpid = wpid;
      s->kind = kind;
      s->fidx = 0;
      s->probe = 0;
      s->sets = 0;
      s->state = (kind == 1 || kind == 0) ? SL_OFFLINE : SL_OTHER;
    } else if (s->state != SL_NOFN && s->state != SL_OTHER) {
      s->state = SL_OFFLINE;
    }
    return;
  }
  note("%s.%d (%04X): linked", r->name, n, wpid);
  s->online = 1;
  s->fails = 0;
  s->due = now_cs() + SETTLE_CS;
  if (wpid != s->wpid || s->state == SL_EMPTY) {
    s->wpid = wpid;
    s->kind = kind;
    s->fidx = 0;
    s->probe = 0;
    s->sets = 0;
    s->state = (kind == 1 || kind == 0) ? SL_PROBE : SL_OTHER;   /* 1: keyboard */
  } else if (s->state == SL_NOFN || s->state == SL_OTHER) {
    /* known not to have one */
  } else {
    s->state = s->fidx ? SL_SET : SL_PROBE;
  }
}

/* Gives up on a slot until it next links up. */
static void unreachable(receiver *r, int n)
{
  slot *s = &r->sl[n];
  if (s->state == SL_PROBE || s->state == SL_SET) s->state = SL_OFFLINE;
}

static void reply(receiver *r, const unsigned char *p, int err)
{
  int n = r->req.slot;
  slot *s = &r->sl[n];
  r->req.active = 0;

  switch (r->req.kind) {
    case RQ_GETNOTIF:
      if (!err) memcpy(r->notif, p + 4, 3);
      r->setup = RQ_SETNOTIF;
      break;
    case RQ_SETNOTIF:
      if (err) note("%s: notifications refused (%02X)", r->name, err);
      r->setup = RQ_ANNOUNCE;
      break;
    case RQ_ANNOUNCE:
      if (err) note("%s: device announcement refused (%02X)", r->name, err);
      else note("%s: watching receiver %04X:%04X", r->name, LOGITECH_VID, r->product);
      r->setup = RQ_ANNOUNCE + 1;
      break;
    case RQ_GETFEAT:
      if (err == ERR_UNKNOWN_DEV || err == ERR_RESOURCE) {
        unreachable(r, n);
      } else if (err || p[4] == 0) {
        if (err || ++s->probe == N_FEATURES) {
          s->state = SL_NOFN;
          note("%s.%d (%04X): no Fn feature", r->name, n, s->wpid);
        }
      } else {
        s->fid = fn_features[s->probe];
        s->fidx = p[4];
        s->state = SL_SET;
        note("%s.%d (%04X): feature %04X at %d", r->name, n, s->wpid, s->fid, s->fidx);
      }
      break;
    case RQ_SETFN:
      if (err == ERR_UNKNOWN_DEV || err == ERR_RESOURCE) {
        unreachable(r, n);
      } else if (err) {
        note("%s.%d (%04X): set refused (%02X)", r->name, n, s->wpid, err);
        s->fidx = 0;
        s->probe = 0;
        s->state = ++s->fails >= TRIES ? SL_NOFN : SL_PROBE;
      } else {
        s->state = SL_DONE;
        s->mode = want_mode;
        s->sets++;
        note("%s.%d (%04X): set to %s", r->name, n, s->wpid,
             want_mode == MODE_FKEYS ? "F-keys" : "media keys");
      }
      break;
  }
}

static void timed_out(receiver *r)
{
  r->req.active = 0;
  if (r->req.kind <= RQ_ANNOUNCE) {
    note("%s: no answer to setup step %d", r->name, r->req.kind);
    r->setup = r->req.kind + 1;
  } else {
    unreachable(r, r->req.slot);
  }
}

/* One HID++ report from the receiver. */
static void packet(receiver *r, const unsigned char *p)
{
  const unsigned char *m = r->req.msg;
  if (p[0] != HIDPP_SHORT && p[0] != HIDPP_LONG) return;
  if (p[2] == CONNECTION && p[1] >= 1 && p[1] <= SLOTS) {
    connection(r, p);
    return;
  }
  if (!r->req.active || p[1] != m[1]) return;
  if (p[2] == m[2] && p[3] == m[3]) {
    reply(r, p, 0);
  } else if ((p[2] == ERR_10 || p[2] == ERR_20) && p[3] == m[2] && p[4] == m[3]) {
    reply(r, p, p[5] ? p[5] : 0xFF);
  }
}

static void read_stream(receiver *r)
{
  int avail = 0, i;
  if (_swix(OS_Args, _INR(0,1)|_OUT(2), 2, r->handle, &avail)) return;
  while (avail-- > 0 && r->rxlen < (int)sizeof r->rx) {
    int c = 0;
    if (_swix(OS_BGet, _IN(1)|_OUT(0), r->handle, &c)) return;
    r->rx[r->rxlen++] = (unsigned char)c;
  }
  /* split into reports by their IDs; zeros are padding */
  i = 0;
  while (i < r->rxlen) {
    int id = r->rx[i], len;
    len = id == HIDPP_SHORT ? 7 : id == HIDPP_LONG ? 20 : id == DJ_SHORT ? 15 : id == DJ_LONG ? 32 : 1;
    if (i + len > r->rxlen) break;                /* the rest is still to come */
    if (len > 1) packet(r, r->rx + i);
    i += len;
  }
  memmove(r->rx, r->rx + i, r->rxlen - i);
  r->rxlen -= i;
  if (r->rxlen == (int)sizeof r->rx) r->rxlen = 0;   /* nonsense: start again */
}

static void open_receiver(receiver *r)
{
  char path[96];
  _kernel_oserror *e;
  int h = 0;
  snprintf(path, sizeof path, "devices#interface%d;endpoint%d;interrupt:%s", r->iface, r->ep, r->name);
  e = _swix(OS_Find, _INR(0,1)|_OUT(0), 0x4F, path, &h);
  if (e || !h) {
    if (++r->open_fails >= 5) {
      note("%s: can't open: %s", r->name, e ? e->errmess : "?");
      r->used = 0;
    } else {
      r->open_at = now_cs() + 100;
    }
    return;
  }
  r->handle = h;
  r->setup = RQ_GETNOTIF;
}

/* Starts the next request on a receiver, if there's one due. */
static void next_request(receiver *r)
{
  int n;
  switch (r->setup) {
    case RQ_GETNOTIF:
      send(r, RQ_GETNOTIF, RECEIVER, GET_REG, 0x00, 0, 0, 0);
      return;
    case RQ_SETNOTIF:                             /* add wireless notifications (bit 0 of r1) */
      send(r, RQ_SETNOTIF, RECEIVER, SET_REG, 0x00, r->notif[0], r->notif[1] | 0x01, r->notif[2]);
      return;
    case RQ_ANNOUNCE:                             /* report every connected device now */
      send(r, RQ_ANNOUNCE, RECEIVER, SET_REG, 0x02, 0x02, 0, 0);
      return;
  }
  for (n = 1; n <= SLOTS; n++) {
    slot *s = &r->sl[n];
    if ((s->state == SL_PROBE || s->state == SL_SET) && reached(s->due)) {
      if (s->state == SL_PROBE) get_feature(r, n);
      else set_fn(r, n);
      return;
    }
  }
}

static void work(receiver *r)
{
  if (r->dead) {
    close_receiver(r);
    return;
  }
  if (!r->handle) {
    if (reached(r->open_at)) open_receiver(r);
    if (!r->handle) return;
  }
  read_stream(r);
  if (r->req.active && reached(r->req.sent_at + REPLY_CS)) {
    if (r->req.tries + 1 < TRIES) resend(r);
    else timed_out(r);
  }
  if (!r->req.active) next_request(r);
}

_kernel_oserror *lf_callback_handler(_kernel_swi_regs *regs, void *pw)
{
  int i;
  (void)regs; (void)pw;
  cb_pending = 0;
  if (in_callback) return NULL;
  in_callback = 1;
  if (scan_wanted && reached(scan_at)) {
    scan_wanted = 0;
    scan();
  }
  for (i = 0; i < MAX_RECEIVERS; i++)
    if (rcv[i].used) work(&rcv[i]);
  in_callback = 0;
  return NULL;
}

/* -------------------------------------------------------------------------
 * Module entry points */

_kernel_oserror *lf_init(const char *tail, int podule_base, void *pw)
{
  _kernel_oserror *e;
  (void)tail; (void)podule_base;
  g_pw = pw;
  scan_wanted = 1;
  scan_at = now_cs();
  e = _swix(OS_CallEvery, _INR(0,2), TICK_CS - 1, lf_ticker_entry, pw);
  if (e) return e;
  schedule();
  return NULL;
}

_kernel_oserror *lf_final(int fatal, int podule, void *pw)
{
  int i;
  (void)fatal; (void)podule;
  _swix(OS_RemoveTickerEvent, _INR(0,1), lf_ticker_entry, pw);
  _swix(OS_RemoveCallBack, _INR(0,1), lf_callback_entry, pw);
  for (i = 0; i < MAX_RECEIVERS; i++) if (rcv[i].used) close_receiver(&rcv[i]);
  return NULL;
}

void lf_service(int service_number, _kernel_swi_regs *r, void *pw)
{
  int i;
  (void)pw;
  if (service_number == Service_USB) {
    switch (r->r[0]) {
      case USB_Attach:
        if (r->r[2]) consider((const unsigned char *)r->r[2], OPEN_DELAY_CS);
        break;
      case USB_DriverStarting:
        scan_wanted = 1;
        scan_at = now_cs() + 100;
        break;
      case USB_DriverDying:
        for (i = 0; i < MAX_RECEIVERS; i++) {
          if (!rcv[i].used) continue;
          if (in_callback) rcv[i].dead = 1;
          else close_receiver(&rcv[i]);
        }
        break;
    }
  } else if (service_number == Service_DeviceDead && r->r[3]) {
    const char *name = (const char *)r->r[3];
    for (i = 0; i < MAX_RECEIVERS; i++) {
      if (!rcv[i].used || !same_name(rcv[i].name, name)) continue;
      if (in_callback) rcv[i].dead = 1;
      else close_receiver(&rcv[i]);
    }
  }
}

static void show_status(void)
{
  int i, n, any = 0;
  unsigned t = now_cs();
  printf("LogiFn: F1-F12 set to %s\n",
         want_mode == MODE_FKEYS ? "function keys (FKeys)" : "media keys (Media)");
  for (i = 0; i < MAX_RECEIVERS; i++) {
    receiver *r = &rcv[i];
    if (!r->used) continue;
    any = 1;
    printf("%s: receiver %04X:%04X, HID++ on interface %d, %s\n", r->name, LOGITECH_VID,
           r->product, r->iface, r->handle ? (r->setup > RQ_ANNOUNCE ? "watching" : "starting")
                                           : "not open yet");
    for (n = 1; n <= SLOTS; n++) {
      slot *s = &r->sl[n];
      if (s->state == SL_EMPTY) continue;
      printf("  %d: %04X %s, %s", n, s->wpid, s->online ? "linked" : "not linked", sl_names[s->state]);
      if (s->fidx) printf(", feature %04X at %d", s->fid, s->fidx);
      if (s->sets) printf(", set %u time%s", s->sets, s->sets == 1 ? "" : "s");
      printf("\n");
    }
  }
  if (!any) printf("No Logitech receivers with an HID++ interface found.\n");
  if (log_count) {
    int k = (log_next - log_count + LOG_LINES) % LOG_LINES;
    printf("Recent events:\n");
    for (i = 0; i < log_count; i++, k = (k + 1) % LOG_LINES)
      printf("  %5us ago  %s\n", (t - log_time[k]) / 100, log_text[k]);
  }
}

_kernel_oserror *lf_command(const char *arg_string, int argc, int cmd_no, void *pw)
{
  static _kernel_oserror syntax = { 0, "Syntax: *LogiFnMode [FKeys|Media]" };
  (void)pw;
  if (cmd_no == CMD_LogiFn) {
    show_status();
    return NULL;
  }
  if (cmd_no == CMD_LogiFnMode) {
    char word[8];
    int i, n, mode;
    if (argc == 0) {
      printf("%s\n", want_mode == MODE_FKEYS ? "FKeys" : "Media");
      return NULL;
    }
    for (i = 0; i < 7 && arg_string[i] > ' '; i++) word[i] = arg_string[i] | 0x20;
    word[i] = '\0';
    if (strcmp(word, "fkeys") == 0) mode = MODE_FKEYS;
    else if (strcmp(word, "media") == 0) mode = MODE_MEDIA;
    else return &syntax;
    want_mode = mode;
    for (i = 0; i < MAX_RECEIVERS; i++)
      for (n = 1; n <= SLOTS; n++)
        if (rcv[i].used && rcv[i].sl[n].state == SL_DONE && rcv[i].sl[n].mode != mode) {
          rcv[i].sl[n].state = SL_SET;
          rcv[i].sl[n].due = now_cs();
        }
    schedule();
  }
  return NULL;
}
