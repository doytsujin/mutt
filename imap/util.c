/*
 * Copyright (C) 1996-8 Michael R. Elkins <me@cs.hmc.edu>
 * Copyright (C) 1996-9 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999 Brendan Cully <brendan@kublai.com>
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */ 

/* general IMAP utility functions */

#include "mutt.h"
#include "imap_private.h"

#include <stdlib.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include <errno.h>

/* imap_continue: display a message and ask the user if she wants to
 *   go on. */
int imap_continue (const char* msg, const char* resp)
{
  imap_error (msg, resp);
  return mutt_yesorno (_("Continue?"), 0);
}

/* imap_error: show an error and abort */
void imap_error (const char *where, const char *msg)
{
  mutt_error (_("%s [%s]\n"), where, msg);
  sleep (2);
}

/*
 * Fix up the imap path.  This is necessary because the rest of mutt
 * assumes a hierarchy delimiter of '/', which is not necessarily true
 * in IMAP.  Additionally, the filesystem converts multiple hierarchy
 * delimiters into a single one, ie "///" is equal to "/".  IMAP servers
 * are not required to do this.
 */
char *imap_fix_path (IMAP_DATA *idata, char *mailbox, char *path, 
    size_t plen)
{
  int x = 0;

  if (!mailbox || !*mailbox)
  {
    strfcpy (path, "INBOX", plen);
    return path;
  }

  while (mailbox && *mailbox && (x < (plen - 1)))
  {
    if ((*mailbox == '/') || (*mailbox == idata->delim))
    {
      while ((*mailbox == '/') || (*mailbox == idata->delim)) mailbox++;
      path[x] = idata->delim;
    }
    else
    {
      path[x] = *mailbox;
      mailbox++;
    }
    x++;
  }
  path[x] = '\0';
  return path;
}

/* imap_get_literal_count: write number of bytes in an IMAP literal into
 *   bytes, return 0 on success, -1 on failure. */
int imap_get_literal_count(const char *buf, long *bytes)
{
  char *pc;
  char *pn;

  if (!(pc = strchr (buf, '{')))
    return (-1);
  pc++;
  pn = pc;
  while (isdigit (*pc))
    pc++;
  *pc = 0;
  *bytes = atoi(pn);
  return (0);
}

/* imap_get_qualifier: in a tagged response, skip tag and status for
 *   the qualifier message. Used by imap_copy_message for TRYCREATE */
char* imap_get_qualifier (char* buf)
{
  char *s = buf;

  /* skip tag */
  s = imap_next_word (s);
  /* skip OK/NO/BAD response */
  s = imap_next_word (s);

  return s;
}

/* imap_next_word: return index into string where next IMAP word begins */
char *imap_next_word (char *s)
{
  while (*s && !ISSPACE (*s))
    s++;
  SKIPWS (s);
  return s;
}

/* imap_parse_path: given an IMAP mailbox name, return host, port
 *   and a path IMAP servers will recognise. */
int imap_parse_path (char* path, char* host, size_t hlen, int* port, 
  int *socktype, char** mbox)
{
  int n;
  char *pc;
  char *pt;

  /* set default port */
  if (port)
    *port = 0;
  if (socktype)
    *socktype = M_NEW_SOCKET;
  pc = path;
  if (*pc != '{')
    return -1;
  pc++;
  /* skip over the entire host, but copy in only what we have room for */
  for (n = 0; *pc && *pc != '}' && *pc != ':' && *pc != '/'; pc++)
    if (n+1 < hlen)
      host[n++] = *pc;
  if (hlen)
    host[n] = 0;

  /* catch NULL hosts, unless we're deliberately not parsing them */
  if (hlen && !*host)
  {
    dprint (1, (debugfile, "imap_parse_path: NULL host in %s\n", path));
    return -1;
  }
  if (!*pc)
    return -1;
  if (*pc == ':')
  {
    char c;
    pc++;
    pt = pc;
    while (*pc && *pc != '}' && *pc != '/') pc++;
    if (!*pc)
      return -1;
    c = *pc;
    *pc = '\0';
    if (port)
      *port = atoi (pt);
    if (port && !*port)
    {
      dprint (1, (debugfile, "imap_parse_path: bad port in %s\n", path));
      return -1;
    }
    *pc = c;
  }
  if (*pc == '/') 
  {
    pc++;
    pt = pc;
    while (*pc && *pc != '}') pc++;
    if (!*pc)
      return (-1);
    *pc = '\0';
#ifdef USE_SSL
    if (!strcmp (pt, "ssl"))
    {
      if (socktype)
	*socktype = M_NEW_SSL_SOCKET;
      if (port && !*port)
	*port = IMAP_SSL_PORT;
    } else
#endif
      return (-1);
    *pc = '}';
  }
  pc++;
  
  if (port && !*port)
    *port = IMAP_PORT;
  
  *mbox = pc;
  return 0;
}

