#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <features.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include "linux_ppp.h"

#include "crypto.h"

#include "triton.h"

#include "ap_session.h"
#include "events.h"
#include "ppp.h"
#include "ppp_fsm.h"
#include "ipdb.h"
#include "log.h"
#include "spinlock.h"
#include "mempool.h"

#include "memdebug.h"

int __export conf_ppp_verbose;
int conf_unit_cache = 0;

static mempool_t buf_pool;

static LIST_HEAD(layers);

struct layer_node_t
{
	struct list_head entry;
	int order;
	struct list_head items;
};

struct pppunit_cache
{
	struct list_head entry;
	int fd;
	int unit_idx;
};

static pthread_mutex_t uc_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(uc_list);
static int uc_size;
static mempool_t uc_pool;

static int ppp_chan_read(struct triton_md_handler_t*);
static int ppp_unit_read(struct triton_md_handler_t*);
static void init_layers(struct ppp_t *);
static void _free_layers(struct ppp_t *);
static void start_first_layer(struct ppp_t *);

void __export ppp_init(struct ppp_t *ppp)
{
	memset(ppp, 0, sizeof(*ppp));
	INIT_LIST_HEAD(&ppp->layers);
	INIT_LIST_HEAD(&ppp->chan_handlers);
	INIT_LIST_HEAD(&ppp->unit_handlers);
	ppp->fd = -1;
	ppp->chan_fd = -1;
	ppp->unit_fd = -1;

	ap_session_init(&ppp->ses);
}

int __export establish_ppp(struct ppp_t *ppp)
{
	struct pppunit_cache *uc = NULL;

	/* Open an instance of /dev/ppp and connect the channel to it */
	if (ioctl(ppp->fd, PPPIOCGCHAN, &ppp->chan_idx) == -1) {
		log_ppp_error("ioctl(PPPIOCGCHAN): %s\n", strerror(errno));
		return -1;
	}

	ppp->chan_fd = open("/dev/ppp", O_RDWR);
	if (ppp->chan_fd < 0) {
		log_ppp_error("open(chan) /dev/ppp: %s\n", strerror(errno));
		return -1;
	}
	
	fcntl(ppp->chan_fd, F_SETFD, fcntl(ppp->chan_fd, F_GETFD) | FD_CLOEXEC);

	if (ioctl(ppp->chan_fd, PPPIOCATTCHAN, &ppp->chan_idx) < 0) {
		log_ppp_error("ioctl(PPPIOCATTCHAN): %s\n", strerror(errno));
		goto exit_close_chan;
	}

	if (uc_size) {
		pthread_mutex_lock(&uc_lock);
		if (!list_empty(&uc_list)) {
			uc = list_entry(uc_list.next, typeof(*uc), entry);
			list_del(&uc->entry);
			--uc_size;
		}
		pthread_mutex_unlock(&uc_lock);
	}

	if (uc) {
		ppp->unit_fd = uc->fd;
		ppp->ses.unit_idx = uc->unit_idx;
		mempool_free(uc);
	} else {
		ppp->unit_fd = open("/dev/ppp", O_RDWR);
		if (ppp->unit_fd < 0) {
			log_ppp_error("open(unit) /dev/ppp: %s\n", strerror(errno));
			goto exit_close_chan;
		}
		
		fcntl(ppp->unit_fd, F_SETFD, fcntl(ppp->unit_fd, F_GETFD) | FD_CLOEXEC);

		ppp->ses.unit_idx = -1;
		if (ioctl(ppp->unit_fd, PPPIOCNEWUNIT, &ppp->ses.unit_idx) < 0) {
			log_ppp_error("ioctl(PPPIOCNEWUNIT): %s\n", strerror(errno));
			goto exit_close_unit;
		}
	
		if (fcntl(ppp->unit_fd, F_SETFL, O_NONBLOCK)) {
			log_ppp_error("ppp: cannot set nonblocking mode: %s\n", strerror(errno));
			goto exit_close_unit;
		}
	}

  if (ioctl(ppp->chan_fd, PPPIOCCONNECT, &ppp->ses.unit_idx) < 0) {
		log_ppp_error("ioctl(PPPIOCCONNECT): %s\n", strerror(errno));
		goto exit_close_unit;
	}

	if (fcntl(ppp->chan_fd, F_SETFL, O_NONBLOCK)) {
		log_ppp_error("ppp: cannot set nonblocking mode: %s\n", strerror(errno));
		goto exit_close_unit;
	}
	
	sprintf(ppp->ses.ifname, "ppp%i", ppp->ses.unit_idx);
	
	log_ppp_info1("connect: %s <--> %s(%s)\n", ppp->ses.ifname, ppp->ses.ctrl->name, ppp->ses.chan_name);

	init_layers(ppp);

	if (list_empty(&ppp->layers)) {
		log_ppp_error("no layers to start\n");
		goto exit_close_unit;
	}

	ppp->buf = mempool_alloc(buf_pool);

	ppp->chan_hnd.fd = ppp->chan_fd;
	ppp->chan_hnd.read = ppp_chan_read;
	ppp->unit_hnd.fd = ppp->unit_fd;
	ppp->unit_hnd.read = ppp_unit_read;
	triton_md_register_handler(ppp->ses.ctrl->ctx, &ppp->chan_hnd);
	triton_md_register_handler(ppp->ses.ctrl->ctx, &ppp->unit_hnd);
	
	triton_md_enable_handler(&ppp->chan_hnd, MD_MODE_READ);
	triton_md_enable_handler(&ppp->unit_hnd, MD_MODE_READ);

	log_ppp_debug("ppp established\n");

	ap_session_starting(&ppp->ses);
	
	start_first_layer(ppp);

	return 0;

exit_close_unit:
	close(ppp->unit_fd);
exit_close_chan:
	close(ppp->chan_fd);

	if (ppp->buf)
		mempool_free(ppp->buf);

	return -1;
}

