/**
 * parser.c -- web log parsing
 *    ______      ___
 *   / ____/___  /   | _____________  __________
 *  / / __/ __ \/ /| |/ ___/ ___/ _ \/ ___/ ___/
 * / /_/ / /_/ / ___ / /__/ /__/  __(__  |__  )
 * \____/\____/_/  |_\___/\___/\___/____/____/
 *
 * The MIT License (MIT)
 * Copyright (c) 2009-2025 Gerardo Orellana <hello @ goaccess.io>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * "_XOPEN_SOURCE" is required for the GNU libc to export "strptime(3)"
 * correctly.
 */
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <libgen.h>

#include "gkhash.h"

#include "parser.h"

#include "browsers.h"
#include "error.h"
#include "goaccess.h"
#include "gstorage.h"
#include "util.h"
#include "websocket.h"
#include "xmalloc.h"

/* Allocate memory for a new GRawData instance.
 *
 * On success, the newly allocated GRawData is returned . */
GRawData *
new_grawdata (void) {
  GRawData *raw_data = xmalloc (sizeof (*raw_data));
  memset (raw_data, 0, sizeof *raw_data);

  return raw_data;
}

/* Allocate memory for a new GRawDataItem instance.
 *
 * On success, the newly allocated GRawDataItem is returned . */
GRawDataItem *
new_grawdata_item (unsigned int size) {
  GRawDataItem *item = xcalloc (size, sizeof (*item));
  return item;
}

/* Free memory allocated for a GRawData and GRawDataItem instance. */
void
free_raw_data (GRawData *raw_data) {
  free (raw_data->items);
  free (raw_data);
}

/* Reset an instance of GLog structure. */
void
reset_struct (Logs *logs) {
  int i = 0;

  for (i = 0; i < logs->size; ++i)
    logs->glog[i].invalid = logs->glog[i].processed = 0;
}

/* Allocate memory for a new Logs and GLog instance.
 *
 * On success, the newly allocated Logs is returned . */
Logs *
new_logs (int size) {
  Logs *logs = xmalloc (sizeof (*logs));
  memset (logs, 0, sizeof *logs);

  logs->glog = xcalloc (size, sizeof (GLog));
  logs->size = size;
  logs->idx = 0;

  return logs;
}

/* Allocate, initialize and add the given filename to our logs structure.
 *
 * On error, 1 is returned.
 * On success, the given filename is added to the Logs structure and 0 is
 * returned. */
int
set_glog (Logs *logs, const char *filename) {
  GLog *tmp = NULL, *glog = NULL;
  int newlen = 0;
  char const *err;
  char *fvh = NULL, *fn = NULL;

  if (logs->size - 1 < logs->idx) {
    newlen = logs->size + 1;
    if (!(tmp = xrealloc (logs->glog, newlen * sizeof (GLog))))
      return ERR_LOG_REALLOC_FAILURE;

    logs->glog = tmp;

    memset (logs->glog + logs->idx, 0, (logs->idx + 1 - logs->size) * sizeof *logs->glog);
    logs->size = newlen;
  }

  fn = xstrdup (filename); /* ensure fn is a string */
  glog = logs->glog;
  glog[logs->idx].errors = xcalloc (MAX_LOG_ERRORS, sizeof (char *));
  glog[logs->idx].props.filename = xstrdup (fn);
  glog[logs->idx].props.fname = xstrdup (basename (fn));

  if (!glog->pipe && conf.fname_as_vhost) {
    if (!(fvh = regex_extract_string (glog[logs->idx].props.fname, conf.fname_as_vhost, 1, &err)))
      FATAL ("%s %s[%s]", err, glog[logs->idx].props.fname, conf.fname_as_vhost);
    glog[logs->idx].fname_as_vhost = fvh;
  }

  logs->processed = &(glog[logs->idx].processed);
  logs->filename = glog[logs->idx].props.filename;
  logs->idx++;
  free (fn);

  return 0;
}

/* Ensure the given filename is part of our original list of files.
 *
 * On error, 1 is returned.
 * On success, the given filename is added to the Logs structure and 0 is
 * returned. */
int
set_log (Logs *logs, const char *value) {
  if (str_inarray (value, conf.filenames, conf.filenames_idx) < 0)
    return ERR_LOG_NOT_FOUND;

  return set_glog (logs, value);
}

/* Allocate memory for a new set of Logs including a GLog instance.
 *
 * On success, the newly allocated Logs is returned . */
Logs *
init_logs (int size) {
  Logs *logs = NULL;
  GLog *glog = NULL;
  int i = 0, ret = 0;

  /* if no logs no a pipe nor restoring, nothing to do then */
  if (!size && !conf.restore)
    return NULL;

  /* If no logs nor a pipe but restoring, we still need an minimal instance of
   * logs and a glog */
  if (!size) {
    logs = xcalloc (1, sizeof (*logs));
    logs->glog = xcalloc (1, sizeof (*glog));
    logs->processed = &(logs->glog[0].processed);
    return logs;
  }

  logs = new_logs (size);
  logs->size = size;

  for (i = 0; i < size; ++i) {
    if ((ret = set_log (logs, conf.filenames[i])))
      FATAL ("%s\n", ERR_LOG_NOT_FOUND_MSG);
  }

  return logs;
}

/* Free all log errors stored during parsing. */
void
free_logerrors (GLog *glog) {
  int i;

  if (!glog->log_erridx)
    return;

  for (i = 0; i < glog->log_erridx; ++i)
    free (glog->errors[i]);
  glog->log_erridx = 0;
}

/* Free all log containers. */
void
free_logs (Logs *logs) {
  GLog *glog = NULL;
  int i;

  for (i = 0; i < logs->size; ++i) {
    glog = &logs->glog[i];

    free (glog->props.filename);
    free (glog->props.fname);
    free (glog->fname_as_vhost);
    free_logerrors (glog);
    free (glog->errors);
    if (glog->pipe) {
      fclose (glog->pipe);
    }
  }

  free (logs->glog);
  free (logs);
}

/* Initialize a new GLogItem instance.
 *
 * On success, the new GLogItem instance is returned. */
GLogItem *
init_log_item (GLog *glog) {
  GLogItem *logitem;
  logitem = xmalloc (sizeof (GLogItem));
  memset (logitem, 0, sizeof *logitem);

  logitem->agent = NULL;
  logitem->browser = NULL;
  logitem->browser_type = NULL;
  logitem->continent = NULL;
  logitem->asn = NULL;
  logitem->country = NULL;
  logitem->date = NULL;
  logitem->errstr = NULL;
  logitem->host = NULL;
  logitem->keyphrase = NULL;
  logitem->method = NULL;
  logitem->os = NULL;
  logitem->os_type = NULL;
  logitem->protocol = NULL;
  logitem->qstr = NULL;
  logitem->ref = NULL;
  logitem->req_key = NULL;
  logitem->req = NULL;
  logitem->resp_size = 0LL;
  logitem->serve_time = 0;
  logitem->status = -1;
  logitem->time = NULL;
  logitem->uniq_key = NULL;
  logitem->vhost = NULL;
  logitem->userid = NULL;
  logitem->cache_status = NULL;

  /* UMS */
  logitem->mime_type = NULL;
  logitem->tls_type = NULL;
  logitem->tls_cypher = NULL;
  logitem->tls_type_cypher = NULL;

  memset (logitem->site, 0, sizeof (logitem->site));
  memset (logitem->agent_hex, 0, sizeof (logitem->agent_hex));
  logitem->dt = glog->start_time;

  return logitem;
}

/* Free all members of a GLogItem */
void
free_glog (GLogItem *logitem) {
  if (logitem->agent != NULL)
    free (logitem->agent);
  if (logitem->browser != NULL)
    free (logitem->browser);
  if (logitem->browser_type != NULL)
    free (logitem->browser_type);
  if (logitem->continent != NULL)
    free (logitem->continent);
  if (logitem->asn != NULL)
    free (logitem->asn);
  if (logitem->country != NULL)
    free (logitem->country);
  if (logitem->date != NULL)
    free (logitem->date);
  if (logitem->errstr != NULL)
    free (logitem->errstr);
  if (logitem->host != NULL)
    free (logitem->host);
  if (logitem->keyphrase != NULL)
    free (logitem->keyphrase);
  if (logitem->method != NULL)
    free (logitem->method);
  if (logitem->os != NULL)
    free (logitem->os);
  if (logitem->os_type != NULL)
    free (logitem->os_type);
  if (logitem->protocol != NULL)
    free (logitem->protocol);
  if (logitem->qstr != NULL)
    free (logitem->qstr);
  if (logitem->ref != NULL)
    free (logitem->ref);
  if (logitem->req_key != NULL)
    free (logitem->req_key);
  if (logitem->req != NULL)
    free (logitem->req);
  if (logitem->time != NULL)
    free (logitem->time);
  if (logitem->uniq_key != NULL)
    free (logitem->uniq_key);
  if (logitem->userid != NULL)
    free (logitem->userid);
  if (logitem->cache_status != NULL)
    free (logitem->cache_status);
  if (logitem->vhost != NULL)
    free (logitem->vhost);

  if (logitem->mime_type != NULL)
    free (logitem->mime_type);
  if (logitem->tls_type != NULL)
    free (logitem->tls_type);
  if (logitem->tls_cypher != NULL)
    free (logitem->tls_cypher);
  if (logitem->tls_type_cypher != NULL)
    free (logitem->tls_type_cypher);

  free (logitem);
}

