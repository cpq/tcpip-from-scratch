#include "SPI.h"
#include "net.h"
#include "str.h"

#define SS_PIN 7  // Slave select pin for the W5500 module

struct spi spi = {
    NULL,  // SPI metadata
    [](void *) { digitalWrite(SS_PIN, LOW); SPI.beginTransaction(SPISettings()); },
    [](void *) { digitalWrite(SS_PIN, HIGH); SPI.endTransaction(); },
    [](void *, uint8_t c) { return SPI.transfer(c); }, // Execute transaction
};

static struct iface network_interface = {
  {2, 3, 4, 5, 6, 7},
  {192, 168, 3, 7},
  {192, 168, 3, 1},
  {255, 255, 255, 0},
  {0},
  &driver_w5500,
  &spi,
};

static void myputchar(char ch, void *x) { Serial.print(ch); }
#define log(fmt, ...) xprintf(myputchar, NULL, fmt, __VA_ARGS__);

static uint32_t csumup(uint32_t sum, const void *buf, size_t len) {
  size_t i;
  const uint8_t *p = (const uint8_t *) buf;
  for (i = 0; i < len; i++) sum += i & 1 ? p[i] : ((uint32_t) p[i]) << 8;
  return sum;
}

static uint16_t csumfin(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return SWAP16(~sum & 0xffff);
}

static uint16_t ipcsum(const void *buf, size_t len) {
  uint32_t sum = csumup(0, buf, len);
  return csumfin(sum);
}

void udp_send(struct iface *ifp, uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port, void *buf, size_t len) {
  uint8_t pkt[sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp) + len];
  struct eth *eth = (struct eth *) pkt;
  struct ip *ip = (struct ip *) (eth + 1);
  struct udp *udp = (struct udp *) (ip + 1);

  memset(pkt, 0, sizeof(pkt));

  memcpy(eth->src, ifp->mac_address, sizeof(eth->src));
  memcpy(eth->dst, ifp->gw_mac_address, sizeof(eth->dst));
  eth->type = SWAP16(0x800);

  ip->ver = 0x45;             // Version 4, header length 5 words
  ip->frag = SWAP16(0x4000);  // Don't fragment
  ip->len = SWAP16((uint16_t) (sizeof(*ip) + sizeof(*udp) + len));
  ip->ttl = 64;
  ip->proto = 17;
  ip->src = * (uint32_t *) ifp->ip_address;
  ip->dst = * (uint32_t *) dst_ip;
  ip->csum = ipcsum(ip, sizeof(*ip));

  udp->sport = SWAP16(src_port);
  udp->dport = SWAP16(dst_port);
  udp->len = SWAP16((uint16_t) (sizeof(*udp) + len));
  uint32_t cs = csumup(0, udp, sizeof(*udp));
  cs = csumup(cs, buf, len);
  cs = csumup(cs, &ip->src, sizeof(ip->src));
  cs = csumup(cs, &ip->dst, sizeof(ip->dst));
  cs += (uint32_t) (ip->proto + sizeof(*udp) + len);
  udp->csum = csumfin(cs);
  memmove(udp + 1, buf, len);

  ifp->driver->tx(pkt, sizeof(pkt), ifp);
}

