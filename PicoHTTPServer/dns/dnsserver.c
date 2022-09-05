#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>

#include <FreeRTOS.h>
#include <task.h>
#include "../debug_printf.h"
#include "../server_settings.h"

static struct
{
	uint32_t primary_ip;
	uint32_t secondary_ip;
	const char *host_name; 
	const char *domain_name;
	bool ignore_network_suffix;
} s_DNSServerSettings;

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

static const char *get_encoded_domain_name_component(const uint8_t *buffer, size_t *offset, size_t max_input_size, int *component_len)
{
	for (int i = *offset; i < max_input_size;)
	{
		if (buffer[i] & 0xC0)
		{
			i = ((buffer[i] & 0x3F) << 8) | (buffer[i+1]);
			continue;
		}
		
		uint8_t len = buffer[i] & 0x3F;
		
		if ((i + len) >= max_input_size)
			return NULL;
		
		if (!len)
			return NULL;
		
		*offset = ++i + len;
		*component_len = len;
		return (const char *)buffer + i;
	}
	
	return NULL;
}

static uint32_t get_address_for_encoded_domain(const uint8_t *buffer, size_t offset, size_t buffer_size)
{
	debug_printf("DNS server: ");
	bool match = false, loose_match = false;
	
	int domain_off = 0, domain_len = 0;
	const char *domain_comp = get_next_domain_name_component(s_DNSServerSettings.domain_name, &domain_off, &domain_len);
	
	for (int i = 0; ; i++)
	{
		int len;
		const char *component = get_encoded_domain_name_component(buffer, &offset, buffer_size, &len);
		if (component)
		{
			if (i)
				debug_write(".", 1);
			debug_write(component, len);
			
			if (i == 0 && !strncasecmp(component, s_DNSServerSettings.host_name, len))
			{
				match = true;
				if (s_DNSServerSettings.ignore_network_suffix)
					loose_match = true;
			}
			else if (i > 0 && match && domain_comp && len == domain_len && !strncasecmp(component, domain_comp, len))
			{
				match = true;
				domain_comp = get_next_domain_name_component(s_DNSServerSettings.domain_name, &domain_off, &domain_len);
			}
			else
				match = false;
		}
		else
		{
			uint32_t ip = (match || loose_match) ? s_DNSServerSettings.primary_ip : s_DNSServerSettings.secondary_ip ;
			debug_printf(" -> %d.%d.%d.%d\n", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
			return ip;
		}
	}
}

#define DNS_QR_QUERY 0
#define DNS_QR_RESPONSE 1
#define DNS_OPCODE_QUERY 0

static void dns_server_thread(void *unused)
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
			packet.header.QR = DNS_QR_RESPONSE;
			packet.header.ANCount = ntohs(1);
			
			int ptr = done - sizeof(packet.header);
			packet.payload[ptr++] = 0xC0; //Domain name is a pointer
			packet.payload[ptr++] = sizeof(packet.header); //The pointer points to the original domain name
			
			struct IPResourceRecord rec = {
				.Type = htons(1), //A	
				.Class = htons(1), //IN
				.TTL = htonl(1), //1 second
				.DataLength = htons(4),
			};
			
			rec.Data = get_address_for_encoded_domain((char *)&packet.header, sizeof(packet.header), done);
			
			memcpy(packet.payload + ptr, &rec, sizeof(rec));
			ptr += sizeof(rec);
			sendto(server_sock, &packet, ptr + sizeof(packet.header), 0, (const struct sockaddr *)&fromaddr, addr_size);
		}
	}
}

void dns_server_init(uint32_t primary_ip,
	uint32_t secondary_ip,
	const char *host_name, 
	const char *domain_name,
	bool dns_ignores_network_suffix)
{
	s_DNSServerSettings.primary_ip = primary_ip;
	s_DNSServerSettings.secondary_ip = secondary_ip;
	s_DNSServerSettings.host_name = host_name;
	s_DNSServerSettings.domain_name = domain_name;
	s_DNSServerSettings.ignore_network_suffix = dns_ignores_network_suffix;
	
	TaskHandle_t task;
	xTaskCreate(dns_server_thread, "DNS server", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 2, &task);
}