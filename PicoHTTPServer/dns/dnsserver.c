#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>

#include <FreeRTOS.h>
#include <task.h>
#include "../debug_printf.h"

//DNS protocol definitions and parsing/formatting logic from https://github.com/devyte/ESPAsyncDNSServer/blob/master/src/ESPAsyncDNSServer.cpp
struct DNSHeader
{
	uint16_t ID; // identification number
	unsigned char RD : 1; // recursion desired
	unsigned char TC : 1; // truncated message
	unsigned char AA : 1; // authoritive answer
	unsigned char OPCode : 4; // message_type
	unsigned char QR : 1; // query/response flag
	unsigned char RCode : 4; // response code
	unsigned char Z : 3; // its z! reserved
	unsigned char RA : 1; // recursion available
	uint16_t QDCount; // number of question entries
	uint16_t ANCount; // number of answer entries
	uint16_t NSCount; // number of authority entries
	uint16_t ARCount; // number of resource entries
};


struct IPResourceRecord
{
	uint16_t Type;
	uint16_t Class;
	uint32_t TTL;
	uint16_t DataLength;
	uint32_t Data;
} __attribute__((packed));

//Translates a domain name from a DNS request into a human-readable null-terminated string.
static int translate_domain_name(uint8_t *name, size_t max_input_size, char *result, size_t result_size)
{
	if (!result || !result_size)
		return 0;
	
	size_t ptr = 0;
	for (int i = 0; i < max_input_size;)
	{
		if (name[i] & 0xC0)
			return 0;	//DNS pointers are not implemented
		uint8_t len = name[i] & 0x3F;
		
		if ((i + len) > max_input_size || (ptr + len) >= (result_size - 1))
			return 0;
		
		if (!len)
		{
			result[ptr] = 0;
			return ptr;
		}
		
		if (ptr)
			result[ptr++] = '.';
		
		for (; len > 0; len--)
			result[ptr++] = name[i++];
	}
	
	return 0;
}

#define DNS_QR_QUERY 0
#define DNS_QR_RESPONSE 1
#define DNS_OPCODE_QUERY 0

static void dns_server_thread(void *arg)
{
	int server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in listen_addr =
	{
		.sin_len = sizeof(struct sockaddr_in),
		.sin_family = AF_INET,
		.sin_port = htons(53),
		.sin_addr = 0,
	};
    
	if (server_sock < 0)
	{
		debug_printf("Unable to create DNS server socket: error %d", errno);
		return;
	}

	if (bind(server_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0)
	{
		debug_printf("Unable to bind DNS server socket: error %d\n", errno);
		return;
	}

	while (true)
	{
		static struct
		{
			struct DNSHeader header;
			char payload[1536 - sizeof(struct DNSHeader)];
		} packet;
		
		struct sockaddr_storage fromaddr;
		socklen_t addr_size = sizeof(fromaddr);
		int done = recvfrom(server_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&fromaddr, &addr_size);
		if (done >= sizeof(packet.header) && done < (sizeof(packet) - sizeof(struct IPResourceRecord) - 2) &&
			packet.header.QR == DNS_QR_QUERY &&
			packet.header.OPCode == DNS_OPCODE_QUERY &&
			packet.header.QDCount == htons(1) &&
			packet.header.ANCount == 0 &&
			packet.header.NSCount == 0 &&
			packet.header.ARCount == 0)
		{
			char domain[32];
			if (translate_domain_name((uint8_t *)packet.payload, done - sizeof(packet.header), domain, sizeof(domain)))
				debug_printf("DNS request for %s\n", domain);
			else
				debug_printf("Unsupported (but valid) DNS request\n");
			
			packet.header.QR = DNS_QR_RESPONSE;
			packet.header.ANCount = ntohs(1);
			
			int ptr = done - sizeof(packet.header);
			packet.payload[ptr++] = 0xC0; //Domain name is a pointer
			packet.payload[ptr++] = sizeof(packet.header); //The pointer points to the original domain name
			
			struct IPResourceRecord rec = {
				.Type = htons(1),
				//A	
				.Class = htons(1), //IN
				.TTL = htonl(1), //1 second
				.DataLength = htons(4),
				.Data = (uint32_t)arg
			};
			
			if (!strstr(domain, "picohttp"))
				rec.Data = 0x01010101;
			
			memcpy(packet.payload + ptr, &rec, sizeof(rec));
			ptr += sizeof(rec);
			sendto(server_sock, &packet, ptr + sizeof(packet.header), 0, (const struct sockaddr *)&fromaddr, addr_size);
		}
	}
}

void dns_server_init(ip4_addr_t *addr)
{
	TaskHandle_t task;
	xTaskCreate(dns_server_thread, "DNS server", configMINIMAL_STACK_SIZE, (void *)addr->addr, tskIDLE_PRIORITY + 2, &task);
}