/* Entry point to decode the given URL-encoded string.
 *
 * On success, the decoded trimmed string is assigned to the output
 * buffer. */
static char *
decode_url (char *url) {
  char *out, *decoded;

  if ((url == NULL) || (*url == '\0'))
    return NULL;

  out = decoded = xstrdup (url);
  decode_hex (url, out, 0);
  /* double encoded URL? */
  if (conf.double_decode)
    decode_hex (decoded, out, 0);
  strip_newlines (out);

  return trim_str (out);
}

/* Process keyphrases from Google search, cache, and translate.
 * Note that the referer hasn't been decoded at the entry point
 * since there could be '&' within the search query.
 *
 * On error, 1 is returned.
 * On success, the extracted keyphrase is assigned and 0 is returned. */
static int
extract_keyphrase (char *ref, char **keyphrase) {
  char *r, *ptr, *pch, *referer;
  int encoded = 0;

  if (!(strstr (ref, "http://www.google.")) &&
      !(strstr (ref, "http://webcache.googleusercontent.com/")) &&
      !(strstr (ref, "http://translate.googleusercontent.com/")) &&
      !(strstr (ref, "https://www.google.")) &&
      !(strstr (ref, "https://webcache.googleusercontent.com/")) &&
      !(strstr (ref, "https://translate.googleusercontent.com/")))
    return 1;

  /* webcache.googleusercontent */
  if ((r = strstr (ref, "/+&")) != NULL)
    return 1;
  /* webcache.googleusercontent */
  else if ((r = strstr (ref, "/+")) != NULL)
    r += 2;
  /* webcache.googleusercontent */
  else if ((r = strstr (ref, "q=cache:")) != NULL) {
    pch = strchr (r, '+');
    if (pch)
      r += pch - r + 1;
  }
  /* www.google.* or translate.googleusercontent */
  else if ((r = strstr (ref, "&q=")) != NULL || (r = strstr (ref, "?q=")) != NULL)
    r += 3;
  else if ((r = strstr (ref, "%26q%3D")) != NULL || (r = strstr (ref, "%3Fq%3D")) != NULL)
    encoded = 1, r += 7;
  else
    return 1;

  if (!encoded && (ptr = strchr (r, '&')) != NULL)
    *ptr = '\0';
  else if (encoded && (ptr = strstr (r, "%26")) != NULL)
    *ptr = '\0';

  referer = decode_url (r);
  if (referer == NULL || *referer == '\0') {
    free (referer);
    return 1;
  }

  referer = char_replace (referer, '+', ' ');
  *keyphrase = trim_str (referer);

  return 0;
}

/* Parse a URI and extracts the *host* part from it
 * i.e., //www.example.com/path?googleguy > www.example.com
 *
 * On error, 1 is returned.
 * On success, the extracted referer is set and 0 is returned. */
static int
extract_referer_site (const char *referer, char *host) {
  char *url, *begin, *end;
  int len = 0;

  if ((referer == NULL) || (*referer == '\0'))
    return 1;

  url = strdup (referer);
  if ((begin = strstr (url, "//")) == NULL)
    goto clean;

  begin += 2;
  if ((len = strlen (begin)) == 0)
    goto clean;

  if ((end = strpbrk (begin, "/?")) != NULL)
    len = end - begin;

  if (len == 0)
    goto clean;

  if (len >= REF_SITE_LEN)
    len = REF_SITE_LEN;

  memcpy (host, begin, len);
  host[len] = '\0';
  free (url);
  return 0;
clean:
  free (url);

  return 1;
}

/* Determine if the given request is static (e.g., jpg, css, js, etc).
 *
 * On error, or if not static, 0 is returned.
 * On success, the 1 is returned. */
static int
verify_static_content (const char *req) {
  const char *nul = NULL;
  const char *ext = NULL, *pch = NULL;
  int elen = 0, i;

  if ((req == NULL) || (*req == '\0'))
    return 0;

  nul = req + strlen (req);

  for (i = 0; i < conf.static_file_idx; ++i) {
    ext = conf.static_files[i];
    if (ext == NULL || *ext == '\0')
      continue;

    elen = strlen (ext);
    if (conf.all_static_files && (pch = strchr (req, '?')) != NULL && pch - req > elen) {
      pch -= elen;
      if (0 == strncasecmp (ext, pch, elen))
        return 1;
      continue;
    }

    if (nul - req > elen && !strncasecmp (nul - elen, ext, elen))
      return 1;
  }

  return 0;
}

/* Extract the HTTP method.
 *
 * On error, or if not found, NULL is returned.
 * On success, the HTTP method is returned. */
static const char *
extract_method (const char *token) {
  size_t i;
  for (i = 0; i < http_methods_len; i++) {
    if (strncasecmp (token, http_methods[i].method, http_methods[i].len) == 0) {
      return http_methods[i].method;
    }
  }
  return NULL;
}

static int
is_cache_hit (const char *tkn) {
  if (strcasecmp ("MISS", tkn) == 0)
    return 1;
  else if (strcasecmp ("BYPASS", tkn) == 0)
    return 1;
  else if (strcasecmp ("EXPIRED", tkn) == 0)
    return 1;
  else if (strcasecmp ("STALE", tkn) == 0)
    return 1;
  else if (strcasecmp ("UPDATING", tkn) == 0)
    return 1;
  else if (strcasecmp ("REVALIDATED", tkn) == 0)
    return 1;
  else if (strcasecmp ("HIT", tkn) == 0)
    return 1;
  return 0;
}

/* Determine if the given token is a valid HTTP protocol.
 *
 * If not valid, 1 is returned.
 * If valid, 0 is returned. */
static const char *
extract_protocol (const char *token) {
  size_t i;
  for (i = 0; i < http_protocols_len; i++) {
    if (strncasecmp (token, http_protocols[i].protocol, http_protocols[i].len) == 0) {
      return http_protocols[i].protocol;
    }
  }
  return NULL;
}

/* Parse a request containing the method and protocol.
 *
 * On error, or unable to parse, NULL is returned.
 * On success, the HTTP request is returned and the method and
 * protocol are assigned to the corresponding buffers. */
static char *
parse_req (char *line, char **method, char **protocol) {
  char *req = NULL, *request = NULL, *dreq = NULL, *ptr = NULL;
  const char *meth, *proto;
  ptrdiff_t rlen;

  meth = extract_method (line);

  /* couldn't find a method, so use the whole request line */
  if (meth == NULL) {
    request = xstrdup (line);
  }
  /* method found, attempt to parse request */
  else {
    req = line + strlen (meth);
    if (!(ptr = strrchr (req, ' ')) || !(proto = extract_protocol (++ptr)))
      return alloc_string ("-");

    req++;
    if ((rlen = ptr - req) <= 0)
      return alloc_string ("-");

    request = xmalloc (rlen + 1);
    strncpy (request, req, rlen);
    request[rlen] = 0;

    if (conf.append_method)
      (*method) = strtoupper (xstrdup (meth));

    if (conf.append_protocol)
      (*protocol) = strtoupper (xstrdup (proto));
  }

  if (!(dreq = decode_url (request)))
    return request;
  else if (*dreq == '\0') {
    free (dreq);
    return request;
  }

  free (request);
  return dreq;
}

#if defined(HAVE_LIBSSL) && defined(HAVE_CIPHER_STD_NAME)
static int
extract_tls_version_cipher (char *tkn, char **cipher, char **tls_version) {
  SSL_CTX *ctx = NULL;
  SSL *ssl = NULL;
  int code = 0;
  unsigned short code_be;
  unsigned char cipherid[3];
  const SSL_CIPHER *c = NULL;
  char *bEnd;
  const char *sn = NULL;

  code = strtoull (tkn, &bEnd, 10);
  if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE) {
    LOG_DEBUG (("unable to convert cipher code to a valid decimal."));
    goto fail;
  }

  /* ssl context */
  if (!(ctx = SSL_CTX_new (SSLv23_server_method ()))) {
    LOG_DEBUG (("Unable to create a new SSL_CTX_new to extract TLS."));
    goto fail;
  }
  if (!(ssl = SSL_new (ctx))) {
    LOG_DEBUG (("Unable to create a new instance of SSL_new to extract TLS."));
    goto fail;
  }

  code_be = htobe16 (code);
  memcpy (cipherid, &code_be, 2);
  cipherid[2] = 0;

  if (!(c = SSL_CIPHER_find (ssl, cipherid))) {
    LOG_DEBUG (("Unable to find cipher to extract TLS."));
    goto fail;
  }

  if (!(sn = SSL_CIPHER_standard_name (c))) {
    LOG_DEBUG (("Unable to get cipher standard name to extract TLS."));
    goto fail;
  }
  *cipher = xstrdup (sn);
  *tls_version = xstrdup (SSL_CIPHER_get_version (c));

  free (tkn);
  SSL_free (ssl);
  SSL_CTX_free (ctx);

  return 0;

fail:
  free (tkn);
  if (ssl)
    SSL_free (ssl);
  if (ctx)
    SSL_CTX_free (ctx);
  return 1;
}
#endif

