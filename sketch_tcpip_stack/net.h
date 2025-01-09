#pragma once

struct iface;
struct driver {
  bool (*init)(struct iface *);                         // Init driver
  size_t (*tx)(const void *, size_t, struct iface *);   // Transmit frame
  size_t (*rx)(void *buf, size_t len, struct iface *);  // Receive frame
  bool (*up)(struct iface *);                           // Up/down status
};

struct spi {
  void *spi;                        // Opaque SPI bus descriptor
  void (*begin)(void *);            // SPI begin: slave select low
  void (*end)(void *);              // SPI end: slave select high
  uint8_t (*txn)(void *, uint8_t);  // SPI transaction: write 1 byte, read reply
};

struct iface {
  uint8_t mac_address[6];
  uint8_t ip_address[4]; 
  uint8_t gw_address[4];
  uint8_t net_mask[4];
  uint8_t gw_mac_address[6];
  struct driver *driver;
  void *driver_data;

  void (*on_udp)(struct iface *, uint8_t ip[4], uint16_t src_port, uint16_t dst_port, uint8_t *buf, size_t len);
};

#pragma pack(push, 1)
struct eth {
  uint8_t dst[6];  // Destination MAC address
  uint8_t src[6];  // Source MAC address
  uint16_t type;   // Ethernet type
};

struct ip {
  uint8_t ver;    // Version
  uint8_t tos;    // Unused
  uint16_t len;   // Length
  uint16_t id;    // Unused
  uint16_t frag;  // Fragmentation
#define IP_FRAG_OFFSET_MSK 0x1fff
#define IP_MORE_FRAGS_MSK 0x2000
  uint8_t ttl;    // Time to live
  uint8_t proto;  // Upper level protocol
  uint16_t csum;  // Checksum
  uint32_t src;   // Source IP
  uint32_t dst;   // Destination IP
};

struct icmp {
  uint8_t type;
  uint8_t code;
  uint16_t csum;
};

struct udp {
  uint16_t sport;  // Source port
  uint16_t dport;  // Destination port
  uint16_t len;    // UDP length
  uint16_t csum;   // UDP checksum
};

struct arp {
  uint16_t fmt;    // Format of hardware address
  uint16_t pro;    // Format of protocol address
  uint8_t hlen;    // Length of hardware address
  uint8_t plen;    // Length of protocol address
  uint16_t op;     // Operation
  uint8_t sha[6];  // Sender hardware address
  uint32_t spa;    // Sender protocol address
  uint8_t tha[6];  // Target hardware address
  uint32_t tpa;    // Target protocol address
};
#pragma pack(pop)

#define P8(x, i) (((uint8_t *) (x))[i] & 0xff)
#define P16(x) (((uint16_t) P8(x, 0)) << 8 | P8(x, 1))
#define SWAP16(x) __builtin_bswap16(x)

extern struct driver driver_w5500;
extern void net_poll(struct iface *);