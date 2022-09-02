#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
#include "debug_printf.h"
#include "httpserver.h"

struct _http_server_instance
{
	int socket;
	int buffer_size;
	const char *hostname;
	const char *domain_name;
	xSemaphoreHandle semaphore;
	http_zone *first_zone;
};

struct _http_connection
{
	http_server_instance server;
	int socket;
	char buffer[1];
};

//Receive at least one line into the buffer. Return the total size of received data.
static int recv_line(int socket, char *buffer, int buffer_size)
{
	int buffer_done = 0;
	while (buffer_done < buffer_size)
	{
		int done = recv(socket, buffer, buffer_size - buffer_done, 0);
		if (done <= 0)
			return 0;
		
		buffer_done += done;
		char *p = strnstr(buffer, "\r\n", buffer_done);
		if (p)
			return buffer_done;
	}
	
	return 0;
}

//Read next line using the buffer (multiple lines can be buffered at once).
//If the line was too long to fit into the buffer, returned length will be negative, but the next line will still get found correctly.
static char *recv_next_line_buffered(int socket, char *buffer, int buffer_size, int *buffer_used, int *offset, int *len)
{
	int skipped_len = 0;
	if (*offset > *buffer_used)
		return NULL;
	
	for (;;)
	{
		char *start = buffer + *offset;
		char *limit = buffer + *buffer_used;
		char *p = strnstr(start, "\r\n", limit - start);
		if (p)
		{
			*p = 0;
			*offset = p + 2 - buffer;
			if (skipped_len)
				*len = -(p - start + skipped_len);
			else
				*len = p - start;
			return start;
		}
		
		if (*offset == 0 && buffer_size == *buffer_used)
		{
			/* The length of this line exceeds the entire buffer.
			 * Discard the buffer contents and continue searching for the end-of-line.*/

			buffer[0] = buffer[buffer_size - 1];
			*buffer_used = (buffer[0] == '\r') ? 1 : 0;
			skipped_len = buffer_size - *buffer_used;
			*offset = 0;	
		}
		else if (start < limit && start > buffer)
		{
			memmove(buffer, start, limit - start);
			*buffer_used -= *offset;
			*offset = 0;
		}
		
		int done = recv(socket, buffer + *buffer_used, buffer_size - *buffer_used, 0);
		if (done <= 0)
			return NULL;
		
		*buffer_used += done;		
	}
}

static bool host_name_matches(http_connection ctx, char *host)
{
	int len = strlen(ctx->server->hostname);
	if (strncasecmp(host, ctx->server->hostname, len))
		return false;
	
	if (!host[len])
		return true;	//Host name without domain
	
	if (host[len] == '.' && !strcasecmp(host + len + 1, ctx->server->domain_name))
		return true;	//Host name with domain
	
	return false;
}

static bool send_all(int socket, const char *buf, int size)
{
	while (size > 0)
	{
		int done = send(socket, buf, size, 0);
		if (done <= 0)
			return false;
		
		buf += done;
		size -= done;
	}
	
	return true;
}

static inline void append(char *buf, int *offset, const char *data, int len)
{
	memcpy(buf + *offset, data, len);
	*offset += len;
}

static void parse_and_handle_http_request(http_connection ctx)
{
	int len = recv_line(ctx->socket, ctx->buffer, ctx->server->buffer_size);
	char *path = NULL;
	char *header_buf = NULL;
	int header_buf_size = 0, header_buf_pos = 0, header_buf_used = 0;
	char host[32];
	host[0] = 0;
	
	if (len)
	{
		//Expected request format: GET <path> HTTP/1.0
		char *p1 = strchr(ctx->buffer, ' '), *p2 = NULL, *p3 = NULL;
		if (p1)
			p2 = strchr(++p1, ' ');
		
		if (p2)
			p3 = strstr(p2, "\r\n");
		
		if (p3)
		{
			path = p1;
			*p2 = 0;
			
			int off = p3 + 2 - ctx->buffer;
			header_buf = ctx->buffer + off;
			header_buf_size = ctx->server->buffer_size - off;
			header_buf_used = len - off;
		}
	}
	
	if (!header_buf || header_buf_size < 32)
	{
		debug_printf("HTTP: invalid first line");
		return;
	}
	
	for (;;)
	{
		char *line = recv_next_line_buffered(ctx->socket, header_buf, header_buf_size, &header_buf_used, &header_buf_pos, &len);
		if (!line)
		{
			debug_printf("HTTP: unexpected end of headers");
			return;
		}
		
		if (!line[0])
			break;	//Proper end of headers
		
		if (len > 0 && !strncasecmp(line, "Host: ", 6) && (len - 6) < (sizeof(host) - 1))
		{
			memcpy(host, line + 6, len - 6);
			host[len - 6] = 0;
		}
	}
	
	debug_printf("HTTP: %s%s\n", host, path);
	
	if (!host_name_matches(ctx, host))
	{
		static const char header[] = "HTTP/1.0 302 Found\r\nLocation: http://";
		static const char footer[] = "\r\nConnection: Close\r\n\r\n";
		int host_len = strlen(ctx->server->hostname), domain_len = strlen(ctx->server->domain_name);
		
		int len = sizeof(header) + sizeof(footer) + host_len + domain_len + 2;
		if (len < ctx->server->buffer_size)
		{
			int off = 0;
			append(ctx->buffer, &off, header, sizeof(header) - 1);
			append(ctx->buffer, &off, ctx->server->hostname, host_len);
			if (domain_len)
			{
				append(ctx->buffer, &off, ".", 1);
				append(ctx->buffer, &off, ctx->server->domain_name, domain_len);
			}
			append(ctx->buffer, &off, footer, sizeof(footer) - 1);
			send_all(ctx->socket, ctx->buffer, off);
		}
	}
	else
	{
		for (http_zone *zone = ctx->server->first_zone; zone; zone = zone->next)
		{
			if (strncasecmp(path, zone->prefix, zone->prefix_len))
				continue;
			
			int off = zone->prefix_len;
			if (path[off] == 0 || path[off] == '/')
			{
				while (path[off] == '/')
					off++;
				
				if (zone->handler(ctx, path + off, zone->context))
					return;
			}
		}
		
		http_server_send_reply(ctx, "404 Not Found", "text/plain", "File not found", -1);
	}
}

