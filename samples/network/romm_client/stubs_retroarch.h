#ifndef ROMM_CLIENT_STUBS_H
#define ROMM_CLIENT_STUBS_H

#include <boolean.h>

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

void stub_reset(void);

#endif
