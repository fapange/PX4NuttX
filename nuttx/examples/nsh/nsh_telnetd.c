/****************************************************************************
 * examples/nsh/nsh_telnetd.c
 *
 *   Copyright (C) 2007, 2008 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <spudmonkey@racsa.co.cr>
 *
 * This is a leverage of similar logic from uIP:
 *
 *   Author: Adam Dunkels <adam@sics.se>
 *   Copyright (c) 2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <debug.h>

#include <net/if.h>
#include <net/uip/uip-lib.h>

#include "nsh.h"

/****************************************************************************
 * Definitions
 ****************************************************************************/

#define ISO_nl       0x0a
#define ISO_cr       0x0d

#define STATE_NORMAL 0
#define STATE_IAC    1
#define STATE_WILL   2
#define STATE_WONT   3
#define STATE_DO     4
#define STATE_DONT   5
#define STATE_CLOSE  6

#define TELNET_IAC   255
#define TELNET_WILL  251
#define TELNET_WONT  252
#define TELNET_DO    253
#define TELNET_DONT  254

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct telnetio_s
{
  int    tio_sockfd;
  uint16 tio_sndlen;
  uint8  tio_bufndx;
  uint8  tio_state;
  char   tio_buffer[CONFIG_EXAMPLES_NSH_IOBUFFER_SIZE];
};

struct redirect_s
{
  int    rd_fd;      /* Re-direct file descriptor */
  FILE  *rd_stream;  /* Re-direct stream */
};

struct telnetsave_s
{
  boolean ts_redirected;
  union
    {
      struct telnetio_s *tn;
      struct redirect_s  rd;
    } u;
};