/* Extract the next delimiter given a log format and copy the delimiter to the
 * destination buffer.
 *
 * On error, the dest buffer will be empty.
 * On success, the delimiter(s) are stored in the dest buffer. */
static void
get_delim (char *dest, const char *p) {
  /* done, nothing to do */
  if (p[0] == '\0' || p[1] == '\0') {
    dest[0] = '\0';
    return;
  }
  /* add the first delim */
  dest[0] = *(p + 1);
}

/* Extract and malloc a token given the parsed rule.
 *
 * On success, the malloc'd token is returned. */
static char *
parsed_string (const char *pch, const char **str, int move_ptr) {
  char *p;
  size_t len = (pch - *str + 1);

  p = xmalloc (len);
  memcpy (p, *str, (len - 1));
  p[len - 1] = '\0';
  if (move_ptr)
    *str += len - 1;

  return trim_str (p);
}

/* Find and extract a token given a log format rule.
 *
 * On error, or unable to parse it, NULL is returned.
 * On success, the malloc'd token is returned. */
static char *
parse_string (const char **str, const char *delims, int cnt) {
  int idx = 0;
  const char *pch = *str, *p = NULL;
  char end;

  if ((*delims != 0x0) && (p = strpbrk (*str, delims)) == NULL)
    return NULL;

  end = !*delims ? 0x0 : *p;
  do {
    /* match number of delims */
    if (*pch == end)
      idx++;
    /* delim found, parse string then */
    if ((*pch == end && cnt == idx) || *pch == '\0')
      return parsed_string (pch, str, 1);
    /* advance to the first unescaped delim */
    if (*pch == '\\')
      pch++;
  } while (*pch++);

  return NULL;
}

char *
extract_by_delim (const char **str, const char *end) {
  return parse_string (&(*str), end, 1);
}

/* Move forward through the log string until a non-space (!isspace)
 * char is found. */
static void
find_alpha (const char **str) {
  const char *s = *str;
  while (*s) {
    if (isspace ((unsigned char) *s))
      s++;
    else
      break;
  }
  *str += s - *str;
}

/* Move forward through the log string until a non-space (!isspace)
 * char is found and returns the count. */
static int
find_alpha_count (const char *str) {
  int cnt = 0;
  const char *s = str;
  while (*s) {
    if (isspace ((unsigned char) *s))
      s++, cnt++;
    else
      break;
  }
  return cnt;
}

/* Format the broken-down time tm to a numeric date format.
 *
 * On error, or unable to format the given tm, 1 is returned.
 * On success, a malloc'd format is returned. */
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static int
set_date (char **fdate, struct tm tm) {
  char buf[DATE_LEN] = "";      /* Ymd */

  memset (buf, 0, sizeof (buf));
  if (strftime (buf, DATE_LEN, conf.date_num_format, &tm) <= 0)
    return 1;
  *fdate = xstrdup (buf);

  return 0;
}

/* Format the broken-down time tm to a numeric time format.
 *
 * On error, or unable to format the given tm, 1 is returned.
 * On success, a malloc'd format is returned. */
static int
set_time (char **ftime, struct tm tm) {
  char buf[TIME_LEN] = "";

  memset (buf, 0, sizeof (buf));
  if (strftime (buf, TIME_LEN, "%H:%M:%S", &tm) <= 0)
    return 1;
  *ftime = xstrdup (buf);

  return 0;
}

/* Determine the parsing specifier error and construct a message out
 * of it.
 *
 * On success, a malloc'd error message is assigned to the log
 * structure and 1 is returned. */
static int
spec_err (GLogItem *logitem, int code, const char spec, const char *tkn) {
  char *err = NULL;
  const char *fmt = NULL;

  switch (code) {
  case ERR_SPEC_TOKN_NUL:
    fmt = "Token for '%%%c' specifier is NULL.";
    err = xmalloc (snprintf (NULL, 0, fmt, spec) + 1);
    sprintf (err, fmt, spec);
    break;
  case ERR_SPEC_TOKN_INV:
    fmt = "Token '%s' doesn't match specifier '%%%c'";
    err = xmalloc (snprintf (NULL, 0, fmt, (tkn ? tkn : "-"), spec) + 1);
    sprintf (err, fmt, (tkn ? tkn : "-"), spec);
    break;
  case ERR_SPEC_SFMT_MIS:
    fmt = "Missing braces '%s' and ignore chars for specifier '%%%c'";
    err = xmalloc (snprintf (NULL, 0, fmt, (tkn ? tkn : "-"), spec) + 1);
    sprintf (err, fmt, (tkn ? tkn : "-"), spec);
    break;
  case ERR_SPEC_LINE_INV:
    fmt = "Incompatible format due to early parsed line ending '\\0'.";
    err = xmalloc (snprintf (NULL, 0, fmt, (tkn ? tkn : "-")) + 1);
    sprintf (err, fmt, (tkn ? tkn : "-"));
    break;
  }
  logitem->errstr = err;

  return code;
}

static void
set_tm_dt_logitem (GLogItem *logitem, struct tm tm) {
  logitem->dt.tm_year = tm.tm_year;
  logitem->dt.tm_mon = tm.tm_mon;
  logitem->dt.tm_mday = tm.tm_mday;
}

static void
set_tm_tm_logitem (GLogItem *logitem, struct tm tm) {
  logitem->dt.tm_hour = tm.tm_hour;
  logitem->dt.tm_min = tm.tm_min;
  logitem->dt.tm_sec = tm.tm_sec;
}

static void
set_numeric_date (uint32_t *numdate, const char *date) {
  int res = 0;
  if ((res = str2int (date)) == -1)
    FATAL ("Unable to parse date to integer %s", date);
  *numdate = res;
}

static void
set_agent_hash (GLogItem *logitem) {
  logitem->agent_hash = djb2 ((unsigned char *) logitem->agent);
  sprintf (logitem->agent_hex, "%" PRIx32, logitem->agent_hash);
}

static int
handle_default_case_token (const char **str, const char *p) {
  char *pch = NULL;
  if ((pch = strchr (*str, p[1])) != NULL)
    *str += pch - *str;
  return 0;
}

static void
normalize_mime_type (const char *mime, char *out, size_t out_size) {
  size_t mime_len = 0, remaining = 0;
  char *temp = NULL, *dest = NULL, *ptr = NULL, *token = NULL, *end = NULL, *sep = NULL, *c = NULL;
  const char *delims = NULL;
  int first = 0, n = 0;

  if (!mime || !out || out_size == 0)
    return;

  mime_len = strlen (mime);
  temp = xmalloc (mime_len + 1);
  memcpy (temp, mime, mime_len + 1);

  out[0] = '\0';
  dest = out;
  remaining = out_size;

  /* Set the delimiters; both ';' and ',' are significant separators */
  delims = ";,";
  first = 1;
  ptr = temp;

  /* Process each token */
  while (*ptr != '\0') {
    /* Skip leading whitespace */
    while (*ptr && isspace ((unsigned char) *ptr))
      ptr++;
    if (*ptr == '\0')
      break;

    token = ptr;
    /* Find next delimiter */
    sep = strpbrk (ptr, delims);
    if (sep != NULL) {
      *sep = '\0';
      ptr = sep + 1;
    } else {
      ptr += strlen (ptr);
    }

    /* Trim trailing whitespace on token */
    end = token + strlen (token) - 1;
    while (end >= token && isspace ((unsigned char) *end)) {
      *end = '\0';
      end--;
    }
    if (*token == '\0')
      continue;

    /* Convert token to lower-case in place */
    for (c = token; *c; c++)
      *c = (char) tolower ((unsigned char) *c);

    /* Append separator if this is not the first token */
    if (!first) {
      n = snprintf (dest, remaining, "; ");
      if (n < 0 || (size_t) n >= remaining) {
        dest[remaining - 1] = '\0';
        free (temp);
        return;
      }
      dest += n;
      remaining -= n;
    } else {
      first = 0;
    }

    /* Append the normalized token safely */
    n = snprintf (dest, remaining, "%s", token);
    if (n < 0 || (size_t) n >= remaining) {
      dest[remaining - 1] = '\0';
      free (temp);
      return;
    }
    dest += n;
    remaining -= n;
  }
  free (temp);
}

#pragma GCC diagnostic warning "-Wformat-nonliteral"

/* Parse the log string given log format rule.
 *
 * On error, or unable to parse it, 1 is returned.
 * On success, the malloc'd token is assigned to a GLogItem member. */