/* imap_qualify_path: make an absolute IMAP folder target, given host, port
 *   and relative path. Use this and maybe it will be easy to convert to
 *   IMAP URLs */
void imap_qualify_path (char* dest, size_t len, const char* host, int port,
  const char* path, const char* name)
{
  if (port == IMAP_PORT)
    snprintf (dest, len, "{%s}%s%s", host, NONULL (path), NONULL (name));
  else
    snprintf (dest, len, "{%s:%d}%s%s", host, port, NONULL (path),
      NONULL (name));
}

/* imap_quote_string: quote string according to IMAP rules:
 *   surround string with quotes, escape " and \ with \ */
void imap_quote_string (char *dest, size_t slen, const char *src)
{
  char quote[] = "\"\\", *pt;
  const char *s;
  size_t len = slen;

  pt = dest;
  s  = src;

  *pt++ = '"';
  /* save room for trailing quote-char */
  len -= 2;
  
  for (; *s && len; s++)
  {
    if (strchr (quote, *s))
    {
      len -= 2;
      if (!len)
	break;
      *pt++ = '\\';
      *pt++ = *s;
    }
    else
    {
      *pt++ = *s;
      len--;
    }
  }
  *pt++ = '"';
  *pt = 0;
}

/* imap_unquote_string: equally stupid unquoting routine */
void imap_unquote_string (char *s)
{
  char *d = s;

  if (*s == '\"')
    s++;
  else
    return;

  while (*s)
  {
    if (*s == '\"')
    {
      *d = '\0';
      return;
    }
    if (*s == '\\')
    {
      s++;
    }
    if (*s)
    {
      *d = *s;
      d++;
      s++;
    }
  }
  *d = '\0';
}

/* imap_wordcasecmp: find word a in word list b */
int imap_wordcasecmp(const char *a, const char *b)
{
  char tmp[SHORT_STRING];
  char *s = (char *)b;
  int i;

  tmp[SHORT_STRING-1] = 0;
  for(i=0;i < SHORT_STRING-2;i++,s++)
  {
    if (!*s || ISSPACE(*s))
    {
      tmp[i] = 0;
      break;
    }
    tmp[i] = *s;
  }
  tmp[i+1] = 0;

  return mutt_strcasecmp(a, tmp);
}

/* imap keepalive: use buffy to poll a remote imap folder
 * while waiting for an external process
 */

static RETSIGTYPE alrm_handler (int sig)
{
  /* empty */
}

int imap_wait_keepalive (pid_t pid)
{
  struct sigaction oldalrm;
  struct sigaction act;
  sigset_t oldblocked;
  int rc;

  short imap_passive = option (OPTIMAPPASSIVE);
  
  set_option (OPTIMAPPASSIVE);
  set_option (OPTKEEPQUIET);

  sigprocmask (SIG_BLOCK, NULL, &oldblocked);

  sigemptyset (&act.sa_mask);
  act.sa_handler = alrm_handler;
#ifdef SA_INTERRUPT
  act.sa_flags = SA_INTERRUPT;
#else
  act.sa_flags = 0;
#endif

  sigaction (SIGALRM, &act, &oldalrm);

  alarm (ImapCheckTimeout > 0 ? ImapCheckTimeout : 60);
  while (waitpid (pid, &rc, 0) < 0 && errno == EINTR)
  {
    if (!option (OPTMSGERR))
      mutt_buffy_check (0);

    alarm (ImapCheckTimeout > 0 ? ImapCheckTimeout : 60);
  }

  alarm (0);	/* cancel an possibly pending alarm */
  
  sigaction (SIGALRM, &oldalrm, NULL);
  sigprocmask (SIG_BLOCK, &oldblocked, NULL);

  unset_option (OPTKEEPQUIET);
  if (!imap_passive)
    unset_option (OPTIMAPPASSIVE);

  return rc;
}

