#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
#include "debug_printf.h"

const int kConnectionThreadCount = 3;
static xSemaphoreHandle s_ConnectionSemaphore;

typedef struct
{
	int socket;
	char buffer[4096];
} http_request_context;

static void do_handle_http_request(http_request_context *ctx)
{
	int buffer_done = 0;
	for (;;)
	{
		int done = recv(ctx->socket, ctx->buffer, sizeof(ctx->buffer) - buffer_done, 0);
		if (done <= 0)
			return;
		
		buffer_done += done;
		char *p = strnstr(ctx->buffer, "\r\n\r\n", buffer_done);
		if (p)
		{
			*p = 0;
			debug_printf("%s\n", ctx->buffer);
			char *space = strchr(ctx->buffer, ' ');
			if (!space)
				return;
			
			break;
		}
	}
	
	static const char Found[] = "HTTP/1.0 302 Found\r\nLocation: http://picohttp/\r\nConnection: Close\r\n\r\n";
	static const char HelloWorld[] = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: Close\r\n\r\n<html><body><h1>Hello, World</h1>This message is shown to you by the lwIP example project.</body></html>";
	static const char Captive[] = "HTTP/1.0 200 OK\r\nContent-Type: application/captive+json\r\n\r\n{\n    \"captive\": false,\n    \"user-portal-url\": \"https://picohttp/portal.html\",\n    \"venue-info-url\": \"https://picohttp/entertainment\",\n    \"seconds-remaining\": 326,\n    \"can-extend-session\": true\n}";

	if (strstr(ctx->buffer, "/captive-portal"))
	{
		int done = send(ctx->socket, Captive, sizeof(Captive) - 1, 0);
		if (done != sizeof(Captive) - 1)
		{
			asm("bkpt 255");
		}
	}
	else if (strstr(ctx->buffer, "picohttp"))
	{
		int done = send(ctx->socket, HelloWorld, sizeof(HelloWorld) - 1, 0);
		if (done != sizeof(HelloWorld) - 1)
		{
			asm("bkpt 255");
		}
	}
	else
	{
		int done = send(ctx->socket, Found, sizeof(Found) - 1, 0);
		if (done != sizeof(Found) - 1)
		{
			asm("bkpt 255");
		}
	}
}

static void do_handle_connection(void *arg)
{
	http_request_context *ctx = (http_request_context *)arg;
	do_handle_http_request(ctx);
	closesocket(ctx->socket);
	vPortFree(ctx);
	xSemaphoreGive(s_ConnectionSemaphore);
	vTaskDelete(NULL);
}

static void http_server_thread(void *arg)
{
	int server_sock = (int)arg;
	
	while (true)
	{
		struct sockaddr_storage remote_addr;
		socklen_t len = sizeof(remote_addr);
		int conn_sock = accept(server_sock, (struct sockaddr *)&remote_addr, &len);
		if (conn_sock >= 0)
		{
			http_request_context *ctx = pvPortMalloc(sizeof(http_request_context));
			if (ctx)
			{
				ctx->socket = conn_sock;
				TaskHandle_t task;
				xSemaphoreTake(s_ConnectionSemaphore, portMAX_DELAY);
				if (xTaskCreate(do_handle_connection, "HTTP Connection", configMINIMAL_STACK_SIZE, ctx, tskIDLE_PRIORITY + 2, &task) != pdTRUE)
				{
					vPortFree(ctx);
					xSemaphoreGive(s_ConnectionSemaphore);
					ctx = NULL;
				}
			}
			
			if (!ctx)
				closesocket(conn_sock);
		}
	}
}

void http_server_init(ip4_addr_t *addr)
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
		return;
	}

	if (bind(server_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0)
	{
		debug_printf("Unable to bind HTTP socket: error %d\n", errno);
		return;
	}

	if (listen(server_sock, kConnectionThreadCount * 2) < 0)
	{
		debug_printf("Unable to listen on HTTP socket: error %d\n", errno);
		return;
	}

	s_ConnectionSemaphore = xSemaphoreCreateCounting(kConnectionThreadCount, kConnectionThreadCount);
	
	TaskHandle_t task;
	xSemaphoreTake(s_ConnectionSemaphore, portMAX_DELAY);
	xTaskCreate(http_server_thread, "HTTP Server", configMINIMAL_STACK_SIZE, (void *)server_sock, tskIDLE_PRIORITY + 2, &task);
}