static int
parse_specifier (GLogItem *logitem, const char **str, const char *p, const char *end) {
  struct tm tm;
  const char *dfmt = conf.date_format;
  const char *tfmt = conf.time_format;
  char norm_mime[MAX_MIME_OUT] = { 0 };

  char *pch, *sEnd, *bEnd, *tkn = NULL;
  double serve_secs = 0.0;
  uint64_t bandw = 0, serve_time = 0;
  int dspc = 0, fmtspcs = 0;

  errno = 0;
  memset (&tm, 0, sizeof (tm));
  tm.tm_isdst = -1;
  tm = logitem->dt;

  switch (*p) {
    /* date */
  case 'd':
    if (logitem->date)
      return handle_default_case_token (str, p);

    /* Attempt to parse date format containing spaces,
     * i.e., syslog date format (Jul\s15, Nov\s\s2).
     * Note that it's possible a date could contain some padding, e.g.,
     * Dec\s\s2 vs Nov\s22, so we attempt to take that into consideration by looking
     * ahead the log string and counting the # of spaces until we find an alphanum char. */
    if ((fmtspcs = count_matches (dfmt, ' ')) && (pch = strchr (*str, ' ')))
      dspc = find_alpha_count (pch);

    if (!(tkn = parse_string (&(*str), end, MAX (dspc, fmtspcs) + 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    if (str_to_time (tkn, dfmt, &tm, 1) != 0 || set_date (&logitem->date, tm) != 0) {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }

    set_numeric_date (&logitem->numdate, logitem->date);
    set_tm_dt_logitem (logitem, tm);
    free (tkn);
    break;
    /* time */
  case 't':
    if (logitem->time)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    if (str_to_time (tkn, tfmt, &tm, 1) != 0 || set_time (&logitem->time, tm) != 0) {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }

    set_tm_tm_logitem (logitem, tm);
    free (tkn);
    break;
    /* date/time as decimal, i.e., timestamps, ms/us  */
  case 'x':
    if (logitem->time && logitem->date)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    if (str_to_time (tkn, tfmt, &tm, 1) != 0 || set_date (&logitem->date, tm) != 0 ||
        set_time (&logitem->time, tm) != 0) {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }
    set_numeric_date (&logitem->numdate, logitem->date);
    set_tm_dt_logitem (logitem, tm);
    set_tm_tm_logitem (logitem, tm);
    free (tkn);
    break;
    /* Virtual Host */
  case 'v':
    if (logitem->vhost)
      return handle_default_case_token (str, p);
    tkn = parse_string (&(*str), end, 1);
    if (tkn == NULL)
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);
    logitem->vhost = tkn;
    break;
    /* remote user */
  case 'e':
    if (logitem->userid)
      return handle_default_case_token (str, p);
    tkn = parse_string (&(*str), end, 1);
    if (tkn == NULL)
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);
    logitem->userid = tkn;
    break;
    /* cache status */
  case 'C':
    if (logitem->cache_status)
      return handle_default_case_token (str, p);
    tkn = parse_string (&(*str), end, 1);
    if (tkn == NULL)
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);
    if (is_cache_hit (tkn))
      logitem->cache_status = tkn;
    else
      free (tkn);
    break;
    /* remote hostname (IP only) */
  case 'h':
    if (logitem->host)
      return handle_default_case_token (str, p);
    /* per https://datatracker.ietf.org/doc/html/rfc3986#section-3.2.2 */
    /* square brackets are possible */
    if (*str[0] == '[' && (*str += 1) && **str)
      end = "]";
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    if (!conf.no_ip_validation && invalid_ipaddr (tkn, &logitem->type_ip)) {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }
    /* require a valid host token (e.g., ord38s18-in-f14.1e100.net) even when we're
     * not validating the IP */
    if (conf.no_ip_validation && *tkn == '\0') {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }
    logitem->host = tkn;
    break;
    /* request method */
  case 'm':
    if (logitem->method)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);
    {
      const char *meth = NULL;
      if (!(meth = extract_method (tkn))) {
        spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
        free (tkn);
        return 1;
      }
      logitem->method = xstrdup (meth);
      free (tkn);
    }
    break;
    /* request not including method or protocol */
  case 'U':
    if (logitem->req)
      return handle_default_case_token (str, p);
    tkn = parse_string (&(*str), end, 1);
    if (tkn == NULL || *tkn == '\0') {
      free (tkn);
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);
    }

    if ((logitem->req = decode_url (tkn)) == NULL) {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }
    free (tkn);
    break;
    /* query string alone, e.g., ?param=goaccess&tbm=shop */
  case 'q':
    if (logitem->qstr)
      return handle_default_case_token (str, p);
    tkn = parse_string (&(*str), end, 1);
    if (tkn == NULL || *tkn == '\0') {
      free (tkn);
      return 0;
    }

    if ((logitem->qstr = decode_url (tkn)) == NULL) {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }
    free (tkn);
    break;
    /* request protocol */
  case 'H':
    if (logitem->protocol)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);
    {
      const char *proto = NULL;
      if (!(proto = extract_protocol (tkn))) {
        spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
        free (tkn);
        return 1;
      }
      logitem->protocol = xstrdup (proto);
      free (tkn);
    }
    break;
    /* request, including method + protocol */
  case 'r':
    if (logitem->req)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    logitem->req = parse_req (tkn, &logitem->method, &logitem->protocol);
    free (tkn);
    break;
    /* Status Code */
  case 's':
    if (logitem->status >= 0)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    logitem->status = strtol (tkn, &sEnd, 10);
    if (tkn == sEnd || *sEnd != '\0' || errno == ERANGE ||
        (!conf.no_strict_status && !is_valid_http_status (logitem->status))) {
      spec_err (logitem, ERR_SPEC_TOKN_INV, *p, tkn);
      free (tkn);
      return 1;
    }
    free (tkn);
    break;
    /* size of response in bytes - excluding HTTP headers */
  case 'b':
    if (logitem->resp_size)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    bandw = strtoull (tkn, &bEnd, 10);
    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      bandw = 0;
    logitem->resp_size = bandw;
    {
      int expected = 0;
      __atomic_compare_exchange_n (&conf.bandwidth, &expected, 1, false, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST);
    }
    free (tkn);
    break;
    /* referrer */
  case 'R':
    if (logitem->ref)
      return handle_default_case_token (str, p);

    if (!(tkn = parse_string (&(*str), end, 1)))
      tkn = alloc_string ("-");
    if (*tkn == '\0') {
      free (tkn);
      tkn = alloc_string ("-");
    }
    if (strcmp (tkn, "-") != 0) {
      extract_keyphrase (tkn, &logitem->keyphrase);
      extract_referer_site (tkn, logitem->site);

      /* hide referrers from report */
      if (hide_referer (logitem->site)) {
        logitem->site[0] = '\0';
        free (tkn);
      } else
        logitem->ref = tkn;
      break;
    }
    logitem->ref = tkn;

    break;
    /* user agent */
  case 'u':
    if (logitem->agent)
      return handle_default_case_token (str, p);

    tkn = parse_string (&(*str), end, 1);
    if (tkn != NULL && *tkn != '\0') {
      /* Make sure the user agent is decoded (i.e.: CloudFront) */
      logitem->agent = decode_url (tkn);

      set_browser_os (logitem);
      set_agent_hash (logitem);
      free (tkn);
      break;
    } else if (tkn != NULL && *tkn == '\0') {
      free (tkn);
      tkn = alloc_string ("-");
    }
    /* must be null */
    else {
      tkn = alloc_string ("-");
    }
    logitem->agent = tkn;
    set_agent_hash (logitem);
    break;
    /* time taken to serve the request, in milliseconds as a decimal number */
  case 'L':
    /* ignore it if we already have served time */
    if (logitem->serve_time)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    serve_secs = strtoull (tkn, &bEnd, 10);
    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      serve_secs = 0;
    /* convert it to microseconds */
    logitem->serve_time = (serve_secs > 0) ? serve_secs * MILS : 0;

    /* Determine if time-served data was stored on-disk. */
    {
      int expected = 0;
      __atomic_compare_exchange_n (&conf.serve_usecs, &expected, 1, false, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST);
    }

    free (tkn);
    break;
    /* time taken to serve the request, in seconds with a milliseconds
     * resolution */
  case 'T':
    /* ignore it if we already have served time */
    if (logitem->serve_time)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    if (strchr (tkn, '.') != NULL)
      serve_secs = strtod (tkn, &bEnd);
    else
      serve_secs = strtoull (tkn, &bEnd, 10);

    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      serve_secs = 0;
    /* convert it to microseconds */
    logitem->serve_time = (serve_secs > 0) ? serve_secs * SECS : 0;

    /* Determine if time-served data was stored on-disk. */
    {
      int expected = 0;
      __atomic_compare_exchange_n (&conf.serve_usecs, &expected, 1, false, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST);
    }
    free (tkn);
    break;
    /* time taken to serve the request, in microseconds */
  case 'D':
    /* ignore it if we already have served time */
    if (logitem->serve_time)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    serve_time = strtoull (tkn, &bEnd, 10);
    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      serve_time = 0;
    logitem->serve_time = serve_time;

    /* Determine if time-served data was stored on-disk. */
    {
      int expected = 0;
      __atomic_compare_exchange_n (&conf.serve_usecs, &expected, 1, false, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST);
    }
    free (tkn);
    break;
    /* time taken to serve the request, in nanoseconds */
  case 'n':
    /* ignore it if we already have served time */
    if (logitem->serve_time)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    serve_time = strtoull (tkn, &bEnd, 10);
    if (tkn == bEnd || *bEnd != '\0' || errno == ERANGE)
      serve_time = 0;

    /* convert it to microseconds */
    logitem->serve_time = (serve_time > 0) ? serve_time / MILS : 0;

    /* Determine if time-served data was stored on-disk. */
    {
      int expected = 0;
      __atomic_compare_exchange_n (&conf.serve_usecs, &expected, 1, false, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST);
    }
    free (tkn);
    break;
    /* UMS: Krypto (TLS) "ECDHE-RSA-AES128-GCM-SHA256" */
  case 'k':
    /* error to set this twice */
    if (logitem->tls_cypher)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

