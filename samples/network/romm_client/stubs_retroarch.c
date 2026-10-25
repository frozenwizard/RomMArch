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

void RARCH_DBG(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
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
stub_logged_t stub_log[STUB_LOG_MAX];
unsigned stub_log_count;

static void stub_log_request(void)
{
   stub_logged_t *l = &stub_log[stub_log_count++ % STUB_LOG_MAX];
   l->cb        = stub_req.cb;
   l->user_data = stub_req.user_data;
   strlcpy(l->url, stub_req.url, sizeof(l->url));
}
retro_time_t stub_now;
retro_task_t *stub_task;
bool stub_refuse_task;

void stub_reset(void)
{
   memset(&stub_req, 0, sizeof(stub_req));
}

retro_time_t cpu_features_get_time_usec(void)
{
   return stub_now;
}

retro_task_t *task_init(void)
{
   return (retro_task_t*)calloc(1, sizeof(retro_task_t));
}

bool task_queue_push(retro_task_t *task)
{
   if (stub_refuse_task)
      return false;
   stub_task = task;
   return true;
}

void task_queue_cancel_task(void *task)
{
   task_set_flags((retro_task_t*)task, RETRO_TASK_FLG_CANCELLED, true);
}

void task_set_flags(retro_task_t *task, uint8_t flags, bool set)
{
   if (set)
      task->flags |= flags;
   else
      task->flags &= ~flags;
}

uint8_t task_get_flags(retro_task_t *task)
{
   return (uint8_t)task->flags;
}

void task_set_progress(retro_task_t *task, int8_t progress)
{
   task->progress = progress;
}

void task_set_title(retro_task_t *task, char *title)
{
   task->title = title;
}

void task_free_title(retro_task_t *task)
{
   free(task->title);
   task->title = NULL;
}

void *task_push_http_transfer_with_headers(const char *url, bool mute,
      const char *type, const char *headers,
      retro_task_callback_t cb, void *user_data)
{
   (void)mute;
   if (stub_req.refuse)
      return NULL;
   stub_req.count++;
   strlcpy(stub_req.method, type ? type : "GET", sizeof(stub_req.method));
   strlcpy(stub_req.url, url, sizeof(stub_req.url));
   strlcpy(stub_req.headers, headers ? headers : "",
         sizeof(stub_req.headers));
   stub_req.cb        = cb;
   stub_req.user_data = user_data;
   stub_log_request();
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
   stub_req.body_len = content_len;
   if (content_len < sizeof(stub_req.body))
   {
      memcpy(stub_req.body, content, content_len);
      stub_req.body[content_len] = '\0';
   }
   stub_req.cb        = cb;
   stub_req.user_data = user_data;
   stub_log_request();
   return &stub_req;
}