static void destablish_ppp(struct ppp_t *ppp)
{
	struct pppunit_cache *uc;

	triton_event_fire(EV_SES_PRE_FINISHED, &ppp->ses);

	triton_md_unregister_handler(&ppp->chan_hnd);
	triton_md_unregister_handler(&ppp->unit_hnd);
	
	if (uc_size < conf_unit_cache) {
		uc = mempool_alloc(uc_pool);
		uc->fd = ppp->unit_fd;
		uc->unit_idx = ppp->ses.unit_idx;

		pthread_mutex_lock(&uc_lock);
		list_add_tail(&uc->entry, &uc_list);
		++uc_size;
		pthread_mutex_unlock(&uc_lock);
	} else
		close(ppp->unit_fd);

	close(ppp->chan_fd);
	close(ppp->fd);

	ppp->unit_fd = -1;
	ppp->chan_fd = -1;
	ppp->fd = -1;

	_free_layers(ppp);
	
	log_ppp_debug("ppp destablished\n");

	mempool_free(ppp->buf);

	ap_session_finished(&ppp->ses);
}

/*void print_buf(uint8_t *buf, int size)
{
	int i;
	for(i=0;i<size;i++)
		printf("%x ",buf[i]);
	printf("\n");
}*/

int __export ppp_chan_send(struct ppp_t *ppp, void *data, int size)
{
	int n;

	//printf("ppp_chan_send: ");
	//print_buf((uint8_t*)data,size);
	
	n = write(ppp->chan_fd,data,size);
	if (n < size)
		log_ppp_error("ppp_chan_send: short write %i, excpected %i\n", n, size);
	return n;
}

int __export ppp_unit_send(struct ppp_t *ppp, void *data, int size)
{
	int n;

	//printf("ppp_unit_send: ");
	//print_buf((uint8_t*)data,size);
	
	n=write(ppp->unit_fd, data, size);
	if (n < size)
		log_ppp_error("ppp_unit_send: short write %i, excpected %i\n",n,size);
	return n;
}

static int ppp_chan_read(struct triton_md_handler_t *h)
{
	struct ppp_t *ppp = container_of(h, typeof(*ppp), chan_hnd);
	struct ppp_handler_t *ppp_h;
	uint16_t proto;

	while(1) {
cont:
		ppp->buf_size = read(h->fd, ppp->buf, PPP_MRU);
		if (ppp->buf_size < 0) {
			if (errno == EAGAIN)
				return 0;
			log_ppp_error("ppp_chan_read: %s\n", strerror(errno));
			return 0;
		}

		//printf("ppp_chan_read: ");
		//print_buf(ppp->buf,ppp->buf_size);
		if (ppp->buf_size == 0) {
			ap_session_terminate(&ppp->ses, TERM_NAS_ERROR, 1);
			return 1;
		}

		if (ppp->buf_size < 2) {
			log_ppp_error("ppp_chan_read: short read %i\n", ppp->buf_size);
			continue;
		}

		proto = ntohs(*(uint16_t*)ppp->buf);
		list_for_each_entry(ppp_h, &ppp->chan_handlers, entry) {
			if (ppp_h->proto == proto) {
				ppp_h->recv(ppp_h);
				if (ppp->chan_fd == -1) {
					//ppp->ses.ctrl->finished(ppp);
					return 1;
				}
				goto cont;
			}
		}

		lcp_send_proto_rej(ppp, proto);
		//log_ppp_warn("ppp_chan_read: discarding unknown packet %x\n", proto);
	}
}