#if defined(HAVE_LIBSSL) && defined(HAVE_CIPHER_STD_NAME)
    {
      char *tmp = NULL;
      for (tmp = tkn; isdigit ((unsigned char) *tmp); tmp++);
      if (!strlen (tmp))
        extract_tls_version_cipher (tkn, &logitem->tls_cypher, &logitem->tls_type);
      else
        logitem->tls_cypher = tkn;
    }
#else
    logitem->tls_cypher = tkn;
#endif

    break;

    /* UMS: Krypto (TLS) parameters like "TLSv1.2" */
  case 'K':
    /* error to set this twice */
    if (logitem->tls_type)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    logitem->tls_type = tkn;
    break;

    /* UMS: Mime-Type like "text/html" */
  case 'M':
    /* error to set this twice */
    if (logitem->mime_type)
      return handle_default_case_token (str, p);
    if (!(tkn = parse_string (&(*str), end, 1)))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, *p, NULL);

    normalize_mime_type (tkn, norm_mime, sizeof (norm_mime));
    if (norm_mime[0] != '\0')
      logitem->mime_type = strdup (norm_mime);
    else
      logitem->mime_type = NULL;
    free (tkn);

    break;
    /* move forward through str until not a space */
  case '~':
    find_alpha (&(*str));
    break;
    /* everything else skip it */
  default:
    handle_default_case_token (str, p);
  }

  return 0;
}

/* Parse the special host specifier and extract the characters that
 * need to be rejected when attempting to parse the XFF field.
 *
 * If no unable to find both curly braces (boundaries), NULL is returned.
 * On success, the malloc'd reject set is returned. */
static char *
extract_braces (const char **p) {
  const char *b1 = NULL, *b2 = NULL, *s = *p;
  char *ret = NULL;
  int esc = 0;
  ptrdiff_t len = 0;

  /* iterate over the log format */
  for (; *s; s++) {
    if (*s == '\\') {
      esc = 1;
    } else if (*s == '{' && !esc) {
      b1 = s;
    } else if (*s == '}' && !esc) {
      b2 = s;
      break;
    } else {
      esc = 0;
    }
  }

  if ((!b1) || (!b2))
    return NULL;
  if ((len = b2 - (b1 + 1)) <= 0)
    return NULL;

  /* Found braces, extract 'reject' character set. */
  ret = xmalloc (len + 1);
  memcpy (ret, b1 + 1, len);
  ret[len] = '\0';
  (*p) = b2 + 1;

  return ret;
}

/* Attempt to extract the client IP from an X-Forwarded-For (XFF) field.
 *
 * If no IP is found, 1 is returned.
 * On success, the malloc'd token is assigned to a GLogItem->host and
 * 0 is returned. */
static int
set_xff_host (GLogItem *logitem, const char *str, const char *skips, int out) {
  const char *ptr = NULL, *tkn = NULL;
  int invalid_ip = 1, len = 0, type_ip = TYPE_IPINV;
  int idx = 0, skips_len = 0;

  skips_len = strlen (skips);
  ptr = str;
  while (*ptr != '\0') {
    if ((len = strcspn (ptr, skips)) == 0) {
      len++, ptr++, idx++;
      goto move;
    }
    /* If our index does not match the number of delimiters and we have already a
     * valid client IP, then we assume we have reached the length of the XFF */
    if (idx < skips_len && logitem->host)
      break;

    ptr += len;
    /* extract possible IP */
    if (!(tkn = parsed_string (ptr, &str, 0)))
      break;

    invalid_ip = invalid_ipaddr (tkn, &type_ip);
    /* done, already have IP and current token is not a host */
    if (logitem->host && invalid_ip) {
      free ((void *) tkn);
      break;
    }
    if (!logitem->host && !invalid_ip) {
      logitem->host = xstrdup (tkn);
      logitem->type_ip = type_ip;
    }
    free ((void *) tkn);
    idx = 0;

    /* found the client IP, break then */
    if (logitem->host && out)
      break;

  move:
    str += len;
  }

  return logitem->host == NULL;
}

/* Attempt to find possible delimiters in the X-Forwarded-For (XFF) field.
 *
 * If no IP is found, 1 is returned.
 * On success, the malloc'd token is assigned to a GLogItem->host and 0 is returned. */
static int
find_xff_host (GLogItem *logitem, const char **str, const char **p) {
  char *skips = NULL, *extract = NULL;
  char pch[2] = { 0 };
  int res = 0;

  if (!(skips = extract_braces (p)))
    return spec_err (logitem, ERR_SPEC_SFMT_MIS, **p, "{}");

  /* if the log format current char is not within the braces special chars, then
   * we assume the range of IPs are within hard delimiters */
  if (!strchr (skips, **p) && strchr (*str, **p)) {
    *pch = **p;
    *(pch + 1) = '\0';
    if (!(extract = parse_string (&(*str), pch, 1)))
      goto clean;

    res = set_xff_host (logitem, extract, skips, 1);
    free (extract);
    (*str)++;   /* move a char forward from the trailing delim */
  } else {
    res = set_xff_host (logitem, *str, skips, 0);
  }

clean:
  free (skips);

  return res;
}

/* Handle special specifiers.
 *
 * On error, or unable to parse it, 1 is returned.
 * On success, the malloc'd token is assigned to a GLogItem member and
 * 0 is returned. */
static int
special_specifier (GLogItem *logitem, const char **str, const char **p) {
  switch (**p) {
    /* XFF remote hostname (IP only) */
  case 'h':
    if (find_xff_host (logitem, str, p))
      return spec_err (logitem, ERR_SPEC_TOKN_NUL, 'h', NULL);
    break;
  }

  return 0;
}

/* Iterate over the given log format.
 *
 * On error, or unable to parse it, 1 is returned.
 * On success, the malloc'd token is assigned to a GLogItem member and
 * 0 is returned. */
static int
parse_format (GLogItem *logitem, const char *str, const char *lfmt) {
  char end[2 + 1] = { 0 };
  const char *p = NULL, *last = NULL;
  int perc = 0, tilde = 0, ret = 0;

  if (str == NULL || *str == '\0')
    return 1;

  /* iterate over the log format */
  last = lfmt + strlen (lfmt);
  for (p = lfmt; p < last; p++) {
    if (*p == '%') {
      perc++;
      continue;
    }
    if (*p == '~' && perc == 0) {
      tilde++;
      continue;
    }
    if (*str == '\0')
      return spec_err (logitem, ERR_SPEC_LINE_INV, '-', NULL);
    if (*str == '\n')
      return 0;

    if (tilde && *p != '\0') {
      if (*str == '\0')
        return 0;
      if (special_specifier (logitem, &str, &p) == 1)
        return 1;
      tilde = 0;
    }
    /* %h */
    else if (perc && *p != '\0') {
      if (*str == '\0')
        return 0;

      memset (end, 0, sizeof end);
      get_delim (end, p);
      /* attempt to parse format specifiers */
      if ((ret = parse_specifier (logitem, &str, p, end)))
        return ret;
      perc = 0;
    } else if (perc && isspace ((unsigned char) p[0])) {
      return 1;
    } else {
      str++;
    }
  }

  return 0;
}

/* Determine if the log string is valid and if it's not a comment.
 *
 * On error, or invalid, 1 is returned.
 * On success, or valid line, 0 is returned. */
static int
valid_line (char *line) {
  /* invalid line */
  if ((line == NULL) || (*line == '\0'))
    return 1;
  /* ignore comments */
  if (*line == '#' || *line == '\n')
    return 1;

  return 0;
}

/* Ignore request's query string. e.g.,
 * /index.php?timestamp=1454385289 */
static void
strip_qstring (char *req) {
  char *qmark;
  if ((qmark = strchr (req, '?')) != NULL) {
    if ((qmark - req) > 0)
      *qmark = '\0';
  }
}


/* Output all log errors stored during parsing. */
void
output_logerrors (void) {
  Logs *logs = get_db_logs (DB_INSTANCE);
  GLog *glog = NULL;
  int pid = getpid (), i;

  for (i = 0; i < logs->size; ++i) {
    glog = &logs->glog[i];
    if (!glog->log_erridx)
      continue;

    fprintf (stderr, "==%d== GoAccess - version %s - %s %s\n", pid, GO_VERSION, __DATE__, __TIME__);
    fprintf (stderr, "==%d== Config file: %s\n", pid, conf.iconfigfile ? : NO_CONFIG_FILE);
    fprintf (stderr, "==%d== https://goaccess.io - <hello@goaccess.io>\n", pid);
    fprintf (stderr, "==%d== Released under the MIT License.\n", pid);
    fprintf (stderr, "==%d==\n", pid);
    fprintf (stderr, "==%d== FILE: %s\n", pid, glog->props.filename);
    fprintf (stderr, "==%d== ", pid);
    fprintf (stderr, ERR_PARSED_NLINES, glog->log_erridx);
    fprintf (stderr, " %s:\n", ERR_PARSED_NLINES_DESC);
    fprintf (stderr, "==%d==\n", pid);
    for (i = 0; i < glog->log_erridx; ++i)
      fprintf (stderr, "==%d== %s\n", pid, glog->errors[i]);
  }
  fprintf (stderr, "==%d==\n", pid);
  fprintf (stderr, "==%d== %s\n", pid, ERR_FORMAT_HEADER);
  fprintf (stderr, "==%d== %s\n", pid, HINT_INVALID_REQUESTS);
}

