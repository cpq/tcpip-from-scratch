#include "SPI.h"
#include "str.h"
#include "tcpip.h"

#define SS_PIN 7

struct spi spi = {
    NULL,  // SPI metadata
    [](void *) { digitalWrite(SS_PIN, LOW); SPI.beginTransaction(SPISettings()); },
    [](void *) { digitalWrite(SS_PIN, HIGH); SPI.endTransaction(); },
    [](void *, uint8_t c) { return SPI.transfer(c); }, // Execute transaction
};

void myputchar(char ch, void *arg) { Serial.write(ch); }
#define log(fmt, ...) xprintf(myputchar, NULL, fmt, __VA_ARGS__)

struct iface iface = {
  &driver_w5500,
  &spi,
  {192, 168, 3, 7},
  { 2, 3, 4, 5, 6, 7},
};

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(50);

  SPI.begin();
  pinMode(SS_PIN, OUTPUT);

  bool ok = iface.driver->init(&iface);
  log("Driver init: %s\n", ok ? "success" : "failure");
}

void send_arp(struct iface *ifp, uint16_t op,
              uint8_t src_mac[6], uint8_t src_ip[4],
              uint8_t dst_mac[6], uint8_t dst_ip[4]) {
  uint8_t pkt[sizeof(struct eth) + sizeof(struct arp)];
  struct eth *eth = (struct eth *) pkt;
  struct arp *arp = (struct arp *) (eth + 1);

  memset(pkt, 0, sizeof(pkt));
  memcpy(eth->src, src_mac, 6);
  memcpy(eth->dst, dst_mac, 6);
  eth->type = NET16(0x806);

  arp->fmt = NET16(1);
  arp->pro = NET16(0x800);
  arp->hlen = 6;
  arp->plen = 4;
  arp->op = NET16(op);
  memcpy(arp->sha, src_mac, 6);
  memcpy(&arp->spa, src_ip, 4);
  memcpy(arp->tha, dst_mac, 6);
  memcpy(&arp->tpa, dst_ip, 4);

  ifp->driver->tx(pkt, sizeof(pkt), ifp);
}

void process_arp(struct iface *ifp, struct eth *eth, size_t len) {
  struct arp *arp = (struct arp *) (eth + 1);
  if (len < sizeof(*eth) + sizeof(*arp)) return;
  uint16_t op = NET16(arp->op);
  log("  ARP: %M/%M -> %M/%M type %hx\n",
    fmt_mac, arp->sha, fmt_ip4, &arp->spa,
    fmt_mac, arp->tha, fmt_ip4, &arp->tpa,
    op
  );
  if (op == 1 && memcmp(&arp->tpa, ifp->ip_address, 4) == 0) {
    send_arp(ifp, 2, ifp->mac_address, ifp->ip_address, arp->sha, (uint8_t *) &arp->tpa);
  }
}

static uint32_t csumup(uint32_t sum, const void *buf, size_t len) {
  size_t i;
  const uint8_t *p = (const uint8_t *) buf;
  for (i = 0; i < len; i++) sum += i & 1 ? p[i] : ((uint32_t) p[i]) << 8;
  return sum;
}

static uint16_t csumfin(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return NET16(~sum & 0xffff);
}

static uint16_t ipcsum(const void *buf, size_t len) {
  uint32_t sum = csumup(0, buf, len);
  return csumfin(sum);
}


void send_icmp(struct iface *ifp, uint8_t op, uint8_t dst_mac[6],
              uint8_t dst_ip[4], void *buf, size_t len) {
  uint8_t pkt[sizeof(struct eth) + sizeof(struct ip) + sizeof(struct icmp) + len];
  struct eth *eth = (struct eth *) pkt;
  struct ip *ip = (struct ip *) (eth + 1);
  struct icmp *icmp = (struct icmp *) (ip + 1);

  memset(pkt, 0, sizeof(pkt));
  memcpy(eth->src, ifp->mac_address, 6);
  memcpy(eth->dst, dst_mac, 6);
  eth->type = NET16(0x800);

  ip->ver = 0x45;
  ip->len = NET16(sizeof(*ip) + sizeof(*icmp) + len);
  ip->ttl = 64;
  ip->proto = 1;
  memcpy(&ip->src, ifp->ip_address, 4);
  memcpy(&ip->dst, dst_ip, 4);
  ip->csum = ipcsum(ip, sizeof(*ip));

  icmp->type = op;
  memcpy(icmp + 1, buf, len);
  icmp->csum = ipcsum(icmp, sizeof(*icmp) + len);

  ifp->driver->tx(pkt, sizeof(pkt), ifp);
}

void process_icmp(struct iface *ifp, struct eth *eth, struct ip *ip, size_t len) {
  struct icmp *icmp = (struct icmp *) (ip + 1);
  size_t headers_len = sizeof(*eth) + sizeof(*ip) + sizeof(*icmp);
  if (len < headers_len) return;
  log("    ICMP: type %hhu code %hhu payload %lu bytes\n", icmp->type, icmp->code, len - headers_len);
  if (icmp->type == 8) send_icmp(ifp, 0, eth->src, (uint8_t *) &ip->src, (uint8_t *) (icmp + 1), len - headers_len);
}

void process_ip(struct iface *ifp, struct eth *eth, size_t len) {
  struct ip *ip = (struct ip *) (eth + 1);
  if (len < sizeof(*eth) + sizeof(*ip)) return;
  uint16_t ip_len = NET16(ip->len);
  if (len > sizeof(*eth) + ip_len) len = sizeof(*eth) + ip_len;
  log("  IP: %M -> %M proto %hhu\n", fmt_ip4, &ip->src, fmt_ip4, &ip->dst, ip->proto);
  if (ip->proto == 1) process_icmp(ifp, eth, ip, len);
  // if (ip->proto == 17) process_udp(ifp, ip, len);
  // if (ip->proto == 6) process_tcp(ifp, ip, len);
}

void process_ethernet(struct iface *ifp) {
  uint8_t buf[1500];
  if (ifp->driver->up(ifp) == false) return;
  size_t len = ifp->driver->rx(buf, sizeof(buf), ifp);
  if (len > 0) {
    struct eth *eth = (struct eth *) buf;
    if (len < sizeof(*eth)) return;
    uint16_t type = NET16(eth->type);
    if (!(type == 0x806 || memcmp(eth->dst, ifp->mac_address, 6) == 0)) return;
    log("ETH: %M -> %M proto %hx\n", fmt_mac, eth->src, fmt_mac, eth->dst, type);

    if (type == 0x806) process_arp(ifp, eth, len);
    if (type == 0x800) process_ip(ifp, eth, len);
  }
}


void loop() {
  process_ethernet(&iface);
}