static int ppp_unit_read(struct triton_md_handler_t *h)
{
	struct ppp_t *ppp = container_of(h, typeof(*ppp), unit_hnd);
	struct ppp_handler_t *ppp_h;
	uint16_t proto;

	while (1) {
cont:
		ppp->buf_size = read(h->fd, ppp->buf, PPP_MRU);
		if (ppp->buf_size < 0) {
			if (errno == EAGAIN)
				return 0;
			log_ppp_error("ppp_unit_read: %s\n",strerror(errno));
			return 0;
		}

		//printf("ppp_unit_read: %i\n", ppp->buf_size);
		if (ppp->buf_size == 0)
			return 0;
		//print_buf(ppp->buf,ppp->buf_size);

		/*if (ppp->buf_size == 0) {
			ap_session_terminate(ppp, TERM_NAS_ERROR, 1);
			return 1;
		}*/

		if (ppp->buf_size < 2) {
			log_ppp_error("ppp_unit_read: short read %i\n", ppp->buf_size);
			continue;
		}

		proto=ntohs(*(uint16_t*)ppp->buf);
		list_for_each_entry(ppp_h, &ppp->unit_handlers, entry) {
			if (ppp_h->proto == proto) {
				ppp_h->recv(ppp_h);
				if (ppp->unit_fd == -1) {
					//ppp->ses.ctrl->finished(ppp);
					return 1;
				}
				goto cont;
			}
		}
		lcp_send_proto_rej(ppp, proto);
		//log_ppp_warn("ppp_unit_read: discarding unknown packet %x\n", proto);
	}
}

void ppp_recv_proto_rej(struct ppp_t *ppp, uint16_t proto)
{
	struct ppp_handler_t *ppp_h;

	list_for_each_entry(ppp_h, &ppp->chan_handlers, entry) {
		if (ppp_h->proto == proto) {
			if (ppp_h->recv_proto_rej)
				ppp_h->recv_proto_rej(ppp_h);
			return;
		}
	}
	
	list_for_each_entry(ppp_h, &ppp->unit_handlers, entry) {
		if (ppp_h->proto == proto) {
			if (ppp_h->recv_proto_rej)
				ppp_h->recv_proto_rej(ppp_h);
			return;
		}
	}
}

static void __ppp_layer_started(struct ppp_t *ppp, struct ppp_layer_data_t *d)
{
	struct layer_node_t *n = d->node;
	int f = 0;

	list_for_each_entry(d, &n->items, entry) {
		if (!d->started && !d->passive) return;
		if (d->started && !d->optional)
			f = 1;
	}

	if (!f)
		return;
	

	if (n->entry.next == &ppp->layers) {
		if (ppp->ses.state == AP_STATE_STARTING) {
			ap_session_activate(&ppp->ses);
		}
	} else {
		n = list_entry(n->entry.next, typeof(*n), entry);
		list_for_each_entry(d, &n->items, entry) {
			d->starting = 1;
			if (d->layer->start(d)) {
				ap_session_terminate(&ppp->ses, TERM_NAS_ERROR, 0);
				return;
			}
		}
	}
}

void __export ppp_layer_started(struct ppp_t *ppp, struct ppp_layer_data_t *d)
{
	if (d->started)
		return;

	d->started = 1;

	__ppp_layer_started(ppp, d);
}

void __export ppp_layer_passive(struct ppp_t *ppp, struct ppp_layer_data_t *d)
{
	if (d->started)
		return;

	d->passive = 1;
	
	__ppp_layer_started(ppp, d);
}

void __export ppp_layer_finished(struct ppp_t *ppp, struct ppp_layer_data_t *d)
{
	struct layer_node_t *n = d->node;

	d->finished = 1;
	d->starting = 0;

	list_for_each_entry(n, &ppp->layers, entry) {
		list_for_each_entry(d, &n->items, entry) {
			if (d->starting && !d->finished)
				return;
		}
	}

	destablish_ppp(ppp);
}

void __export ppp_terminate(struct ap_session *ses, int hard)
{
	struct ppp_t *ppp = container_of(ses, typeof(*ppp), ses);
	struct layer_node_t *n;
	struct ppp_layer_data_t *d;
	int s = 0;

	if (hard) {
		destablish_ppp(ppp);
		return;
	}
	
	list_for_each_entry(n,&ppp->layers,entry) {
		list_for_each_entry(d,&n->items,entry) {
			if (d->starting) {
				s = 1;
				d->layer->finish(d);
			}
		}
	}
	if (s)
		return;

	destablish_ppp(ppp);
}