static void do_handle_connection(void *arg)
{
	http_connection ctx = (http_connection)arg;
	parse_and_handle_http_request(ctx);
	closesocket(ctx->socket);
	vPortFree(ctx);
	xSemaphoreGive(ctx->server->semaphore);
	vTaskDelete(NULL);
}

static void http_server_thread(void *arg)
{
	http_server_instance sctx = (http_server_instance)arg;
	
	while (true)
	{
		struct sockaddr_storage remote_addr;
		socklen_t len = sizeof(remote_addr);
		int conn_sock = accept(sctx->socket, (struct sockaddr *)&remote_addr, &len);
		if (conn_sock >= 0)
		{
			http_connection cctx = pvPortMalloc(sizeof(struct _http_connection) + sctx->buffer_size);
			if (cctx)
			{
				cctx->server = sctx;
				cctx->socket = conn_sock;
				TaskHandle_t task;
				xSemaphoreTake(sctx->semaphore, portMAX_DELAY);
				if (xTaskCreate(do_handle_connection, "HTTP Connection", configMINIMAL_STACK_SIZE, cctx, tskIDLE_PRIORITY + 2, &task) != pdTRUE)
				{
					vPortFree(cctx);
					xSemaphoreGive(sctx->semaphore);
					cctx = NULL;
				}
			}
			
			if (!cctx)
				closesocket(conn_sock);
		}
	}
}

http_server_instance http_server_create(const char *main_host, const char *main_domain, int max_thread_count, int buffer_size)
{
	int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	struct sockaddr_in listen_addr =
	{
		.sin_len = sizeof(struct sockaddr_in),
		.sin_family = AF_INET,
		.sin_port = htons(80),
		.sin_addr = 0,
	};
    
	if (server_sock < 0)
	{
		debug_printf("Unable to create HTTP socket: error %d", errno);
		return NULL;
	}

	if (bind(server_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0)
	{
		closesocket(server_sock);
		debug_printf("Unable to bind HTTP socket: error %d\n", errno);
		return NULL;
	}

	if (listen(server_sock, max_thread_count * 2) < 0)
	{
		closesocket(server_sock);
		debug_printf("Unable to listen on HTTP socket: error %d\n", errno);
		return NULL;
	}
	
	http_server_instance ctx = (http_server_instance)pvPortMalloc(sizeof(struct _http_server_instance));
	if (!ctx)
	{
		closesocket(server_sock);
		return NULL;
	}

	ctx->socket = server_sock;
	ctx->semaphore = xSemaphoreCreateCounting(max_thread_count, max_thread_count);
	ctx->hostname = main_host;
	ctx->domain_name = main_domain;
	ctx->buffer_size = buffer_size;
	
	TaskHandle_t task;
	xTaskCreate(http_server_thread, "HTTP Server", configMINIMAL_STACK_SIZE, ctx, tskIDLE_PRIORITY + 2, &task);
	return ctx;
}

void http_server_add_zone(http_server_instance server, http_zone *zone, const char *prefix, http_request_handler handler, void *context)
{
	zone->next = server->first_zone;
	zone->prefix = prefix;
	zone->prefix_len = strlen(prefix);
	zone->handler = handler;
	zone->context = context;
	server->first_zone = zone;
}

void http_server_send_reply(http_connection conn, const char *code, const char *contentType, const char *content, int size)
{
	if (size < 0)
		size = strlen(content);
	
	int done = snprintf(conn->buffer, conn->server->buffer_size, "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", code, contentType, size);
	send_all(conn->socket, conn->buffer, done);
	send_all(conn->socket, content, size);
}
