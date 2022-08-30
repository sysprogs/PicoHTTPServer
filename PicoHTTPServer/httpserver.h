#pragma once

typedef void *http_server_instance;

http_server_instance http_server_create(const char *main_host, const char *main_domain, int max_thread_count, int buffer_size);