struct telnetd_s
{
  struct nsh_vtbl_s tn_vtbl;
  boolean           tn_redirected;
  int               tn_refs;    /* Reference counts on the instance */
  union
    {
      struct telnetio_s *tn;
      struct redirect_s  rd;
    } u;
  char tn_cmd[CONFIG_EXAMPLES_NSH_LINELEN];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static FAR struct nsh_vtbl_s *nsh_telnetclone(FAR struct nsh_vtbl_s *vtbl);
static void nsh_telnetaddref(FAR struct nsh_vtbl_s *vtbl);
static void nsh_telnetrelease(FAR struct nsh_vtbl_s *vtbl);
static int nsh_telnetoutput(FAR struct nsh_vtbl_s *vtbl, const char *fmt, ...);
static int nsh_redirectoutput(FAR struct nsh_vtbl_s *vtbl, const char *fmt, ...);
static FAR char *nsh_telnetlinebuffer(FAR struct nsh_vtbl_s *vtbl);
static void nsh_telnetredirect(FAR struct nsh_vtbl_s *vtbl, int fd, FAR ubyte *save);
static void nsh_telnetundirect(FAR struct nsh_vtbl_s *vtbl, FAR ubyte *save);
static void nsh_telnetexit(FAR struct nsh_vtbl_s *vtbl);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nsh_dumpbuffer
 *
 * Description:
 *   Dump a buffer of data (debug only)
 *
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_NSH_TELNETD_DUMPBUFFER
static void nsh_dumpbuffer(const char *msg, const char *buffer, ssize_t nbytes)
{
#ifdef CONFIG_DEBUG
  char line[128];
  int ch;
  int i;
  int j;

  dbg("%s:\n", msg);
  for (i = 0; i < nbytes; i += 16)
    {
      sprintf(line, "%04x: ", i);

      for ( j = 0; j < 16; j++)
        {
          if (i + j < nbytes)
            {
              sprintf(&line[strlen(line)], "%02x ", buffer[i+j] );
            }
          else
            {
              strcpy(&line[strlen(line)], "   ");
            }
        }

      for ( j = 0; j < 16; j++)
        {
          if (i + j < nbytes)
            {
              ch = buffer[i+j];
              sprintf(&line[strlen(line)], "%c", ch >= 0x20 && ch <= 0x7e ? ch : '.');
            }
        }
      dbg("%s\n", line);
    }
#endif
}
#else
# define nsh_dumpbuffer(msg,buffer,nbytes)
#endif

/****************************************************************************
 * Name: nsh_allocstruct
 ****************************************************************************/

static FAR struct telnetd_s *nsh_allocstruct(void)
{
  struct telnetd_s *pstate = (struct telnetd_s *)malloc(sizeof(struct telnetd_s));
  if (pstate)
    {
      memset(pstate, 0, sizeof(struct telnetd_s));
      pstate->tn_vtbl.clone      = nsh_telnetclone;
      pstate->tn_vtbl.addref     = nsh_telnetaddref;
      pstate->tn_vtbl.release    = nsh_telnetrelease;
      pstate->tn_vtbl.output     = nsh_telnetoutput;
      pstate->tn_vtbl.linebuffer = nsh_telnetlinebuffer;
      pstate->tn_vtbl.redirect   = nsh_telnetredirect;
      pstate->tn_vtbl.undirect   = nsh_telnetundirect;
      pstate->tn_vtbl.exit       = nsh_telnetexit;

      memset(&pstate->tn_vtbl.np, 0, sizeof(struct nsh_parser_s));

      pstate->tn_refs            = 1;
    }
  return pstate;
}

/****************************************************************************
 * Name: nsh_openifnotopen
 ****************************************************************************/

static int nsh_openifnotopen(struct telnetd_s *pstate)
{
  struct redirect_s *rd = &pstate->u.rd;

  /* The stream is open in a lazy fashion.  This is done because the file
   * descriptor may be opened on a different task than the stream.
   */

  if (!rd->rd_stream)
    {
      rd->rd_stream = fdopen(rd->rd_fd, "w");
      if (!rd->rd_stream)
        {
          return ERROR;
        }
    }
  return 0;
}

/****************************************************************************
 * Name: nsh_closeifnotclosed
 ****************************************************************************/

static void nsh_closeifnotclosed(struct telnetd_s *pstate)
{
  struct redirect_s *rd = &pstate->u.rd;

  if (rd->rd_stream == stdout)
    {
      fflush(stdout);
      rd->rd_fd = 1;
    }
  else
    {
      if (rd->rd_stream)
        {
          fflush(rd->rd_stream);
          fclose(rd->rd_stream);
        }
      else if (rd->rd_fd >= 0 && rd->rd_fd != 1)
        {
          close(rd->rd_fd);
        }

      rd->rd_fd     = -1;
      rd->rd_stream = NULL;
    }
}

/****************************************************************************
 * Name: nsh_putchar
 *
 * Description:
 *   Add another parsed character to the TELNET command string
 *
 ****************************************************************************/

static void nsh_putchar(struct telnetd_s *pstate, uint8 ch)
{
  struct telnetio_s *tio = pstate->u.tn;

  /* Ignore carriage returns */

  if (ch == ISO_cr)
  {
    return;
  }

  /* Add all other characters to the cmd buffer */

  pstate->tn_cmd[tio->tio_bufndx] = ch;

  /* If a newline was added or if the buffer is full, then process it now */

  if (ch == ISO_nl || tio->tio_bufndx == (CONFIG_EXAMPLES_NSH_LINELEN - 1))
    {
      if (tio->tio_bufndx > 0)
        {
          pstate->tn_cmd[tio->tio_bufndx] = '\0';
        }

      nsh_dumpbuffer("TELNET CMD", pstate->tn_cmd, strlen(pstate->tn_cmd));
      nsh_parse(&pstate->tn_vtbl, pstate->tn_cmd);
      tio->tio_bufndx = 0;
    }
  else
    {
      tio->tio_bufndx++;
      vdbg("Add '%c', bufndx=%d\n", ch, tio->tio_bufndx);
    }
}

/****************************************************************************
 * Name: nsh_sendopt
 *
 * Description:
 *
 ****************************************************************************/

static void nsh_sendopt(struct telnetd_s *pstate, uint8 option, uint8 value)
{
  struct telnetio_s *tio = pstate->u.tn;
  uint8 optbuf[4];
  optbuf[0] = TELNET_IAC;
  optbuf[1] = option;
  optbuf[2] = value;
  optbuf[3] = 0;

  nsh_dumpbuffer("Send optbuf", optbuf, 4);
  if (send(tio->tio_sockfd, optbuf, 4, 0) < 0)
    {
      dbg("[%d] Failed to send TELNET_IAC\n", tio->tio_sockfd);
    }
}

/****************************************************************************
 * Name: nsh_flush
 *
 * Description:
 *   Dump the buffered output info.
 *
 ****************************************************************************/

static void nsh_flush(FAR struct telnetd_s *pstate)
{
  struct telnetio_s *tio = pstate->u.tn;

  if (tio->tio_sndlen > 0)
    {
      nsh_dumpbuffer("Shell output", tio->tio_buffer, tio->tio_sndlen);
      if (send(tio->tio_sockfd, tio->tio_buffer, tio->tio_sndlen, 0) < 0)
        {
          dbg("[%d] Failed to send response\n", tio->tio_sockfd);
        }
    }
  tio->tio_sndlen = 0;
}

/****************************************************************************
 * Name: nsh_receive
 *
 * Description:
 *   Process a received TELENET buffer
 *
 ****************************************************************************/

static int nsh_receive(struct telnetd_s *pstate, size_t len)
{
  struct telnetio_s *tio = pstate->u.tn;
  char              *ptr = tio->tio_buffer;
  uint8 ch;

  while (len > 0)
    {
      ch = *ptr++;
      len--;

      vdbg("ch=%02x state=%d\n", ch, tio->tio_state);
      switch (tio->tio_state)
        {
          case STATE_IAC:
            if (ch == TELNET_IAC)
              {
                nsh_putchar(pstate, ch);
                tio->tio_state = STATE_NORMAL;
             }
            else
              {
                switch (ch)
                  {
                    case TELNET_WILL:
                      tio->tio_state = STATE_WILL;
                      break;

                    case TELNET_WONT:
                      tio->tio_state = STATE_WONT;
                      break;

                    case TELNET_DO:
                      tio->tio_state = STATE_DO;
                      break;

                    case TELNET_DONT:
                      tio->tio_state = STATE_DONT;
                      break;

                    default:
                      tio->tio_state = STATE_NORMAL;
                      break;
                  }
              }
            break;

          case STATE_WILL:
            /* Reply with a DONT */

            nsh_sendopt(pstate, TELNET_DONT, ch);
            tio->tio_state = STATE_NORMAL;
            break;

          case STATE_WONT:
            /* Reply with a DONT */

            nsh_sendopt(pstate, TELNET_DONT, ch);
            tio->tio_state = STATE_NORMAL;
            break;

          case STATE_DO:
            /* Reply with a WONT */

            nsh_sendopt(pstate, TELNET_WONT, ch);
            tio->tio_state = STATE_NORMAL;
            break;

          case STATE_DONT:
            /* Reply with a WONT */

            nsh_sendopt(pstate, TELNET_WONT, ch);
            tio->tio_state = STATE_NORMAL;
            break;

          case STATE_NORMAL:
            if (ch == TELNET_IAC)
              {
                tio->tio_state = STATE_IAC;
              }
            else
              {
                nsh_putchar(pstate, ch);
              }
            break;
        }
    }
  return OK;
}

/****************************************************************************
 * Name: nsh_connection
 *
 * Description:
 *   Each time a new connection to port 23 is made, a new thread is created
 *   that begins at this entry point.  There should be exactly one argument
 *   and it should be the socket descriptor (+1).
 *
 ****************************************************************************/

static void *nsh_connection(void *arg)
{
  struct telnetd_s  *pstate = nsh_allocstruct();
  struct telnetio_s *tio    = (struct telnetio_s *)malloc(sizeof(struct telnetio_s));
  int                sockfd = (int)arg;
  int                ret    = ERROR;

  dbg("[%d] Started\n", sockfd);

  /* Verify that the state structure was successfully allocated */

  if (pstate && tio)
    {
      /* Initialize the thread state structure */

      tio->tio_sockfd = sockfd;
      tio->tio_state  = STATE_NORMAL;
      pstate->u.tn    = tio;

      /* Output a greeting */

      nsh_output(&pstate->tn_vtbl, "NuttShell (NSH)\n");

      /* Loop processing each TELNET command */

      do
        {
          /* Display the prompt string */

          nsh_output(&pstate->tn_vtbl, g_nshprompt);
          nsh_flush(pstate);

          /* Read a buffer of data from the TELNET client */

          ret = recv(tio->tio_sockfd, tio->tio_buffer, CONFIG_EXAMPLES_NSH_IOBUFFER_SIZE, 0);
          if (ret > 0)
            {

              /* Process the received TELNET data */

              nsh_dumpbuffer("Received buffer", pstate->u.tn.tio_buffer, ret);
              ret = nsh_receive(pstate, ret);
            }
        }
      while (ret >= 0 && tio->tio_state != STATE_CLOSE);
      dbg("[%d] ret=%d tn.tio_state=%d\n", sockfd, ret, tio->tio_state);

      /* End of command processing -- Clean up and exit */
    }

  /* Exit the task */

  if (pstate)
    {
      free(pstate);
    }

  if (tio)
    {
      free(tio);
    }

  dbg("[%d] Exitting\n", sockfd);
  close(sockfd);
  pthread_exit(NULL);
}

/****************************************************************************
 * Name: nsh_telnetoutput
 *
 * Description:
 *   Print a string to the remote shell window.
 *
 *   This function is implemented by the shell GUI / telnet server and
 *   can be called by the shell back-end to output a string in the
 *   shell window. The string is automatically appended with a linebreak.
 *
 ****************************************************************************/

static int nsh_telnetoutput(FAR struct nsh_vtbl_s *vtbl, const char *fmt, ...)
{
  struct telnetd_s  *pstate = (struct telnetd_s *)vtbl;
  struct telnetio_s *tio    = pstate->u.tn;
  int                nbytes = tio->tio_sndlen;
  int                len;
  va_list            ap;

  /* Put the new info into the buffer.  Here we are counting on the fact that
   * no output strings will exceed CONFIG_EXAMPLES_NSH_LINELEN!
   */

  va_start(ap, fmt);
  vsnprintf(&tio->tio_buffer[nbytes],
            (CONFIG_EXAMPLES_NSH_IOBUFFER_SIZE - 1) - nbytes, fmt, ap);
  va_end(ap);

  /* Get the size of the new string just added and the total size of
   * buffered data
   */

  len     = strlen(&tio->tio_buffer[nbytes]);
  nbytes += len;

  /* Expand any terminating \n to \r\n */

  if (nbytes < (CONFIG_EXAMPLES_NSH_IOBUFFER_SIZE - 2) &&
      tio->tio_buffer[nbytes-1] == '\n')
    {
      tio->tio_buffer[nbytes-1] = ISO_cr;
      tio->tio_buffer[nbytes]   = ISO_nl;
      tio->tio_buffer[nbytes+1] = '\0';
      nbytes++;
    }
  tio->tio_sndlen = nbytes;

  /* Flush to the network if the buffer does not have room for one more
   * maximum length string.
   */

  if (nbytes > CONFIG_EXAMPLES_NSH_IOBUFFER_SIZE - CONFIG_EXAMPLES_NSH_LINELEN)
    {
      nsh_flush(pstate);
    }

  return len;
}

/****************************************************************************
 * Name: nsh_redirectoutput
 *
 * Description:
 *   Print a string to the currently selected stream.
 *
 ****************************************************************************/

static int nsh_redirectoutput(FAR struct nsh_vtbl_s *vtbl, const char *fmt, ...)
{
  FAR struct telnetd_s *pstate = (FAR struct telnetd_s *)vtbl;
  va_list ap;
  int     ret;

  /* The stream is open in a lazy fashion.  This is done because the file
   * descriptor may be opened on a different task than the stream.  The
   * actual open will then occur with the first output from the new task.
   */

  if (nsh_openifnotopen(pstate) != 0)
   {
     return ERROR;
   }
 
  va_start(ap, fmt);
  ret = vfprintf(pstate->u.rd.rd_stream, fmt, ap);
  va_end(ap);
 
  return ret;
}

/****************************************************************************
 * Name: nsh_telnetlinebuffer
 *
 * Description:
 *   Return a reference to the current line buffer
 *
* ****************************************************************************/

static FAR char *nsh_telnetlinebuffer(FAR struct nsh_vtbl_s *vtbl)
{
  struct telnetd_s *pstate = (struct telnetd_s *)vtbl;
  return pstate->tn_cmd;
}

/****************************************************************************
 * Name: nsh_telnetclone
 *
 * Description:
 *   Make an independent copy of the vtbl
 *
 ****************************************************************************/

static FAR struct nsh_vtbl_s *nsh_telnetclone(FAR struct nsh_vtbl_s *vtbl)
{
  FAR struct telnetd_s *pstate = (FAR struct telnetd_s *)vtbl;
  FAR struct telnetd_s *pclone = nsh_allocstruct();

  pclone->tn_redirected  = TRUE;
  pclone->tn_vtbl.output = nsh_redirectoutput;
  pclone->u.rd.rd_fd     = pstate->u.rd.rd_fd;
  pclone->u.rd.rd_stream = NULL;
  return &pclone->tn_vtbl;
}

/****************************************************************************
 * Name: nsh_telnetaddref
 *
 * Description:
 *   Increment the reference count on the vtbl.
 *
 ****************************************************************************/

static void nsh_telnetaddref(FAR struct nsh_vtbl_s *vtbl)
{
  FAR struct telnetd_s *pstate = (FAR struct telnetd_s *)vtbl;
  pstate->tn_refs++;
}

/****************************************************************************
 * Name: nsh_telnetrelease
 *
 * Description:
 *   Decrement the reference count on the vtbl, releasing it when the count
 *   decrements to zero.
 *
 ****************************************************************************/

static void nsh_telnetrelease(FAR struct nsh_vtbl_s *vtbl)
{
  FAR struct telnetd_s *pstate = (FAR struct telnetd_s *)vtbl;
  if (pstate->tn_refs > 1)
    {
      pstate->tn_refs--;
    }
  else
    {
      DEBUGASSERT(pstate->tn_redirected);
      nsh_closeifnotclosed(pstate);
      free(vtbl);
    }
}

/****************************************************************************
 * Name: nsh_telnetredirect
 *
 * Description:
 *   Set up for redirected output
 *
 ****************************************************************************/

static void nsh_telnetredirect(FAR struct nsh_vtbl_s *vtbl, int fd, FAR ubyte *save)
{
  FAR struct telnetd_s    *pstate = (FAR struct telnetd_s *)vtbl;
  FAR struct telnetsave_s *ssave  = (FAR struct telnetsave_s *)save;

  if (pstate->tn_redirected)
    {
       (void)nsh_openifnotopen(pstate);
       fflush(pstate->u.rd.rd_stream);
       if (!ssave)
         {
           fclose(pstate->u.rd.rd_stream);
         }
    }

  if (ssave)
    {
      ssave->ts_redirected = pstate->tn_redirected;
      memcpy(&ssave->u.rd, &pstate->u.rd, sizeof(struct redirect_s));
    }

  pstate->tn_redirected  = TRUE;
  pstate->u.rd.rd_fd     = fd;
  pstate->u.rd.rd_stream = NULL;  
}

/****************************************************************************
 * Name: nsh_telnetredirect
 *
 * Description:
 *   Set up for redirected output
 *
 ****************************************************************************/

static void nsh_telnetundirect(FAR struct nsh_vtbl_s *vtbl, FAR ubyte *save)
{
  FAR struct telnetd_s *pstate = (FAR struct telnetd_s *)vtbl;
  FAR struct telnetsave_s *ssave  = (FAR struct telnetsave_s *)save;

  if (pstate->tn_redirected)
    {
      nsh_closeifnotclosed(pstate);
    }

  pstate->tn_redirected = ssave->ts_redirected;
  memcpy(&pstate->u.rd, &ssave->u.rd, sizeof(struct redirect_s));
}

/****************************************************************************
 * Name: nsh_telnetexit
 *
 * Description:
 *   Quit the shell instance
 *
 ****************************************************************************/

static void nsh_telnetexit(FAR struct nsh_vtbl_s *vtbl)
{
  struct telnetd_s *pstate = (struct telnetd_s *)vtbl;
  pstate->u.tn->tio_state = STATE_CLOSE;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nsh_main
 *
 * Description:
 *   This is the main processing thread for telnetd.  It never returns
 *   unless an error occurs
 *
 ****************************************************************************/

int nsh_telnetmain(int argc, char *argv[])
{
 struct in_addr addr;
#if defined(CONFIG_EXAMPLES_NSH_DHCPC) || defined(CONFIG_EXAMPLES_NSH_NOMAC)
 uint8 mac[IFHWADDRLEN];
#endif

/* Many embedded network interfaces must have a software assigned MAC */

#ifdef CONFIG_EXAMPLES_NSH_NOMAC
  mac[0] = 0x00;
  mac[1] = 0xe0;
  mac[2] = 0xb0;
  mac[3] = 0x0b;
  mac[4] = 0xba;
  mac[5] = 0xbe;
  uip_setmacaddr("eth0", mac);
#endif

  /* Set up our host address */

#if !defined(CONFIG_EXAMPLES_NSH_DHCPC)
  addr.s_addr = HTONL(CONFIG_EXAMPLES_NSH_IPADDR);
#else
  addr.s_addr = 0;
#endif
  uip_sethostaddr("eth0", &addr);

  /* Set up the default router address */

  addr.s_addr = HTONL(CONFIG_EXAMPLES_NSH_DRIPADDR);
  uip_setdraddr("eth0", &addr);

  /* Setup the subnet mask */

  addr.s_addr = HTONL(CONFIG_EXAMPLES_NSH_NETMASK);
  uip_setnetmask("eth0", &addr);

#if defined(CONFIG_EXAMPLES_NSH_DHCPC)
  /* Set up the resolver */

  resolv_init();
#endif

#if defined(CONFIG_EXAMPLES_NSH_DHCPC)
  /* Get the MAC address of the NIC */

  uip_getmacaddr("eth0", mac);

  /* Set up the DHCPC modules */

  handle = dhcpc_open(&mac, IFHWADDRLEN);

  /* Get an IP address */

  if (handle)
    {
        struct dhcpc_state ds;
        (void)dhcpc_request(handle, &ds);
        uip_sethostaddr("eth1", &ds.ipaddr);
        if (ds.netmask.s_addr != 0)
          {
            uip_setnetmask("eth0", &ds.netmask);
          }
        if (ds.default_router.s_addr != 0)
          {
            uip_setdraddr("eth0", &ds.default_router);
          }
        if (ds.dnsaddr.s_addr != 0)
          {
            resolv_conf(&ds.dnsaddr);
          }
        dhcpc_close(handle);
    }
#endif

  /* Execute nsh_connection on each connection to port 23 */

  uip_server(HTONS(23), nsh_connection, CONFIG_EXAMPLES_NSH_STACKSIZE);
  return OK;
}
