#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include "log.h"
#include "ppp.h"
#include "events.h"
#include "mempool.h"
#include "ipdb.h"

#include "memdebug.h"

#define MAX_DNS_COUNT 3

static int conf_init_ra = 3;
static int conf_init_ra_interval = 1;
static int conf_ra_interval = 60;
static int conf_router_lifetime = 300;
static int conf_rdnss_lifetime = 300;
static struct in6_addr conf_dns[MAX_DNS_COUNT];
static int conf_dns_count;
static void *conf_dnssl;
static int conf_dnssl_size;
static int conf_managed;

#undef ND_OPT_ROUTE_INFORMATION
#define  ND_OPT_ROUTE_INFORMATION	24
struct nd_opt_route_info_local     /* route information */
{
	uint8_t   nd_opt_ri_type;
	uint8_t   nd_opt_ri_len;
	uint8_t   nd_opt_ri_prefix_len;
	uint8_t   nd_opt_ri_flags_reserved;
	uint32_t  nd_opt_ri_lifetime;
	struct in6_addr  nd_opt_ri_prefix;
};

#undef ND_OPT_RDNSS_INFORMATION
#define  ND_OPT_RDNSS_INFORMATION	25
struct nd_opt_rdnss_info_local
{
	uint8_t  nd_opt_rdnssi_type;
	uint8_t  nd_opt_rdnssi_len;
	uint16_t nd_opt_rdnssi_pref_flag_reserved;
	uint32_t nd_opt_rdnssi_lifetime;
	struct in6_addr  nd_opt_rdnssi[0];
};

#undef ND_OPT_DNSSL_INFORMATION
#define  ND_OPT_DNSSL_INFORMATION	31
struct nd_opt_dnssl_info_local
{
	uint8_t  nd_opt_dnssli_type;
	uint8_t  nd_opt_dnssli_len;
	uint16_t nd_opt_dnssli_pref_flag_reserved;
	uint32_t nd_opt_dnssli_lifetime;
	uint8_t  nd_opt_dnssli[0];
};

struct ipv6_nd_handler_t
{
	struct ppp_t *ppp;
	struct ppp_pd_t pd;
	struct triton_md_handler_t hnd;
	struct triton_timer_t timer;
	int ra_sent;
};

static void *pd_key;

#define BUF_SIZE 1024
static mempool_t buf_pool;