void __export ppp_register_chan_handler(struct ppp_t *ppp,struct ppp_handler_t *h)
{
	list_add_tail(&h->entry,&ppp->chan_handlers);
}
void __export ppp_register_unit_handler(struct ppp_t *ppp,struct ppp_handler_t *h)
{
	list_add_tail(&h->entry,&ppp->unit_handlers);
}
void __export ppp_unregister_handler(struct ppp_t *ppp,struct ppp_handler_t *h)
{
	list_del(&h->entry);
}

static int get_layer_order(const char *name)
{
	if (!strcmp(name,"lcp")) return 0;
	if (!strcmp(name,"auth")) return 1;
	if (!strcmp(name,"ccp")) return 2;
	if (!strcmp(name,"ipcp")) return 2;
	if (!strcmp(name,"ipv6cp")) return 2;
	return -1;
}

int __export ppp_register_layer(const char *name, struct ppp_layer_t *layer)
{
	int order;
	struct layer_node_t *n,*n1;

	order = get_layer_order(name);

	if (order < 0)
		return order;

	list_for_each_entry(n, &layers, entry) {
		if (order > n->order)
			continue;
		if (order < n->order) {
			n1 = _malloc(sizeof(*n1));
			memset(n1, 0, sizeof(*n1));
			n1->order = order;
			INIT_LIST_HEAD(&n1->items);
			list_add_tail(&n1->entry, &n->entry);
			n = n1;
		}
		goto insert;
	}
	n1 = _malloc(sizeof(*n1));
	memset(n1, 0, sizeof(*n1));
	n1->order = order;
	INIT_LIST_HEAD(&n1->items);
	list_add_tail(&n1->entry, &layers);
	n = n1;
insert:
	list_add_tail(&layer->entry, &n->items);

	return 0;
}
void __export ppp_unregister_layer(struct ppp_layer_t *layer)
{
	list_del(&layer->entry);
}

static void init_layers(struct ppp_t *ppp)
{
	struct layer_node_t *n, *n1;
	struct ppp_layer_t *l;
	struct ppp_layer_data_t *d;

	list_for_each_entry(n,&layers,entry) {
		n1 = _malloc(sizeof(*n1));
		memset(n1, 0, sizeof(*n1));
		INIT_LIST_HEAD(&n1->items);
		list_add_tail(&n1->entry, &ppp->layers);
		list_for_each_entry(l, &n->items, entry) {
			d = l->init(ppp);
			d->layer = l;
			d->started = 0;
			d->node = n1;
			list_add_tail(&d->entry, &n1->items);
		}
	}
}

static void _free_layers(struct ppp_t *ppp)
{
	struct layer_node_t *n;
	struct ppp_layer_data_t *d;
	
	while (!list_empty(&ppp->layers)) {
		n = list_entry(ppp->layers.next, typeof(*n), entry);
		while (!list_empty(&n->items)) {
			d = list_entry(n->items.next, typeof(*d), entry);
			list_del(&d->entry);
			d->layer->free(d);
		}
		list_del(&n->entry);
		_free(n);
	}
}

static void start_first_layer(struct ppp_t *ppp)
{
	struct layer_node_t *n;
	struct ppp_layer_data_t *d;

	n = list_entry(ppp->layers.next, typeof(*n), entry);
	list_for_each_entry(d, &n->items, entry) {
		d->starting = 1;
		if (d->layer->start(d)) {
			ap_session_terminate(&ppp->ses, TERM_NAS_ERROR, 0);
			return;
		}
	}
}

struct ppp_layer_data_t *ppp_find_layer_data(struct ppp_t *ppp, struct ppp_layer_t *layer)
{
	struct layer_node_t *n;
	struct ppp_layer_data_t *d;

	list_for_each_entry(n,&ppp->layers,entry) {
		list_for_each_entry(d,&n->items,entry) {
			if (d->layer == layer)
				return d;
		}
	}
	
	return NULL;
}

static void load_config(void)
{
	const char *opt;

	opt = conf_get_opt("ppp", "verbose");
	if (opt && atoi(opt) > 0)
		conf_ppp_verbose = 1;

	opt = conf_get_opt("ppp", "unit-cache");
	if (opt && atoi(opt) > 0)
		conf_unit_cache = atoi(opt);
	else
		conf_unit_cache = 0;
}

static void init(void)
{
	buf_pool = mempool_create(PPP_MRU);
	uc_pool = mempool_create(sizeof(struct pppunit_cache));

	load_config();
	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
}

DEFINE_INIT(2, init);
