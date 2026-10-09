/* The frontend and task-queue symbols network/romm/romm_client.c
 * reaches for. Every request the client starts is recorded, not sent:
 * the test plays RomM by feeding the recorded callback a response. */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <compat/strl.h>

#include "../../../tasks/tasks_internal.h"

#include "stubs_retroarch.h"

void RARCH_LOG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
}

void RARCH_WARN(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   printf("WARN: ");
   vprintf(fmt, ap);
   va_end(ap);
}

void RARCH_ERR(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   printf("ERR: ");
   vprintf(fmt, ap);
   va_end(ap);
}

stub_request_t stub_req;

void stub_reset(void)
{
   memset(&stub_req, 0, sizeof(stub_req));
}

void *task_push_http_transfer_with_headers(const char *url, bool mute,
      const char *type, const char *headers,
      retro_task_callback_t cb, void *user_data)
{
   (void)mute; (void)type;
   if (stub_req.refuse)
      return NULL;
   stub_req.count++;
   strlcpy(stub_req.method, "GET", sizeof(stub_req.method));
   strlcpy(stub_req.url, url, sizeof(stub_req.url));
   strlcpy(stub_req.headers, headers ? headers : "",
         sizeof(stub_req.headers));
   stub_req.cb        = cb;
   stub_req.user_data = user_data;
   /* Any non-NULL handle: callers only test it for NULL. */
   return &stub_req;
}

void *task_push_http_transfer_with_content(const char *url,
      const char *method, const void *content, size_t content_len,
      const char *content_type, bool mute, bool headers_accept_err,
      const char *headers, retro_task_callback_t cb, void *user_data)
{
   (void)mute; (void)headers_accept_err;
   if (stub_req.refuse)
      return NULL;
   stub_req.count++;
   strlcpy(stub_req.method, method, sizeof(stub_req.method));
   strlcpy(stub_req.url, url, sizeof(stub_req.url));
   strlcpy(stub_req.headers, headers ? headers : "",
         sizeof(stub_req.headers));
   strlcpy(stub_req.content_type, content_type ? content_type : "",
         sizeof(stub_req.content_type));
   stub_req.body[0] = '\0';
   if (content_len < sizeof(stub_req.body))
   {
      memcpy(stub_req.body, content, content_len);
      stub_req.body[content_len] = '\0';
   }
   stub_req.cb        = cb;
   stub_req.user_data = user_data;
   return &stub_req;
}
