#ifndef FTP_H
#define FTP_H

#include "core.h"
#include "ui.h"

/* Minimal FTP server, lifted from EmuC0re. The Lua launcher pre-binds both
   sockets and hands the fds in, so this never calls socket() itself.
   Runs once at startup, uploads land in ROM_DIR, and the client releases it
   with SITE EXIT. */

#define FTP_PORT      1337
#define FTP_DATA_PORT 1338
#define FTP_DEST      ROM_DIR
#define FTP_BUF_SZ    8192

/* How long to wait for an FTP client before giving up and going to the ROM
   picker. ROMs usually arrive out of band (already in savedata or
   content_tmp), so this must never be an unbounded wait -- the first hardware
   run stranded the emulator on the waiting screen because it was. */
/* How long to wait for an FTP client before giving up and going to the
   picker. Short when ROMs are already in the container -- nobody wants to sit
   through a timeout on every launch -- and long when there are none, because
   then FTP is the only way to get a game in and the user needs time to reach
   their client. */
#define FTP_WAIT_MS         6000


/* Called roughly every 200ms while ftp_serve is waiting for a client, with
   the number of ROMs received so far. Return non-zero to stop waiting and
   return from ftp_serve.

   This exists so the caller can keep the screen alive and read the pad during
   what would otherwise be a blind blocking wait. It is also how the wait ends
   naturally: once a transfer completes and the client disconnects, the wait
   loop resumes, the callback sees a non-zero count and says stop. */
typedef int (*ftp_idle_fn)(void *user, int roms_so_far);

/* wait_ms <= 0 waits indefinitely, in which case only the idle callback can
   end it. */
int ftp_serve(s32 srv_fd, s32 data_listen_fd,
              void *G, void *D, void *load_mod, void *mmap,
              void *kopen, void *kwrite, void *kclose, void *kmkdir,
              void *getdents, void *usleep,
              void *recvfrom, void *sendto, void *accept,
              void *getsockname, void *poll, int wait_ms,
              s32 log_fd, u8 *log_sa, s32 userId,
              struct rom_entry *roms, int max_roms,
              ftp_idle_fn idle, void *idle_user);

#endif
