#ifndef ROMM_CLIENT_STUBS_H
#define ROMM_CLIENT_STUBS_H

#include <boolean.h>
#include <queues/task_queue.h>

#include "../../../tasks/tasks_internal.h"

/* The most recent request the client started, and how many it has
 * started since stub_reset(). Set @refuse to make starting one fail. */
typedef struct
{
   unsigned count;
   bool refuse;
   char method[16];
   char url[1024];
   char headers[1024];
   char content_type[64];
   char body[4096];
   retro_task_callback_t cb;
   void *user_data;
} stub_request_t;

extern stub_request_t stub_req;

/* Every request since the program started: request n (counting from
 * 1 across resets) is stub_log[(n - 1) % STUB_LOG_MAX]. */
#define STUB_LOG_MAX 64
typedef struct
{
   retro_task_callback_t cb;
   void *user_data;
   char url[1024];
} stub_logged_t;

extern stub_logged_t stub_log[STUB_LOG_MAX];
extern unsigned stub_log_count;

/* The clock cpu_features_get_time_usec() reports, the last task the
 * client pushed, and whether pushing one fails. */
extern retro_time_t stub_now;
extern retro_task_t *stub_task;
extern bool stub_refuse_task;

void stub_reset(void);

#endif
