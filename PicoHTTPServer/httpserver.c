#include <stdarg.h>
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
	size_t buffered_size;
	struct
	{
		int buffer_used, buffer_pos;
		int remaining_input_len;
		int offset_from_main_buffer;
	} post;
	
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
static char *recv_next_line_buffered(int socket, char *buffer, int buffer_size, int *buffer_used, int *offset, int *len, int *recv_limit)
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
		
		int buffer_avail = buffer_size - *buffer_used;
		if (recv_limit)
			buffer_avail = MIN(buffer_avail, *recv_limit);
		
		if (buffer_avail <= 0)
			return NULL;
		
		int done = recv(socket, buffer + *buffer_used, buffer_avail, 0);
		if (done <= 0)
			return NULL;
		
		if (recv_limit)
			*recv_limit -= done;
		
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
#if MEM_SIZE < 16384
		/*	As of SDK 1.4.0, lwIP running out of memory to allocate a network buffer on TCP send
		 *	permanently stalls the entire netconn. As it doesn't free up the resources taken by
		 *	that netconn, the effect quickly snowballs, rendering the entire network stack unusable:
		 *	
		 *	1. tcp_pbuf_prealloc() called by tcp_write() returns a NULL.
		 *	2. tcp_write() returns ERR_MEM
		 *	3. lwip_netconn_do_write() receives ERR_MEM and assumes that it needs to wait for
		 *	   the remote side to acknowledge the receipt. So it begins waiting on the netconn semaphore:
		 *		sys_arch_sem_wait(LWIP_API_MSG_SEM(msg), 0)
		 *  4. As we did not send out any meaningful data, the acknowledgement (normally done in tcp_receive())
		 *     never happens, and the lwip_send() never returns.
		 *     
		 *  You can easily detect this condition by checking lwip_stats.tcp.memerr. If the value is not 0, 
		 *  the IP stack has run out of memory at some point and might get stuck as described before.
		 *  
		 *  Increasing MEM_SIZE generally solves this issue, although it might return if multiple threads
		 *  attempt to send large amounts of data simultaneously.
		 **/
#error Too little memory allocated for lwIP buffers.
#endif
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
	enum http_request_type reqtype = HTTP_GET;
	ctx->post.remaining_input_len = ctx->post.buffer_used = ctx->post.buffer_pos = 0;
	
	if (len)
	{
		//Expected request format: GET <path> HTTP/1.0
		char *p1 = strchr(ctx->buffer, ' '), *p2 = NULL, *p3 = NULL;
		if (p1)
			p2 = strchr(++p1, ' ');
		
		if (!strncasecmp(ctx->buffer, "POST ", 5))
			reqtype = HTTP_POST;
		
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
		char *line = recv_next_line_buffered(ctx->socket, header_buf, header_buf_size, &header_buf_used, &header_buf_pos, &len, NULL);
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
		else if (!strncasecmp(line, "Content-length: ", 16))
		{
			ctx->post.remaining_input_len = atoi(line + 16);
		}
	}
	
	if (reqtype == HTTP_POST && ctx->post.remaining_input_len)
	{
		ctx->post.buffer_pos = header_buf_pos;
		ctx->post.buffer_used = header_buf_used;
		ctx->post.remaining_input_len -= (header_buf_used - header_buf_pos);
		ctx->post.offset_from_main_buffer = header_buf - ctx->buffer;
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
				
				if (zone->handler(ctx, reqtype, path + off, zone->context))
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
	ctx->first_zone = NULL;
	
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

http_write_handle http_server_begin_write_reply(http_connection conn, const char *code, const char *contentType)
{
	conn->buffered_size = snprintf(conn->buffer, conn->server->buffer_size, "HTTP/1.0 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", code, contentType);
	return (http_write_handle)conn;
}

void http_server_write_reply(http_write_handle handle, const char *format, ...)
{
	http_connection conn = (http_connection)handle;
	va_list args;
	va_start(args, format);
	int written = vsnprintf(conn->buffer + conn->buffered_size, conn->server->buffer_size - conn->buffered_size, format, args);
	va_end(args);
	if ((conn->buffered_size + written) < (conn->server->buffer_size - 16))
	{
		conn->buffered_size += written;
		return;
	}
	
	send_all(conn->socket, conn->buffer, conn->buffered_size);
	va_start(args, format);
	conn->buffered_size = vsnprintf(conn->buffer, conn->server->buffer_size, format, args);
	va_end(args);
}

void http_server_end_write_reply(http_write_handle handle, const char *footer)
{
	http_connection conn = (http_connection)handle;
	int len = footer ? strlen(footer) : 0;
	if (len && len < (conn->server->buffer_size - conn->buffered_size))
	{
		memcpy(conn->buffer + conn->buffered_size, footer, len);
		conn->buffered_size += len;
		len = 0;
	}
	
	if (conn->buffered_size)
		send_all(conn->socket, conn->buffer, conn->buffered_size);
	
	if (len)
		send_all(conn->socket, footer, len);
	
	conn->buffered_size = 0;
}

char *http_server_read_post_line(http_connection conn)
{
	if (conn->post.remaining_input_len <= 0 && conn->post.buffer_pos >= conn->post.buffer_used)
		return NULL;
	
	int len = 0;
	char *result = recv_next_line_buffered(conn->socket, 
		conn->buffer + conn->post.offset_from_main_buffer,
		conn->server->buffer_size - conn->post.offset_from_main_buffer,
		&conn->post.buffer_used,
		&conn->post.buffer_pos, 
		&len,
		&conn->post.remaining_input_len);
	
	if (!result)
		return NULL;
	
	if (len < 0)
		return NULL;	//Too long line got truncated
	
	result[len] = 0;
	
	return result;
}
