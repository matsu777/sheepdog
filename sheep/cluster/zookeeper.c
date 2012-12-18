/*
 * Copyright (C) 2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <search.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <zookeeper/zookeeper.h>
#include <urcu/uatomic.h>

#include "cluster.h"
#include "event.h"
#include "work.h"
#include "util.h"
#include "rbtree.h"

#define SESSION_TIMEOUT 30000		/* millisecond */
#define MEMBER_CREATE_TIMEOUT SESSION_TIMEOUT
#define MEMBER_CREATE_INTERVAL 10	/* millisecond */

#define BASE_ZNODE "/sheepdog"
#define QUEUE_ZNODE BASE_ZNODE "/queue"
#define MEMBER_ZNODE BASE_ZNODE "/member"

/* iterate child znodes */
#define FOR_EACH_ZNODE(parent, path, strs)			       \
	for (zk_get_children(parent, strs),		               \
		     (strs)->data += (strs)->count;		       \
	     (strs)->count-- ?					       \
		     sprintf(path, "%s/%s", parent, *--(strs)->data) : \
		     (free((strs)->data), 0);			       \
	     free(*(strs)->data))

enum zk_event_type {
	EVENT_JOIN_REQUEST = 1,
	EVENT_JOIN_RESPONSE,
	EVENT_LEAVE,
	EVENT_BLOCK,
	EVENT_NOTIFY,
};

struct zk_node {
	bool joined;
	struct rb_node rb;
	clientid_t clientid;
	struct sd_node node;
};

struct zk_event {
	enum zk_event_type type;
	struct zk_node sender;

	enum cluster_join_result join_result;

	size_t buf_len;
	uint8_t buf[SD_MAX_EVENT_BUF_SIZE];
};

static uatomic_bool zk_notify_blocked;

/* leave event circular array */
static struct zk_event zk_levents[SD_MAX_NODES];
static int nr_zk_levents;
static unsigned zk_levent_head;
static unsigned zk_levent_tail;
static bool called_by_zk_unblock;

static struct sd_node sd_nodes[SD_MAX_NODES];
static size_t nr_sd_nodes;

struct rb_root zk_node_root = RB_ROOT;

static inline bool is_blocking_event(struct zk_event *ev)
{
	return ev->type == EVENT_BLOCK || ev->type == EVENT_JOIN_REQUEST;
}

static struct zk_node *zk_tree_insert(struct zk_node *new)
{
	struct rb_node **p = &zk_node_root.rb_node;
	struct rb_node *parent = NULL;
	struct zk_node *entry;

	while (*p) {
		int cmp;

		parent = *p;
		entry = rb_entry(parent, struct zk_node, rb);

		cmp = node_id_cmp(&new->node.nid, &entry->node.nid);
		if (cmp < 0)
			p = &(*p)->rb_left;
		else if (cmp > 0)
			p = &(*p)->rb_right;
		else
			/* already has this entry */
			return entry;
	}

	rb_link_node(&new->rb, parent, p);
	rb_insert_color(&new->rb, &zk_node_root);

	return NULL; /* insert successfully */
}

static struct zk_node *zk_tree_search(const struct node_id *nid)
{
	struct rb_node *n = zk_node_root.rb_node;
	struct zk_node *t;

	while (n) {
		int cmp;

		t = rb_entry(n, struct zk_node, rb);
		cmp = node_id_cmp(nid, &t->node.nid);

		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else
			return t; /* found it */
	}

	return NULL;
}

/* zookeeper API wrapper */
static zhandle_t *zhandle;
static struct zk_node this_node;

