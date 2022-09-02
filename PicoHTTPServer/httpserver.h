#pragma once

typedef struct _http_server_instance *http_server_instance;
typedef struct _http_connection *http_connection;

typedef bool(*http_request_handler)(http_connection conn, char *path, void *context);

typedef struct http_zone
{
	char *prefix;
	http_request_handler handler;
	void *context;
	struct http_zone *next;
	int prefix_len;
} http_zone;


http_server_instance http_server_create(const char *main_host, const char *main_domain, int max_thread_count, int buffer_size);
void http_server_add_zone(http_server_instance server, http_zone *instance, const char *prefix, http_request_handler handler, void *context);
void http_server_send_reply(http_connection conn, const char *code, const char *contentType, const char *content, int size);