/* Ensure we have the following fields. */
static int
verify_missing_fields (GLogItem *logitem) {
  /* must have the following fields */
  if (logitem->host == NULL)
    logitem->errstr = xstrdup ("IPv4/6 is required. You have to add format specifier '%h' [host (the client IP address, either IPv4 or IPv6)] to your log-format.");
  else if (logitem->date == NULL)
    logitem->errstr = xstrdup ("A valid date is required. You have to add format specifier '%x' [Datetime] or '%d' [Date] and '%t' [Time] to your log-format.");
  else if (logitem->req == NULL)
    logitem->errstr = xstrdup ("A request is required. Your log-format is missing format specifier '%r' [The request line from the client] or combination of special format specifiers such as '%m', '%U', '%q' and '%H' to parse individual fields.");

  return logitem->errstr != NULL;
}

/* Determine if the request is from a robot or spider and check if we
 * need to ignore or show crawlers only.
 *
 * If the request line is not ignored, 0 is returned.
 * If the request line is ignored, 1 is returned. */
static int
handle_crawler (const char *agent) {
  int bot = 0;

  if (!conf.ignore_crawlers && !conf.crawlers_only)
    return 1;

  bot = is_crawler (agent);
  return (conf.ignore_crawlers && bot) || (conf.crawlers_only && !bot) ? 0 : 1;
}

/* A wrapper function to determine if the request is static.
 *
 * If the request is not static, 0 is returned.
 * If the request is static, 1 is returned. */
static int
is_static (const char *req) {
  return verify_static_content (req);
}

/* Determine if the request of the given status code needs to be
 * ignored.
 *
 * If the status code is not within the ignore-array, 0 is returned.
 * If the status code is within the ignore-array, 1 is returned. */
static int
ignore_status_code (int status) {
  int i = 0;

  if (!status || conf.ignore_status_idx == 0)
    return 0;

  for (i = 0; i < conf.ignore_status_idx; i++)
    if (status == conf.ignore_status[i])
      return 1;

  return 0;
}

/* Determine if static file request should be ignored
    *
    * If the request line is not ignored, 0 is returned.
    * If the request line is ignored, 1 is returned. */
static int
ignore_static (const char *req) {
  if (conf.ignore_statics && is_static (req))
    return 1;
  return 0;
}

/* Determine if the request status code is a 404.
 *
 * If the request is not a 404, 0 is returned.
 * If the request is a 404, 1 is returned. */
static int
is_404 (GLogItem *logitem) {
  /* is this a 404? */
  if (logitem->status == 404)
    return 1;
  /* treat 444 as 404? */
  else if (logitem->status == 444 && conf.code444_as_404)
    return 1;
  return 0;
}

/* A wrapper function to determine if a log line needs to be ignored.
 *
 * If the request line is not ignored, 0 is returned.
 * If the request line is ignored, IGNORE_LEVEL_PANEL is returned.
 * If the request line is only not counted as valid, IGNORE_LEVEL_REQ is returned. */
static int
ignore_line (GLogItem *logitem) {
  if (excluded_ip (logitem) == 0)
    return IGNORE_LEVEL_PANEL;
  if (handle_crawler (logitem->agent) == 0)
    return IGNORE_LEVEL_PANEL;
  if (ignore_referer (logitem->ref))
    return IGNORE_LEVEL_PANEL;
  if (ignore_status_code (logitem->status))
    return IGNORE_LEVEL_PANEL;
  if (ignore_static (logitem->req))
    return conf.ignore_statics; // IGNORE_LEVEL_PANEL or IGNORE_LEVEL_REQ

  /* check if we need to remove the request's query string */
  if (conf.ignore_qstr)
    strip_qstring (logitem->req);

  return 0;
}

/* The following generates a unique key to identity unique visitors.
 * The key is made out of the IP, date, and user agent.
 * Note that for readability, doing a simple snprintf/sprintf should
 * suffice, however, memcpy is the fastest solution
 *
 * On success the new unique visitor key is returned */
static char *
get_uniq_visitor_key (GLogItem *logitem) {
  char *key = NULL;
  size_t s1, s2, s3;

  s1 = strlen (logitem->date);
  s2 = strlen (logitem->host);
  s3 = strlen (logitem->agent_hex);

  /* includes terminating null */
  key = xcalloc (s1 + s2 + s3 + 3, sizeof (char));

  memcpy (key, logitem->date, s1);

  key[s1] = '|';
  memcpy (key + s1 + 1, logitem->host, s2 + 1);

  key[s1 + s2 + 1] = '|';
  memcpy (key + s1 + s2 + 2, logitem->agent_hex, s3 + 1);

  return key;
}

/* Determine if the current log has the content from the last time it was
 * parsed. It does this by comparing READ_BYTES against the beginning of the
 * log.
 *
 * Returns 1 if the content is likely the same or no data to compare
 * Returns 0 if it has different content */
static int
is_likely_same_log (GLog *glog, const GLastParse *lp) {
  size_t size = 0;

  if (!lp->size)
    return 1;

  /* Must be a LOG */
  size = MIN (glog->snippetlen, lp->snippetlen);
  if (glog->snippet[0] != '\0' && lp->snippet[0] != '\0' &&
      memcmp (glog->snippet, lp->snippet, size) == 0)
    return 1;

  return 0;
}

/* Determine if we should insert new record or if it's a duplicate record from
 * a previously persisted dataset
 *
 * Returns 1 if it thinks the record it's being restored from disk
 * Returns 0 if we need to parse the record */
static int
should_restore_from_disk (GLog *glog) {
  GLastParse lp = { 0 };

  if (!conf.restore)
    return 0;

  lp = ht_get_last_parse (glog->props.inode);

  /* No last parse timestamp, continue parsing as we got nothing to compare
   * against */
  if (!lp.ts)
    return 0;

  /* If our current line is greater or equal (zero indexed) to the last parsed
   * line and have equal timestamps, then keep parsing then */
  if (glog->props.inode && is_likely_same_log (glog, &lp)) {
    if (glog->props.size > lp.size && glog->read >= lp.line)
      return 0;
    return 1;
  }

  /* No inode (probably a pipe), prior or equal timestamps means restore from
   * disk (exclusive) */
  if (!glog->props.inode && lp.ts >= glog->lp.ts)
    return 1;

  /* If not likely the same content, then fallback to the following checks */
  /* If timestamp is greater than last parsed, read the line then */
  if (glog->lp.ts > lp.ts)
    return 0;

  /* Check if current log size is smaller than the one last parsed, if it is,
   * it was possibly truncated and thus it may be smaller, so fallback to
   * timestamp even if they are equal to the last parsed timestamp */
  else if (glog->props.size < lp.size && glog->lp.ts == lp.ts)
    return 0;

  /* Everything else we ignore it. For instance, if current log size is
   * greater than the one last parsed, or the timestamp are equal, we ignore the
   * request.
   *
   * **NOTE* We try to play safe here as we would rather miss a few lines
   * than double-count a few. */
  return 1;
}

static void
process_invalid (GLog *glog, GLogItem *logitem, const char *line) {
  GLastParse lp = { 0 };

  /* if not restoring from disk, then count entry as proceeded and invalid */
  if (!conf.restore) {
    count_process_and_invalid (glog, logitem, line);
    return;
  }

  lp = ht_get_last_parse (glog->props.inode);

  /* If our current line is greater or equal (zero indexed) to the last parsed
   * line then keep parsing then */
  if (glog->props.inode && is_likely_same_log (glog, &lp)) {
    /* only count invalids if we're past the last parsed line */
    if (glog->props.size > lp.size && glog->read >= lp.line)
      count_process_and_invalid (glog, logitem, line);
    return;
  }

  /* no timestamp to compare against, just count the invalid then */
  if (!logitem->numdate) {
    count_process_and_invalid (glog, logitem, line);
    return;
  }

  /* if there's a valid timestamp, count only if greater than last parsed ts */
  if ((glog->lp.ts = mktime (&logitem->dt)) == -1)
    return;

  /* check if we were able to at least parse the date/time, if no date/time
   * then we simply don't count the entry as proceed & invalid to attempt over
   * counting restored data */
  if (should_restore_from_disk (glog) == 0)
    count_process_and_invalid (glog, logitem, line);
}

static int
parse_json_specifier (void *ptr_data, char *key, char *str) {
  GLogItem *logitem = (GLogItem *) ptr_data;
  char *spec = NULL;
  int ret = 0;

  if (!key || !str)
    return 0;
  /* empty JSON value, e.g., {method: ""} */
  if (0 == strlen (str))
    return 0;
  if (!(spec = ht_get_json_logfmt (key)))
    return 0;

  ret = parse_format (logitem, str, spec);
  free (spec);

  return ret;
}

static int
parse_json_format (GLogItem *logitem, char *str) {
  return parse_json_string (logitem, str, parse_json_specifier);
}

/* Atomically updates glog->lp.ts with the maximum timestamp value from
 * logitem->dt.
 *
 * On error (if mktime fails), returns -1.
 * On success, returns the updated timestamp value, which is also stored in
 * glog->lp.ts.
 */