static inline ZOOAPI int zk_delete_node(const char *path, int version)
{
	int rc;
	do {
		rc = zoo_delete(zhandle, path, version);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	if (rc != ZOK)
		eprintf("failed, path:%s, rc:%d\n", path, rc);
	return rc;
}

static inline ZOOAPI void
zk_init_node(const char *path)
{
	int rc;
	do {
		rc = zoo_create(zhandle, path, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0,
				NULL, 0);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);

	if (rc != ZOK && rc != ZNODEEXISTS)
		panic("failed, path:%s, rc:%d\n", path, rc);
}

static inline ZOOAPI void
zk_create_node(const char *path, const char *value, int valuelen,
	       const struct ACL_vector *acl, int flags, char *path_buffer,
	       int path_buffer_len)
{
	int rc;
	do {
		rc = zoo_create(zhandle, path, value, valuelen, acl,
				flags, path_buffer, path_buffer_len);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	if (rc != ZOK)
		panic("failed, path:%s, rc:%d\n", path, rc);
}

static inline ZOOAPI int zk_get_data(const char *path, void *buffer,
				     int *buffer_len)
{
	int rc;
	do {
		rc = zoo_get(zhandle, path, 1, (char *)buffer,
			     buffer_len, NULL);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	return rc;
}

static inline ZOOAPI int
zk_set_data(const char *path, const char *buffer, int buflen, int version)
{
	int rc;
	do {
		rc = zoo_set(zhandle, path, buffer, buflen, version);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	if (rc != ZOK)
		panic("failed, path:%s, rc:%d\n", path, rc);
	return rc;
}

static inline ZOOAPI int zk_node_exists(const char *path)
{
	int rc;
	do {
		rc = zoo_exists(zhandle, path, 1, NULL);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);

	return rc;
}

static inline ZOOAPI void zk_get_children(const char *path,
					  struct String_vector *strings)
{
	int rc;
	do {
		rc = zoo_get_children(zhandle, path, 1, strings);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	if (rc != ZOK)
		panic("failed:%s, rc:%d\n", path, rc);
}

/* ZooKeeper-based queue */
static int efd;
static int32_t queue_pos;

static bool zk_queue_empty(void)
{
	int rc;
	char path[256];

	sprintf(path, QUEUE_ZNODE "/%010"PRId32, queue_pos);

	rc = zk_node_exists(path);
	if (rc == ZOK)
		return false;

	return true;
}

static void zk_queue_push(struct zk_event *ev)
{
	static bool first_push = true;
	int len;
	char path[256], buf[256];
	eventfd_t value = 1;

	len = (char *)(ev->buf) - (char *)ev + ev->buf_len;
	sprintf(path, "%s/", QUEUE_ZNODE);
	zk_create_node(path, (char *)ev, len,
		       &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE, buf, sizeof(buf));
	dprintf("create:%s, queue_pos:%010"PRId32", len:%d\n", buf, queue_pos,
		len);

	if (first_push) {
		uint32_t seq;

		sscanf(buf, QUEUE_ZNODE "/%"PRId32, &seq);
		queue_pos = seq;
		eventfd_write(efd, value);
		first_push = false;
	}
}

/*
 * Change the event in place and expect the dedicated handler to be called
 * via zk_watcher which wakes up one of the zk_event_handlers.
 */
static int zk_queue_push_back(struct zk_event *ev)
{
	int len;
	char path[256];

	queue_pos--;

	len = (char *)(ev->buf) - (char *)ev + ev->buf_len;
	sprintf(path, QUEUE_ZNODE "/%010"PRId32, queue_pos);
	zk_set_data(path, (char *)ev, len, -1);
	dprintf("update path:%s, queue_pos:%010"PRId32", len:%d\n",
		path, queue_pos, len);

	return 0;
}

/*
 * Peek next queue event and if it exists, we must watch it and manually notify
 * it in order not to lose it.
 */
static void zk_queue_peek_next_notify(const char *path)
{
	int rc = zk_node_exists(path);
	if (rc == ZOK)
		eventfd_write(efd, 1);
}

static int zk_queue_pop(struct zk_event *ev)
{
	int rc, len;
	int nr_levents;
	char path[256];
	struct zk_event *lev;
	eventfd_t value = 1;

	/*
	 * Continue to process LEAVE event even if we have an unfinished BLOCK
	 * event.
	 */
	if (!called_by_zk_unblock && uatomic_read(&nr_zk_levents)) {
		nr_levents = uatomic_sub_return(&nr_zk_levents, 1) + 1;
		dprintf("nr_levents:%d, head:%u\n", nr_levents, zk_levent_head);

		lev = &zk_levents[zk_levent_head%SD_MAX_NODES];

		/*
		 * If the node pointed to by queue_pos was send by this leaver,
		 * and it have blocked whole cluster, we should ignore it.
		 */
		len = sizeof(*ev);
		sprintf(path, QUEUE_ZNODE "/%010"PRId32, queue_pos);
		rc = zk_get_data(path, ev, &len);
		if (rc == ZOK &&
		    node_eq(&ev->sender.node, &lev->sender.node) &&
		    is_blocking_event(ev)) {
			dprintf("this queue_pos:%010"PRId32" have blocked whole"
				" cluster, ignore it\n", queue_pos);
			queue_pos++;

			sprintf(path, QUEUE_ZNODE "/%010"PRId32, queue_pos);
			zk_queue_peek_next_notify(path);
		}

		memcpy(ev, lev, sizeof(*ev));
		zk_levent_head++;

		if (uatomic_read(&nr_zk_levents) || rc == ZOK) {
			/*
			 * we have pending leave events or queue nodes,
			 * manual notify
			 */
			dprintf("write event to efd:%d\n", efd);
			eventfd_write(efd, value);
		}

		return 0;
	}

	if (!called_by_zk_unblock && uatomic_is_true(&zk_notify_blocked))
		return -1;

	if (zk_queue_empty())
		return -1;

	len = sizeof(*ev);
	sprintf(path, QUEUE_ZNODE "/%010"PRId32, queue_pos);
	rc = zk_get_data(path, ev, &len);
	if (rc != ZOK)
		panic("failed to zk_get_data path:%s, rc:%d\n", path, rc);
	dprintf("read path:%s, type:%d, len:%d, rc:%d\n", path, ev->type,
		len, rc);

	queue_pos++;

	/*
	 * This event will be pushed back to the queue,
	 * we just wait for the arrival of its updated,
	 * not need to watch next data.
	 */
	if (is_blocking_event(ev))
		return 0;

	sprintf(path, QUEUE_ZNODE "/%010"PRId32, queue_pos);
	zk_queue_peek_next_notify(path);
	return 0;
}

static int zk_member_empty(void)
{
	struct String_vector strs;

	zk_get_children(MEMBER_ZNODE, &strs);
	return (strs.count == 0);
}

static inline void zk_tree_add(struct zk_node *node)
{
	struct zk_node *zk = malloc(sizeof(*zk));
	*zk = *node;
	if (zk_tree_insert(zk))
		free(zk);
}

static inline void zk_tree_del(struct zk_node *node)
{
	rb_erase(&node->rb, &zk_node_root);
	free(node);
}

static inline void zk_tree_destroy(void)
{
	struct zk_node *zk;
	int i;

	for (i = 0; i < nr_sd_nodes; i++) {
		zk = zk_tree_search(&sd_nodes[i].nid);
		if (zk)
			zk_tree_del(zk);
	}
}

static inline void build_node_list(void)
{
	struct rb_node *n;
	struct zk_node *zk;

	nr_sd_nodes = 0;
	for (n = rb_first(&zk_node_root); n; n = rb_next(n)) {
		zk = rb_entry(n, struct zk_node, rb);
		sd_nodes[nr_sd_nodes++] = zk->node;
	}
	dprintf("nr_sd_nodes:%zu\n", nr_sd_nodes);
}

static bool is_master(void)
{
	struct rb_node *n;
	struct zk_node *zk;

	if (!nr_sd_nodes) {
		if (zk_member_empty())
			return true;
		else
			return false;
	}

	n = rb_first(&zk_node_root);
	zk = rb_entry(n, struct zk_node, rb);
	if (node_eq(&zk->node, &this_node.node))
		return true;

	return false;
}

static void zk_queue_init(void)
{
	zk_init_node(BASE_ZNODE);
	zk_init_node(QUEUE_ZNODE);
	zk_init_node(MEMBER_ZNODE);
}

static void zk_member_init(void)
{
	int rc, len;
	struct String_vector strs;
	struct zk_node znode;
	char path[256];

	if (zk_member_empty())
		return;

	FOR_EACH_ZNODE(MEMBER_ZNODE, path, &strs) {
		len = sizeof(znode);
		rc = zk_get_data(path, &znode, &len);
		if (rc != ZOK)
			continue;

		switch (rc) {
		case ZOK:
			zk_tree_add(&znode);
		case ZNONODE:
			break;
		default:
			panic("zk_get_data failed:%s, rc:%d\n", path, rc);
		}
	}
}

static int add_event(enum zk_event_type type, struct zk_node *znode, void *buf,
		     size_t buf_len)
{
	struct zk_event ev;

	ev.type = type;
	ev.sender = *znode;
	ev.buf_len = buf_len;
	if (buf)
		memcpy(ev.buf, buf, buf_len);
	zk_queue_push(&ev);
	return 0;
}

static int leave_event(struct zk_node *znode)
{
	int nr_levents;
	struct zk_event *ev;
	const eventfd_t value = 1;

	ev = &zk_levents[zk_levent_tail % SD_MAX_NODES];
	ev->type = EVENT_LEAVE;
	ev->sender = *znode;
	ev->buf_len = 0;

	nr_levents = uatomic_add_return(&nr_zk_levents, 1);
	dprintf("nr_zk_levents:%d, tail:%u\n", nr_levents, zk_levent_tail);

	zk_levent_tail++;

	eventfd_write(efd, value);
	return 0;
}

static void zk_watcher(zhandle_t *zh, int type, int state, const char *path,
		       void *ctx)
{
	eventfd_t value = 1;
	const clientid_t *cid;
	char str[256], *p;
	int ret;
	struct zk_node znode;

	dprintf("path:%s, type:%d\n", path, type);

	/* discard useless event */
	if (type < 0 || type == ZOO_CHILD_EVENT)
		return;

	if (type == ZOO_SESSION_EVENT) {
		cid = zoo_client_id(zh);
		assert(cid != NULL);
		this_node.clientid = *cid;
		dprintf("session change, clientid:%"PRId64"\n", cid->client_id);
	}

	if (type == ZOO_CREATED_EVENT || type == ZOO_CHANGED_EVENT) {
		ret = sscanf(path, MEMBER_ZNODE "/%s", str);
		if (ret == 1)
			zk_node_exists(path);
	}

	if (type == ZOO_DELETED_EVENT) {
		ret = sscanf(path, MEMBER_ZNODE "/%s", str);
		if (ret != 1)
			return;
		p = strrchr(path, '/');
		p++;

		str_to_node(p, &znode.node);
		dprintf("zk_nodes leave:%s\n", node_to_str(&znode.node));

		leave_event(&znode);
		return;
	}

	eventfd_write(efd, value);
}

static int zk_join(const struct sd_node *myself,
		   void *opaque, size_t opaque_len)
{
	int rc;
	char path[256];
	const clientid_t *cid;

	this_node.node = *myself;

	sprintf(path, MEMBER_ZNODE "/%s", node_to_str(myself));
	rc = zk_node_exists(path);
	if (rc == ZOK) {
		eprintf("Previous zookeeper session exist, shoot myself.\n"
			"Wait for a while and restart me again\n");
		exit(1);
	}

	this_node.joined = false;
	cid = zoo_client_id(zhandle);
	assert(cid != NULL);
	this_node.clientid = *cid;

	dprintf("clientid:%"PRId64"\n", cid->client_id);

	return add_event(EVENT_JOIN_REQUEST, &this_node,
			 opaque, opaque_len);
}

static int zk_leave(void)
{
	char path[256];
	sprintf(path, MEMBER_ZNODE "/%s", node_to_str(&this_node.node));
	dprintf("try to delete member path:%s\n", path);
	return zk_delete_node(path, -1);
}

static int zk_notify(void *msg, size_t msg_len)
{
	return add_event(EVENT_NOTIFY, &this_node, msg, msg_len);
}

static void zk_block(void)
{
	add_event(EVENT_BLOCK, &this_node, NULL, 0);
}

static void zk_unblock(void *msg, size_t msg_len)
{
	int rc;
	struct zk_event ev;
	eventfd_t value = 1;

	called_by_zk_unblock = true;
	rc = zk_queue_pop(&ev);
	called_by_zk_unblock = false;
	assert(rc == 0);

	ev.type = EVENT_NOTIFY;
	ev.buf_len = msg_len;
	if (msg)
		memcpy(ev.buf, msg, msg_len);

	zk_queue_push_back(&ev);

	uatomic_set_false(&zk_notify_blocked);

	/* this notify is necessary */
	dprintf("write event to efd:%d\n", efd);
	eventfd_write(efd, value);
}

static void zk_handle_join_request(struct zk_event *ev)
{
	enum cluster_join_result res;

	dprintf("sender: %s, joined: %d\n", node_to_str(&ev->sender.node),
		ev->sender.joined);

	if (!is_master()) {
		/* Let's await master acking the join-request */
		queue_pos--;
		return;
	}

	res = sd_check_join_cb(&ev->sender.node, ev->buf);
	ev->join_result = res;
	ev->type = EVENT_JOIN_RESPONSE;
	ev->sender.joined = true;

	zk_queue_push_back(ev);

	if (res == CJ_RES_MASTER_TRANSFER) {
		eprintf("failed to join sheepdog cluster: "
			"please retry when master is up\n");
		zk_leave();
		exit(1);
	}
	dprintf("I'm the master now\n");
}

static void zk_handle_join_response(struct zk_event *ev)
{
	char path[256];

	dprintf("JOIN RESPONSE\n");
	if (is_master() &&
	    !node_eq(&ev->sender.node, &this_node.node)) {
		/* wait util the member node has been created */
		int retry =
			MEMBER_CREATE_TIMEOUT / MEMBER_CREATE_INTERVAL;

		sprintf(path, MEMBER_ZNODE "/%s",
			node_to_str(&ev->sender.node));

		while (retry && zk_node_exists(path) == ZNONODE) {
			usleep(MEMBER_CREATE_INTERVAL * 1000);
			retry--;
		}
		if (retry <= 0) {
			dprintf("%s failed to create member, ignore it\n",
				node_to_str(&ev->sender.node));
			return;
		}
	}

	if (node_eq(&ev->sender.node, &this_node.node))
		zk_member_init();

	if (ev->join_result == CJ_RES_MASTER_TRANSFER)
		/*
		 * Sheepdog assumes that only one sheep(master will kill
		 * itself) is alive in MASTER_TRANSFER scenario. So only
		 * the joining sheep will run into here.
		 */
		zk_tree_destroy();

	zk_tree_add(&ev->sender);
	dprintf("sender:%s, joined:%d\n", node_to_str(&ev->sender.node),
		ev->sender.joined);

	switch (ev->join_result) {
	case CJ_RES_SUCCESS:
	case CJ_RES_JOIN_LATER:
	case CJ_RES_MASTER_TRANSFER:
		sprintf(path, MEMBER_ZNODE "/%s",
			node_to_str(&ev->sender.node));
		if (node_eq(&ev->sender.node, &this_node.node)) {
			dprintf("create path:%s\n", path);
			zk_create_node(path, (char *)&ev->sender,
				       sizeof(ev->sender),
				       &ZOO_OPEN_ACL_UNSAFE,
				       ZOO_EPHEMERAL, NULL, 0);
		} else {
			zk_node_exists(path);
		}
		break;
	default:
		break;
	}

	build_node_list();
	sd_join_handler(&ev->sender.node, sd_nodes, nr_sd_nodes,
			ev->join_result, ev->buf);
}

static void zk_handle_leave(struct zk_event *ev)
{
	struct zk_node *n = zk_tree_search(&ev->sender.node.nid);
	if (!n) {
		dprintf("can't find this leave node:%s, ignore it.\n",
			node_to_str(&ev->sender.node));
		return;
	}
	zk_tree_del(n);
	build_node_list();
	sd_leave_handler(&ev->sender.node, sd_nodes, nr_sd_nodes);
}

static void zk_handle_block(struct zk_event *ev)
{
	dprintf("BLOCK\n");
	queue_pos--;
	if (sd_block_handler(&ev->sender.node))
		assert(uatomic_set_true(&zk_notify_blocked));
}

static void zk_handle_notify(struct zk_event *ev)
{
	dprintf("NOTIFY\n");
	sd_notify_handler(&ev->sender.node, ev->buf, ev->buf_len);
}

static void (*const zk_event_handlers[])(struct zk_event *ev) = {
	[EVENT_JOIN_REQUEST]	= zk_handle_join_request,
	[EVENT_JOIN_RESPONSE]	= zk_handle_join_response,
	[EVENT_LEAVE]		= zk_handle_leave,
	[EVENT_BLOCK]		= zk_handle_block,
	[EVENT_NOTIFY]		= zk_handle_notify,
};

static const int zk_max_event_handlers = ARRAY_SIZE(zk_event_handlers);

static void zk_event_handler(int listen_fd, int events, void *data)
{
	eventfd_t value;
	struct zk_event ev;

	if (events & EPOLLHUP) {
		eprintf("zookeeper driver received EPOLLHUP event, exiting.\n");
		log_close();
		exit(1);
	}

	if (eventfd_read(efd, &value) < 0)
		return;

	if (zk_queue_pop(&ev) < 0)
		return;

	if (ev.type < zk_max_event_handlers && zk_event_handlers[ev.type])
		zk_event_handlers[ev.type](&ev);
	else
		eprintf("unhandled type %d\n", ev.type);
}

static int zk_init(const char *option)
{
	int ret;

	if (!option) {
		eprintf("specify comma separated host:port pairs, "
			"each corresponding to a zk server.\n");
		eprintf("e.g. sheep /store -c zookeeper:127.0.0.1:"
			"3000,127.0.0.1:3001,127.0.0.1:3002\n");
		return -1;
	}

	zhandle = zookeeper_init(option, zk_watcher, SESSION_TIMEOUT, NULL, NULL,
				 0);
	if (!zhandle) {
		eprintf("failed to connect to zk server %s\n", option);
		return -1;
	}
	dprintf("request session timeout:%dms, "
		"negotiated session timeout:%dms\n",
		SESSION_TIMEOUT, zoo_recv_timeout(zhandle));

	zk_queue_init();

	efd = eventfd(0, EFD_NONBLOCK);
	if (efd < 0) {
		eprintf("failed to create an event fd: %m\n");
		return -1;
	}

	ret = register_event(efd, zk_event_handler, NULL);
	if (ret) {
		eprintf("failed to register zookeeper event handler (%d)\n",
			ret);
		return -1;
	}

	return 0;
}

static struct cluster_driver cdrv_zookeeper = {
	.name       = "zookeeper",

	.init       = zk_init,
	.join       = zk_join,
	.leave      = zk_leave,
	.notify     = zk_notify,
	.block      = zk_block,
	.unblock    = zk_unblock,
};

cdrv_register(cdrv_zookeeper);
