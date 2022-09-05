#pragma once

typedef struct _http_server_instance *http_server_instance;
typedef struct _http_connection *http_connection, *http_write_handle;

enum http_request_type
{
	HTTP_GET  = 0,
	HTTP_POST = 1,
};

typedef bool(*http_request_handler)(http_connection conn, enum http_request_type type, char *path, void *context);

typedef struct http_zone
{
	const char *prefix;
	http_request_handler handler;
	void *context;
	struct http_zone *next;
	int prefix_len;
} http_zone;


http_server_instance http_server_create(const char *main_host, const char *main_domain, int max_thread_count, int buffer_size);
void http_server_add_zone(http_server_instance server, http_zone *instance, const char *prefix, http_request_handler handler, void *context);
void http_server_send_reply(http_connection conn, const char *code, const char *contentType, const char *content, int size);

/* Reads a single line from the POST request using the internal connection buffer. Returns NULL when the entire request has been read. */
char *http_server_read_post_line(http_connection conn);


http_write_handle http_server_begin_write_reply(http_connection conn, const char *code, const char *contentType);
void http_server_write_reply(http_write_handle handle, const char *format, ...);
void http_server_end_write_reply(http_write_handle handle, const char *footer);