static void ipv6_nd_send_ra(struct ipv6_nd_handler_t *h, struct sockaddr_in6 *addr)
{
	void *buf = mempool_alloc(buf_pool), *endptr;
	struct nd_router_advert *adv = buf;
	struct nd_opt_prefix_info *pinfo;
	//struct nd_opt_route_info_local *rinfo;
	struct nd_opt_rdnss_info_local *rdnssinfo;
	struct in6_addr *rdnss_addr;
	struct nd_opt_dnssl_info_local *dnsslinfo;
	//struct nd_opt_mtu *mtu;
	struct ipv6db_addr_t *a;
	int i;
	
	if (!buf) {
		log_emerg("out of memory\n");
		return;
	}

	memset(adv, 0, sizeof(*adv));
	adv->nd_ra_type = ND_ROUTER_ADVERT;
	adv->nd_ra_curhoplimit = 64;
	adv->nd_ra_router_lifetime = htons(conf_router_lifetime);
	adv->nd_ra_flags_reserved = 
		conf_managed ? ND_RA_FLAG_MANAGED | ND_RA_FLAG_OTHER : 0;
	//adv->nd_ra_reachable = 0;
	//adv->nd_ra_retransmit = 0;
	
	pinfo = (struct nd_opt_prefix_info *)(adv + 1);
	list_for_each_entry(a, &h->ppp->ipv6->addr_list, entry) {
		if (a->prefix_len > 64)
			continue;
			
		memset(pinfo, 0, sizeof(*pinfo));
		pinfo->nd_opt_pi_type = ND_OPT_PREFIX_INFORMATION;
		pinfo->nd_opt_pi_len = 4;
		pinfo->nd_opt_pi_prefix_len = a->prefix_len;
		pinfo->nd_opt_pi_flags_reserved = ND_OPT_PI_FLAG_ONLINK | ((a->flag_auto || !conf_managed) ? ND_OPT_PI_FLAG_AUTO : 0);
		pinfo->nd_opt_pi_valid_time = 0xffffffff;
		pinfo->nd_opt_pi_preferred_time = 0xffffffff;
		memcpy(&pinfo->nd_opt_pi_prefix, &a->addr, 8);
		pinfo++;
	}
	
	/*rinfo = (struct nd_opt_route_info_local *)pinfo;
	list_for_each_entry(a, &h->ppp->ipv6->route_list, entry) {
		memset(rinfo, 0, sizeof(*rinfo));
		rinfo->nd_opt_ri_type = ND_OPT_ROUTE_INFORMATION;
		rinfo->nd_opt_ri_len = 3;
		rinfo->nd_opt_ri_prefix_len = a->prefix_len;
		rinfo->nd_opt_ri_lifetime = 0xffffffff;
		memcpy(&rinfo->nd_opt_ri_prefix, &a->addr, 8);
		rinfo++;
	}*/

	if (conf_dns_count) {
		rdnssinfo = (struct nd_opt_rdnss_info_local *)pinfo;
		memset(rdnssinfo, 0, sizeof(*rdnssinfo));
		rdnssinfo->nd_opt_rdnssi_type = ND_OPT_RDNSS_INFORMATION;
		rdnssinfo->nd_opt_rdnssi_len = 1 + 2 * conf_dns_count;
		rdnssinfo->nd_opt_rdnssi_lifetime = htonl(conf_rdnss_lifetime);
		rdnss_addr = (struct in6_addr *)rdnssinfo->nd_opt_rdnssi;
		for (i = 0; i < conf_dns_count; i++) {
			memcpy(rdnss_addr, &conf_dns[i], sizeof(*rdnss_addr));
			rdnss_addr++;
		}
	} else
		rdnss_addr = (struct in6_addr *)pinfo;
	
	if (conf_dnssl) {
		dnsslinfo = (struct nd_opt_dnssl_info_local *)rdnss_addr;
		memset(dnsslinfo, 0, sizeof(*dnsslinfo));
		dnsslinfo->nd_opt_dnssli_type = ND_OPT_DNSSL_INFORMATION;
		dnsslinfo->nd_opt_dnssli_len = 1 + (conf_dnssl_size - 1) / 8 + 1;
		dnsslinfo->nd_opt_dnssli_lifetime = htonl(conf_rdnss_lifetime);
		memcpy(dnsslinfo->nd_opt_dnssli, conf_dnssl, conf_dnssl_size);
		memset(dnsslinfo->nd_opt_dnssli + conf_dnssl_size, 0, (dnsslinfo->nd_opt_dnssli_len - 1) * 8 - conf_dnssl_size);
		endptr = (void *)dnsslinfo + dnsslinfo->nd_opt_dnssli_len * 8;
	} else
		endptr = rdnss_addr;

	sendto(h->hnd.fd, buf, endptr - buf, 0, (struct sockaddr *)addr, sizeof(*addr));

	mempool_free(buf);
}

static void send_ra_timer(struct triton_timer_t *t)
{
	struct ipv6_nd_handler_t *h = container_of(t, typeof(*h), timer);
	struct sockaddr_in6 addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr.s6_addr32[0] = htonl(0xff020000);
	addr.sin6_addr.s6_addr32[3] = htonl(0x1);
	addr.sin6_scope_id = h->ppp->ifindex; 

	if (h->ra_sent++ == conf_init_ra) {
		h->timer.period = conf_ra_interval * 1000;
		triton_timer_mod(t, 0);
	}

	ipv6_nd_send_ra(h, &addr);
}