void net_poll(struct iface *ifp) {
  if (!ifp->driver->up(ifp)) return;
  uint8_t buf[1500];
  size_t len;
  if ((len = ifp->driver->rx(buf, sizeof(buf), ifp)) > 0) {
    // xhexdump(myputchar, NULL, buf, len > 32 ? 32 : len);
    if (len < sizeof(struct eth)) return;
    struct eth *eth = (struct eth *) buf;
    uint16_t proto = SWAP16(eth->type);

    if (!(proto == 0x0806 || memcmp(eth->dst, ifp->mac_address, 6) == 0)) return;

    log("ETH: %u, %M -> %M proto %04hx\n", len, fmt_mac, &eth->src, fmt_mac, &eth->dst, proto);

    if (proto == 0x0806) { // ARP
      if (len < sizeof(struct eth) + sizeof(struct arp)) return;
      struct arp *arp = (struct arp *) (eth + 1);
      uint16_t op = SWAP16(arp->op);
      log("  ARP: op %hu, %M/%M -> %M/%M\n", op, fmt_ip4, &arp->tpa, fmt_mac, &arp->tha, fmt_ip4, &arp->spa, fmt_mac, &arp->sha);
      if (op == 1 && memcmp(&arp->tpa, ifp->ip_address, sizeof(ifp->ip_address)) == 0) {
        struct eth saved_eth = *eth;
        struct arp saved_arp = *arp;
        memcpy(eth->dst, saved_eth.src, sizeof(eth->dst));
        memcpy(eth->src, ifp->mac_address, sizeof(eth->src));
        arp->op = SWAP16(2);
        memcpy(&arp->tpa, &saved_arp.spa, sizeof(arp->tpa));
        memcpy(arp->tha, saved_arp.sha, sizeof(arp->tha));
        memcpy(&arp->spa, ifp->ip_address, sizeof(arp->spa));
        memcpy(arp->sha, ifp->mac_address, sizeof(arp->sha));
        ifp->driver->tx(eth, sizeof(*eth) + sizeof((*arp)), ifp);

        if (memcmp(&saved_arp.spa, &ifp->gw_address, sizeof(saved_arp.spa)) == 0) {
          log("Saving GW MAC: %M\n", fmt_mac, saved_arp.sha);
          memcpy(&ifp->gw_mac_address, saved_arp.sha, sizeof(saved_arp.sha));
        }
      }
    }

    if (proto == 0x0800) { // IP protocol
      struct ip *ip = (struct ip *) (eth + 1);
      if (len < sizeof(struct eth) + sizeof(struct ip)) return;
      uint16_t ip_len = SWAP16(ip->len);
      if (sizeof(*eth) + ip_len > len) return;
      if (sizeof(*eth) + ip_len < len) len = sizeof(*eth) + ip_len;
      log("  IP: %M -> %M\n", fmt_ip4, &ip->src, fmt_ip4, &ip->dst);
      if (ip->proto == 0x0001) {
        struct icmp *icmp = (struct icmp *) (ip + 1);
        if (len < sizeof(struct eth) + sizeof(struct ip) + sizeof(struct icmp)) return;
        log("    ICMP type %hhu code %hhu\n", icmp->type, icmp->code);
        if (icmp->type == 8) {
          struct eth saved_eth = *eth;
          struct ip saved_ip = *ip;
          struct icmp saved_icmp = *icmp;
          size_t plen = len - (sizeof(struct eth) + sizeof(struct ip) + sizeof(struct icmp));
          memcpy(eth->dst, saved_eth.src, sizeof(eth->dst));
          memcpy(eth->src, ifp->mac_address, sizeof(eth->src));
          ip->csum = 0;
          ip->csum = ipcsum(ip, sizeof(*ip));
          icmp->type = 0;
          icmp->csum = 0;
          icmp->csum = ipcsum(icmp, sizeof(*icmp) + plen);
          ip->src = saved_ip.dst;
          ip->dst = saved_ip.src;
          ifp->driver->tx(eth, len, ifp);
        }
      }

      if (ip->proto == 17) {
        struct udp *udp = (struct udp *) (ip + 1);
        if (len < sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp)) return;
        size_t plen = len - (sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp));
        log("    UDP: %hu -> %hu %u %u %u %u\n", SWAP16(udp->sport), SWAP16(udp->dport), len, sizeof(struct eth), sizeof(struct ip), sizeof(struct udp));
        if (ifp->on_udp) ifp->on_udp(ifp, (uint8_t *) &ip->src, SWAP16(udp->sport), SWAP16(udp->dport), (uint8_t *) (udp + 1), plen);
      }
    }
  }
}

 static void on_udp(struct iface *ifp, uint8_t src_ip[4], uint16_t src_port, uint16_t dst_port, uint8_t *buf, size_t len) {
  log("Got UDP data from %M:%hu %lu\n", fmt_ip4, src_ip, src_port, len);
  xhexdump(myputchar, NULL, buf, len);
  char resp[20];
  size_t rlen = xsnprintf(resp, sizeof(resp), "Got %lu bytes\n", len);
  udp_send(ifp, src_ip, src_port, dst_port, resp, rlen);  // Echo received frame back
 }

void setup() {
  Serial.begin(115200);       // Initialise serial
  while (!Serial) delay(50);  // for debug output

  SPI.begin();               // Iniitialise SPI
  pinMode(SS_PIN, OUTPUT);   // to communicate with W5500 Ethernet module

  Serial.print("Initializing interface: ");
  bool ok = network_interface.driver->init(&network_interface);
  Serial.println(ok ? "success" : "failure");

  network_interface.on_udp = on_udp;
}

void loop() {
  net_poll(&network_interface);
}