static int
atomic_lpts_update (GLog *glog, GLogItem *logitem) {
  int64_t newts = mktime (&logitem->dt); // Get timestamp from logitem->dt
  int64_t oldts = __atomic_load_n (&glog->lp.ts, __ATOMIC_SEQ_CST);
  int64_t expected;

  while (oldts < newts) { // Only update if new timestamp is later
    expected = oldts;
    // Attempt to update glog->lp.ts from expected (oldts) to newts.
    if (__atomic_compare_exchange_n (&glog->lp.ts, &expected, newts,
                                     false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      break;    // Successful update.
    }
    // If the CAS failed, expected now holds the current glog->lp.ts.
    oldts = expected;
    if (oldts >= newts)
      break;
  }

  return newts;
}

static int
cleanup_logitem (int ret, GLogItem *logitem) {
  free_glog (logitem);
  return ret;
}

/* Process a line from the log and store it accordingly taking into
 * account multiple parsing options prior to setting data into the
 * corresponding data structure.
 *
 * On error, logitem->errstr will contains the error message. */
int
parse_line (GLog *glog, char *line, int dry_run, GLogItem **logitem_out) {
  char *fmt = conf.log_format;
  int ret = 0;
  GLogItem *logitem = NULL;

  /* soft ignore these lines */
  if (valid_line (line))
    return -1;

  logitem = init_log_item (glog);

  /* Parse a line of log, and fill structure with appropriate values */
  if (conf.is_json_log_format)
    ret = parse_json_format (logitem, line);
  else
    ret = parse_format (logitem, line, fmt);

  /* invalid log line (format issue) */
  if (ret) {
    process_invalid (glog, logitem, line);
    return cleanup_logitem (ret, logitem);
  }

  if (!glog->piping && conf.fname_as_vhost && glog->fname_as_vhost)
    logitem->vhost = xstrdup (glog->fname_as_vhost);

  /* valid format but missing fields */
  if (ret || (ret = verify_missing_fields (logitem))) {
    process_invalid (glog, logitem, line);
    return cleanup_logitem (ret, logitem);
  }

  /* From here on, valid format but possible ignoring of lines */
  if (atomic_lpts_update (glog, logitem) == -1)
    return cleanup_logitem (ret, logitem);

  if (should_restore_from_disk (glog))
    return cleanup_logitem (ret, logitem);

  count_process (glog);

  /* testing log only */
  if (dry_run)
    return cleanup_logitem (ret, logitem);

  /* agent will be null in cases where %u is not specified */
  if (logitem->agent == NULL) {
    logitem->agent = alloc_string ("-");
    set_agent_hash (logitem);
  }

  logitem->ignorelevel = ignore_line (logitem);
  /* ignore line */
  if (logitem->ignorelevel == IGNORE_LEVEL_PANEL)
    return cleanup_logitem (ret, logitem);

  if (is_404 (logitem))
    logitem->is_404 = 1;
  else if (is_static (logitem->req))
    logitem->is_static = 1;

  logitem->uniq_key = get_uniq_visitor_key (logitem);
  *logitem_out = logitem;

  return ret;
}

/* Entry point to process the given line from the log.
 *
 * On error, NULL is returned.
 * On success or soft ignores, GLogItem is returned. */
static GLogItem *
read_line (GLog *glog, char *line, int *test, uint32_t *cnt, int dry_run) {
  GLogItem *logitem = NULL;
  int ret = 0;

  /* Begin processing the log line - in case of an invalid log format, flip
   * the test only if there's at least one valid record discovered during the log
   * format test. This condition applies solely when reading a log from the
   * beginning, not when tailing an ongoing log. */
  if ((ret = parse_line (glog, line, dry_run, &logitem)) == 0)
    *test = 0;

  /* soft ignore these lines from parse_line */
  if (ret == -1)
    return NULL;

  /* reached num of lines to test and no valid records were found, log
   * format is likely not matching */
  if (conf.num_tests && ++(*cnt) >= conf.num_tests && *test) {
    uncount_processed (glog);
    uncount_invalid (glog);
    return NULL;
  }
  glog->read++;

  return logitem;
}

static void *
read_lines_thread (void *arg) {
  GJob *job = (GJob *) arg;
  int i = 0;

  for (i = 0; i < job->p; i++) {
    job->logitems[i] = read_line (job->glog, job->lines[i], &job->test, &job->cnt, job->dry_run);
#ifdef WITH_GETLINE
    free (job->lines[i]);
#endif
  }
  return (void *) 0;
}

/* A replacement for GNU getline() to dynamically expand fgets buffer.
 *
 * On error, NULL is returned.
 * On success, the malloc'd line is returned. */
char *
fgetline (FILE *fp) {
  char buf[LINE_BUFFER] = { 0 };
  char *line = NULL, *tmp = NULL;
  size_t linelen = 0, len = 0;

  while (1) {
    if (!fgets (buf, sizeof (buf), fp)) {
      if (conf.process_and_exit && errno == EAGAIN) {
        (void) nanosleep ((const struct timespec[]) { {0, 100000000L} }, NULL);
        continue;
      } else
        break;
    }

    if (*buf == '\0')
      break;

    len = strlen (buf);

    /* overflow check */
    if (SIZE_MAX - len - 1 < linelen)
      break;

    if ((tmp = realloc (line, linelen + len + 1)) == NULL)
      break;

    line = tmp;
    /* append */
    strcpy (line + linelen, buf);
    linelen += len;

    if (feof (fp) || buf[len - 1] == '\n')
      return line;
  }
  free (line);

  return NULL;
}

void *
process_lines_thread (void *arg) {
  GJob *job = (GJob *) arg;
  int i = 0;

  for (i = 0; i < job->p; i++) {
    if (job->logitems[i] != NULL && !job->dry_run && job->logitems[i]->errstr == NULL) {
      process_log (job->logitems[i]);
      free_glog (job->logitems[i]);
    }
  }
  return (void *) 0;
}

/* Initialize jobs */
static void
init_jobs (GJob jobs[2][conf.jobs], GLog *glog, int dry_run, int test) {
  int b = 0, k = 0;
#ifndef WITH_GETLINE
  int i = 0;
#endif

  for (b = 0; b < 2; b++) {
    for (k = 0; k < conf.jobs; k++) {
      jobs[b][k].p = 0;
      jobs[b][k].cnt = 0;
      jobs[b][k].glog = glog;
      jobs[b][k].test = test;
      jobs[b][k].dry_run = dry_run;
      jobs[b][k].running = 0;
      jobs[b][k].logitems = xcalloc (conf.chunk_size, sizeof (GLogItem));
      jobs[b][k].lines = xcalloc (conf.chunk_size, sizeof (char *));
#ifndef WITH_GETLINE
      for (i = 0; i < conf.chunk_size; i++)
        jobs[b][k].lines[i] = xcalloc (LINE_BUFFER, sizeof (char));
#endif
    }
  }
}

/* Read lines from file */
static void
read_lines_from_file (FILE *fp, GLog *glog, GJob jobs[2][conf.jobs], int b, char **s) {
  int k = 0;

  for (k = 0; k < conf.jobs; k++) {
#ifdef WITH_GETLINE
    while ((*s = fgetline (fp)) != NULL) {
      jobs[b][k].lines[jobs[b][k].p] = *s;
#else
    while ((*s = fgets (jobs[b][k].lines[jobs[b][k].p], LINE_BUFFER, fp)) != NULL) {
#endif
      glog->bytes += strlen (jobs[b][k].lines[jobs[b][k].p]);

      if (++(jobs[b][k].p) >= conf.chunk_size)
        break;  // goto next chunk
    }
  }
}

/* Processes lines using threads from the GJob array, updating counters. */
static void
process_lines (GJob jobs[2][conf.jobs], uint32_t *cnt, int *test, int b) {
  int k = 0;

  for (k = 0; k < conf.jobs; k++) {
    process_lines_thread (&jobs[b][k]);
    *cnt += jobs[b][k].cnt;
    jobs[b][k].cnt = 0;
    *test &= jobs[b][k].test;
    jobs[b][k].p = 0;
  }
}

/* Frees memory for lines and logitems in each job of the GJob array. */
static void
free_jobs (GJob jobs[2][conf.jobs]) {
  int b = 0, k = 0;
#ifndef WITH_GETLINE
  int i = 0;
#endif

  for (b = 0; b < 2; b++) {
    for (k = 0; k < conf.jobs; k++) {
#ifndef WITH_GETLINE
      for (i = 0; i < conf.chunk_size; i++)
        free (jobs[b][k].lines[i]);
#endif
      free (jobs[b][k].logitems);
      free (jobs[b][k].lines);
    }
  }
}

/* To encapsulate the initial log format sniffing and processing.
 * Returns 0 on success (format verified), 1 on error (format mismatch). */
static int
perform_initial_sniff (FILE *fp, GLog *glog, GJob jobs[2][conf.jobs], int dry_run,
                       int *global_test_flag) {
  uint32_t initial_cnt = 0;
  /* Flag to track if a valid format has been found (1 = no, 0 = yes) */
  int initial_test = 1;
  char *s = NULL;

  /* Read and parse lines until `conf.num_tests` are processed or EOF. */
  while (initial_cnt < conf.num_tests && (s = fgetline (fp)) != NULL) {
    GLogItem *logitem = read_line (glog, s, &initial_test, &initial_cnt, dry_run);

    if (logitem != NULL) {
      /* Intentional: If in actual processing mode (`dry_run == 0`) and valid,
       * process the log item. This prevents redundant reads and supports pipes. */
      if (!dry_run && logitem->errstr == NULL) {
        process_log (logitem);
        free_glog (logitem);
      }
      /* `read_line` or subsequent logic handles freeing `logitem` otherwise. */
    }
    free (s);   /* Free buffer from fgetline. */
  }

  /* If no valid format was found within `conf.num_tests` lines, signal an
   * error. */
  if (initial_test) {
    uncount_processed (glog);
    uncount_invalid (glog);
    free_jobs (jobs); /* Clean up all job structures. */
    return 1;   /* Indicate format mismatch/error. */
  }

  /* Format verified. Prepare for multi-threaded phase: The file pointer is
   * deliberately not reset (no `fseek`). This allows continuous reading from the
   * current position, crucial for pipes and efficiency. */
  *global_test_flag = 0; /* Update global `test` flag to disable further format testing. */

  /* Propagate disabled 'test' status to all GJob structures to prevent
   * premature termination. */
  for (int b = 0; b < 2; b++) {
    for (int k = 0; k < conf.jobs; k++) {
      jobs[b][k].test = 0;
    }
  }
  return 0;     /* Success: format verified. */
}

/* Reads lines from the given file pointer `fp` and processes them using
 * parallel threads.
 *
 * On error or when interrupted by a signal (SIGINT), the function returns 0.
 * On success, it returns 1 if the number of processed lines is greater than or
 * equal to the configured number of tests (NUM_TESTS), otherwise 0.
 */
static int
read_lines (FILE *fp, GLog *glog, int dry_run) {
  int b = 0, k = 0, test = conf.num_tests > 0 ? 1 : 0;
  uint32_t cnt = 0;
  void *status = NULL;
  char *s = NULL;
  GJob jobs[2][conf.jobs];
  pthread_t threads[conf.jobs];

  glog->bytes = 0;

  init_jobs (jobs, glog, dry_run, test);

  /* Perform initial single-threaded log format sniffing if `conf.num_tests` is
   * active.  This helps validate the log format early and sets up the state for
   * multi-threaded processing. */
  if (test) {
    if (perform_initial_sniff (fp, glog, jobs, dry_run, &test) != 0) {
      return 1; /* Sniffing failed (log format mismatch). */
    }
    cnt = 0;    /* Reset main counter after sniff, as initial lines are handled. */
  }

  b = 0;
  while (1) {   /* b = 0 or 1 */
    read_lines_from_file (fp, glog, jobs, b, &s);

    /* if nothing was read from the log, skip it for now */
    if (!glog->bytes) {
      test = 0;
      break;
    }

    if (conf.jobs == 1) {
      read_lines_thread (&jobs[b][0]);
    } else {
      for (k = 0; k < conf.jobs; k++) {
        jobs[b][k].running = 1;
        pthread_create (&threads[k], NULL, read_lines_thread, (void *) &jobs[b][k]);
      }
    }

    /* flip from block A/B to B/A */
    if (conf.jobs > 1)
      b = b ^ 1;

    process_lines (jobs, &cnt, &test, b);

    /* flip from block B/A to A/B */
    if (conf.jobs > 1)
      b = b ^ 1;

    if (conf.jobs > 1) {
      for (k = 0; k < conf.jobs; k++) {
        if (jobs[b][k].running) {
          pthread_join (threads[k], &status);
          jobs[b][k].running = 0;
        }
      }
    }

    if (dry_run && cnt >= NUM_TESTS)
      break;

    /* handle SIGINT */
    if (conf.stop_processing)
      break;

    /* check for EOF */
    if (s == NULL)
      break;

    /* flip from block A/B to B/A */
    if (conf.jobs > 1)
      b = b ^ 1;
  }             // while (1)

  /* After EOF, process last data */
  for (b = 0; b < 2; b++) {
    for (k = 0; k < conf.jobs; k++) {
      if (conf.jobs > 1 && jobs[b][k].running) {
        pthread_join (threads[k], &status);
        jobs[b][k].running = 0;
      }

      if (jobs[b][k].p) {
        process_lines_thread (&jobs[b][k]);
        cnt += jobs[b][k].cnt;
        jobs[b][k].cnt = 0;
        test &= jobs[b][k].test;
        jobs[b][k].p = 0;
      }
    }
  }

  free_jobs (jobs);

  /* Handle pipe case with insufficient data */
  if (!s && (errno == EAGAIN || errno == EWOULDBLOCK) && test && cnt < conf.num_tests)
    return 0;

  return test;
}

/* Read the given log file and attempt to mmap a fixed number of bytes so we
 * can compare its content on future runs.
 *
 * On error, 1 is returned.
 * On success, 0 is returned. */
int
set_initial_persisted_data (GLog *glog, FILE *fp, const char *fn) {
  size_t len;
  time_t now = time (0);

  /* reset the snippet */
  memset (glog->snippet, 0, sizeof (glog->snippet));
  glog->snippetlen = 0;

  if (glog->props.size == 0)
    return 1;

  len = MIN (glog->props.size, READ_BYTES);
  if ((fread (glog->snippet, len, 1, fp)) != 1 && ferror (fp))
    FATAL ("Unable to fread the specified log file '%s'", fn);
  glog->snippetlen = len;
  localtime_r (&now, &glog->start_time);

  fseek (fp, 0, SEEK_SET);

  return 0;
}

static void
persist_last_parse (GLog *glog) {
  /* insert last parsed data for the recently file parsed */
  if (glog->props.inode && glog->props.size) {
    glog->lp.line = glog->read;
    glog->lp.snippetlen = glog->snippetlen;

    memcpy (glog->lp.snippet, glog->snippet, glog->snippetlen);

    ht_insert_last_parse (glog->props.inode, &glog->lp);
  }
  /* probably from a pipe */
  else if (!glog->props.inode) {
    ht_insert_last_parse (0, &glog->lp);
  }
}

/* Read the given log line by line and process its data.
 *
 * On error, 1 is returned.
 * On success, 0 is returned. */
static int
read_log (GLog *glog, int dry_run) {
  FILE *fp = NULL;
  int piping = 0;
  struct stat fdstat;

  /* Ensure we have a valid pipe to read from stdin. Only checking for
   * conf.read_stdin without verifying for a valid FILE pointer would certainly
   * lead to issues. */
  if (glog->props.filename[0] == '-' && glog->props.filename[1] == '\0' && glog->pipe) {
    fp = glog->pipe;
    glog->piping = piping = 1;
  }

  /* make sure we can open the log (if not reading from stdin) */
  if (!piping && (fp = fopen (glog->props.filename, "r")) == NULL)
    FATAL ("Unable to open the specified log file '%s'. %s", glog->props.filename,
           strerror (errno));

  /* grab the inode of the file being parsed */
  if (!piping && stat (glog->props.filename, &fdstat) == 0) {
    glog->props.inode = fdstat.st_ino;
    glog->props.size = glog->lp.size = fdstat.st_size;
    set_initial_persisted_data (glog, fp, glog->props.filename);
  }

  /* read line by line */
  if (read_lines (fp, glog, dry_run)) {
    if (!piping)
      fclose (fp);
    return 1;
  }

  persist_last_parse (glog);

  /* close log file if not a pipe */
  if (!piping)
    fclose (fp);

  return 0;
}

static void
set_log_processing (Logs *logs, GLog *glog) {
  lock_spinner ();
  logs->processed = &(glog->processed);
  logs->filename = glog->props.filename;
  unlock_spinner ();
}

/* Entry point to parse the log line by line.
 *
 * On error, 1 is returned.
 * On success, 0 is returned. */
int
parse_log (Logs *logs, int dry_run) {
  GLog *glog = NULL;
  const char *err_log = NULL;
  int idx;

  /* verify that we have the required formats */
  if ((err_log = verify_formats ()))
    FATAL ("%s", err_log);

  /* no data piped, no logs passed, load from disk only then */
  if (conf.restore && !logs->restored)
    logs->restored = rebuild_rawdata_cache ();

  /* no data piped, no logs passed, load from disk only then */
  if (conf.restore && !conf.filenames_idx && !conf.read_stdin) {
    logs->load_from_disk_only = 1;
    return 0;
  }

  for (idx = 0; idx < logs->size; ++idx) {
    glog = &logs->glog[idx];
    set_log_processing (logs, glog);

    if (read_log (glog, dry_run))
      return 1;

    glog->length = glog->bytes;
  }

  return 0;
}

/* Ensure we have valid hits
 *
 * On error, an array of pointers containing the error strings.
 * On success, NULL is returned. */
char **
test_format (Logs *logs, int *len) {
  char **errors = NULL;
  GLog *glog = NULL;
  int i;

  if (parse_log (logs, 1) == 0)
    return NULL;

  for (i = 0; i < logs->size; ++i) {
    glog = &logs->glog[i];
    if (!glog->log_erridx)
      continue;
    break;
  }

  errors = xcalloc (glog->log_erridx, sizeof (char *));
  *len = glog->log_erridx;
  for (i = 0; i < glog->log_erridx; ++i)
    errors[i] = xstrdup (glog->errors[i]);
  free_logerrors (glog);

  return errors;
}