static int ipv6_nd_read(struct triton_md_handler_t *_h)
{
	struct ipv6_nd_handler_t *h = container_of(_h, typeof(*h), hnd);
	struct icmp6_hdr *icmph = mempool_alloc(buf_pool);
	int n;
	struct sockaddr_in6 addr;
	socklen_t addr_len = sizeof(addr);

	if (!icmph) {
		log_emerg("out of memory\n");
		return 0;
	}

	while (1) {
		n = recvfrom(h->hnd.fd, icmph, BUF_SIZE, 0, &addr, &addr_len);
		if (n == -1) {
			if (errno == EAGAIN)
				break;
			log_ppp_error("ipv6_nd: recvmsg: %s\n", strerror(errno));
			continue;
		}

		if (n < sizeof(*icmph)) {
			log_ppp_warn("ipv6_nd: received short icmp packet (%i)\n", n);
			continue;
		}

		if (icmph->icmp6_type != ND_ROUTER_SOLICIT) {
			log_ppp_warn("ipv6_nd: received unexcpected icmp packet (%i)\n", icmph->icmp6_type);
			continue;
		}

		if (!IN6_IS_ADDR_LINKLOCAL(&addr.sin6_addr)) {
			log_ppp_warn("ipv6_nd: received icmp packet from non link-local address\n");
			continue;
		}

		/*if (*(uint64_t *)(addr.sin6_addr.s6_addr + 8) != *(uint64_t *)(h->ppp->ipv6_addr.s6_addr + 8)) {
			log_ppp_warn("ipv6_nd: received icmp packet from unknown address\n");
			continue;
		}*/

		ipv6_nd_send_ra(h, &addr);
	}

	mempool_free(icmph);

	return 0;
}

static int ipv6_nd_start(struct ppp_t *ppp)
{
	int sock;
	struct icmp6_filter filter;
	struct sockaddr_in6 addr;
	struct ipv6_mreq mreq;
	int val;
	struct ipv6_nd_handler_t *h;

	sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	
	if (sock < 0) {
		log_ppp_error("socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6): %s\n", strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr.s6_addr32[0] = htons(0xfe80);
	*(uint64_t *)(addr.sin6_addr.s6_addr + 8) = ppp->ipv6->intf_id;
	addr.sin6_scope_id = ppp->ifindex;

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		log_ppp_error("ipv6_nd: bind: %s %i\n", strerror(errno), errno);
		goto out_err;
	}

	val = 2;
	if (setsockopt(sock, IPPROTO_RAW, IPV6_CHECKSUM, &val, sizeof(val))) {
		log_ppp_error("ipv6_nd: setsockopt(IPV6_CHECKSUM): %s\n", strerror(errno));
		goto out_err;
	}

	val = 255;
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &val, sizeof(val))) {
		log_ppp_error("ipv6_nd: setsockopt(IPV6_UNICAST_HOPS): %s\n", strerror(errno));
		goto out_err;
	}
	
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &val, sizeof(val))) {
		log_ppp_error("ipv6_nd: setsockopt(IPV6_MULTICAST_HOPS): %s\n", strerror(errno));
		goto out_err;
	}

	/*val = 1;
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &val, sizeof(val))) {
		log_ppp_error("ipv6_nd: setsockopt(IPV6_HOPLIMIT): %s\n", strerror(errno));
		goto out_err;
	}*/

	ICMP6_FILTER_SETBLOCKALL(&filter);
	ICMP6_FILTER_SETPASS(ND_ROUTER_SOLICIT, &filter);

	if (setsockopt(sock, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter))) {
		log_ppp_error("ipv6_nd: setsockopt(ICMP6_FILTER): %s\n", strerror(errno));
		goto out_err;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.ipv6mr_interface = ppp->ifindex;
	mreq.ipv6mr_multiaddr.s6_addr32[0] = htonl(0xff020000);
	mreq.ipv6mr_multiaddr.s6_addr32[3] = htonl(0x2);

	if (setsockopt(sock, SOL_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
		log_ppp_error("ipv6_nd: failed to join ipv6 allrouters\n");
		goto out_err;
	}

	fcntl(sock, F_SETFL, O_NONBLOCK);

	h = _malloc(sizeof(*h));
	memset(h, 0, sizeof(*h));
	h->ppp = ppp;
	h->pd.key = &pd_key;
	h->hnd.fd = sock;
	h->hnd.read = ipv6_nd_read;
	h->timer.expire = send_ra_timer;
	h->timer.period = conf_init_ra_interval * 1000;
	list_add_tail(&h->pd.entry, &ppp->pd_list);

	triton_md_register_handler(ppp->ctrl->ctx, &h->hnd);
	triton_md_enable_handler(&h->hnd, MD_MODE_READ);

	triton_timer_add(ppp->ctrl->ctx, &h->timer, 0);

	return 0;

out_err:
	close(sock);
	return -1;
}

static struct ipv6_nd_handler_t *find_pd(struct ppp_t *ppp)
{
	struct ppp_pd_t *pd;

	list_for_each_entry(pd, &ppp->pd_list, entry) {
		if (pd->key == &pd_key)
			return container_of(pd, typeof(struct ipv6_nd_handler_t), pd);
	}

	return NULL;
}

static void ev_ppp_started(struct ppp_t *ppp)
{
	if (!ppp->ipv6)
		return;

	ipv6_nd_start(ppp);
}

static void ev_ppp_finishing(struct ppp_t *ppp)
{
	struct ipv6_nd_handler_t *h = find_pd(ppp);

	if (!h)
		return;
	
	if (h->timer.tpd)
		triton_timer_del(&h->timer);

	triton_md_unregister_handler(&h->hnd);
	close(h->hnd.fd);

	list_del(&h->pd.entry);
	
	_free(h);
}

static void add_dnssl(const char *val)
{
	int n = strlen(val);
	const char *ptr;
	uint8_t *buf;

	if (val[n - 1] == '.')
		n++;
	else
		n += 2;
	
	if (n > 255) {
		log_error("dnsv6: dnssl '%s' is too long\n", val);
		return;
	}
	
	if (!conf_dnssl)
		conf_dnssl = _malloc(n);
	else
		conf_dnssl = _realloc(conf_dnssl, conf_dnssl_size + n);
	
	buf = conf_dnssl + conf_dnssl_size;
	
	while (1) {
		ptr = strchr(val, '.');
		if (!ptr)
			ptr = strchr(val, 0);
		if (ptr - val > 63) {
			log_error("dnsv6: dnssl '%s' is invalid\n", val);
			return;
		}
		*buf = ptr - val;
		memcpy(buf + 1, val, ptr - val);
		buf += 1 + (ptr - val);
		val = ptr + 1;
		if (!*ptr || !*val) {
				*buf = 0;
				break;
		}
	}
	
	conf_dnssl_size += n;
}

static void load_config(void)
{
	struct conf_sect_t *s = conf_get_section("dnsv6");
	struct conf_option_t *opt;
	
	if (!s)
		return;
	
	conf_dns_count = 0;
	
	if (conf_dnssl)
		_free(conf_dnssl);
	conf_dnssl = NULL;
	conf_dnssl_size = 0;

	list_for_each_entry(opt, &s->items, entry) {
		if (!strcmp(opt->name, "dnssl")) {
			add_dnssl(opt->val);
			continue;
		}

		if (!strcmp(opt->name, "dns") || !opt->val) {
			if (conf_dns_count == MAX_DNS_COUNT)
				continue;

			if (inet_pton(AF_INET6, opt->val ? opt->val : opt->name, &conf_dns[conf_dns_count]) == 0) {
				log_error("dnsv6: faild to parse '%s'\n", opt->name);
				continue;
			}
			conf_dns_count++;
		}
	}
}

static void init(void)
{
	buf_pool = mempool_create(BUF_SIZE);

	load_config();
	
	conf_managed = triton_module_loaded("ipv6_dhcp");

	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
	triton_event_register_handler(EV_PPP_STARTED, (triton_event_func)ev_ppp_started);
	triton_event_register_handler(EV_PPP_FINISHING, (triton_event_func)ev_ppp_finishing);
}

DEFINE_INIT(5, init);
