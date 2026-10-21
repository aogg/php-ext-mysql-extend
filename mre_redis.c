/*
  +----------------------------------------------------------------------+
  | mre - Redis capture via PHP streams RESP sniffing                    |
  |                                                                      |
  | Why the stream layer?                                                |
  |   phpredis (Redis / RedisCluster) and Predis (userspace classes)     |
  |   ultimately create their sockets through the same PHP streams       |
  |   transport layer ("tcp://" via php_stream_xport_create /            |
  |   fsockopen / stream_socket_client). We wrap the "tcp" transport     |
  |   factory: connections whose port matches mre.redis_port get their   |
  |   stream ops replaced by an instrumented copy that parses the RESP   |
  |   protocol in both directions.                                       |
  |                                                                      |
  | Limitations (documented in docs/设计.md):                            |
  |   - "tls://" / "ssl://" Redis connections are not sniffed            |
  |   - unix socket Redis connections are not sniffed                    |
  |   - inline (non-RESP) client traffic is logged line by line          |
  |   - array replies are summarized as "*N" (no element dump)           |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#define MRE_REDIS_PEER_MAX  96
#define MRE_REDIS_MAX_DEPTH 8

/* ------------------------------------------------------------------ */
/* RESP request (client -> server) incremental parser                   */
/* ------------------------------------------------------------------ */
typedef enum {
	MREQ_START = 0,
	MREQ_NARGS,
	MREQ_ARGLEN,
	MREQ_ARGDATA,
	MREQ_INLINE
} mre_req_state;

typedef struct {
	mre_req_state state;
	int           nargs;
	int           arg_idx;
	size_t        arg_len;
	size_t        arg_pos;
	char          num[24];
	size_t        num_len;
	char          cmd[MRE_REDIS_CMD_MAX];
	size_t        cmd_len;
	zend_bool     trunc;
	zend_bool     capture;
} mre_req_parser;

/* ------------------------------------------------------------------ */
/* RESP reply (server -> client) incremental summarizer                 */
/* ------------------------------------------------------------------ */
typedef enum {
	MRSP_IDLE = 0,
	MRSP_LINE,       /* + - : replies, read until LF */
	MRSP_BULK_LEN,   /* $<len>                        */
	MRSP_BULK_DATA,  /* <len> bytes + CRLF            */
	MRSP_ARRAY_LEN   /* *<n>                          */
} mre_resp_state;

typedef struct {
	mre_resp_state state;
	int            depth;                            /* open arrays  */
	int            counts[MRE_REDIS_MAX_DEPTH];      /* pending elems */
	char           hdrs[MRE_REDIS_MAX_DEPTH][24];    /* "*N" headers  */
	size_t         bulk_len;
	size_t         bulk_pos;
	char           buf[MRE_REDIS_REPLY_MAX];
	size_t         len;
	zend_bool      trunc;
	zend_bool      capture;
} mre_resp_parser;

/* ------------------------------------------------------------------ */
/* per stream wrapper (allocated with the stream's persistence)         */
/* ------------------------------------------------------------------ */
typedef struct {
	php_stream_ops       ops;   /* patched copy; stream->ops points here */
	const php_stream_ops *orig;
	int                  persistent;
	char                 peer[MRE_REDIS_PEER_MAX];
	mre_req_parser       req;
	mre_resp_parser      resp;
} mre_sockops_wrapper;

static php_stream_transport_factory mre_orig_tcp_factory = NULL;
static zend_bool mre_factory_installed = (zend_bool)0;

/* ------------------------------------------------------------------ */
/* small helpers                                                        */
/* ------------------------------------------------------------------ */
static zend_bool mre_redis_enabled(void)
{
	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	return (zend_bool)(MRE_G(cfg).enable && MRE_G(cfg).capture_redis);
}

/* find the ':' that starts the port field (portable, IPv6 aware) */
static const char *mre_find_port_colon(const char *s, size_t len)
{
	const char *i;

	if (!s || len == 0) {
		return NULL;
	}
	if (s[0] == '[') {
		/* [v6:addr]:port */
		for (i = s; i < s + len; i++) {
			if (*i == ']') {
				if (i + 1 < s + len && i[1] == ':') {
					return i + 1;
				}
				return NULL;
			}
		}
		return NULL;
	}
	for (i = s; i < s + len; i++) {
		if (*i == ':') {
			return i;
		}
	}
	return NULL;
}

/* parse the port out of a "host:port" resourcename (IPv6 safe) */
static int mre_redis_port_of(const char *resourcename, size_t len)
{
	const char *colon;
	char buf[8];
	size_t plen;
	const char *p;

	if (!resourcename || len == 0) {
		return -1;
	}
	/*
	 * Use the colon that starts the port field: a few callers append
	 * extra ":n" segments to resourcename (e.g. fsockopen with both a
	 * URL port and a port argument), so scanning backwards would pick
	 * up the wrong field. Digits stop the port; a following ":..." is
	 * ignored.
	 */
	colon = mre_find_port_colon(resourcename, len);
	if (!colon) {
		return -1;
	}
	plen = len - (size_t)(colon - resourcename) - 1;
	for (p = colon + 1; p < resourcename + len; p++) {
		if (*p < '0' || *p > '9') {
			plen = (size_t)(p - colon - 1);
			break;
		}
	}
	if (plen == 0 || plen >= sizeof(buf)) {
		return -1;
	}
	memcpy(buf, colon + 1, plen);
	buf[plen] = '\0';
	return atoi(buf);
}

/* extract "host" part of "host:port" into dst */
static void mre_redis_host_of(char *dst, size_t cap,
                              const char *resourcename, size_t len)
{
	const char *colon = mre_find_port_colon(resourcename, len);
	size_t hl = colon ? (size_t)(colon - resourcename) : len;

	if (hl >= cap) {
		hl = cap - 1;
	}
	if (resourcename && hl > 0) {
		memcpy(dst, resourcename, hl);
	}
	dst[hl] = '\0';
}

/* ------------------------------------------------------------------ */
/* request parser                                                       */
/* ------------------------------------------------------------------ */
static void mre_req_init(mre_req_parser *p, zend_bool capture)
{
	memset(p, 0, sizeof(*p));
	p->state = MREQ_START;
	p->capture = capture;
}

/* append raw bytes to the accumulating command text (bounded) */
static void mre_req_append(mre_req_parser *p, const char *s, size_t n)
{
	size_t space;

	if (!p || p->trunc) {
		return;
	}
	space = sizeof(p->cmd) - p->cmd_len;
	if (n + 1 > space) {
		if (space > 4) {
			memcpy(p->cmd + p->cmd_len, s, space - 4);
			memcpy(p->cmd + p->cmd_len + (space - 4), "...", 3);
			p->cmd_len += space - 1;
			p->cmd[p->cmd_len] = '\0';
		}
		p->trunc = (zend_bool)1;
		return;
	}
	memcpy(p->cmd + p->cmd_len, s, n);
	p->cmd_len += n;
	p->cmd[p->cmd_len] = '\0';
}

/* complete request: emit one log line */
static void mre_req_emit(mre_req_parser *p, const char *peer)
{
	mre_extra e;

	if (p->cmd_len == 0) {
		return;
	}
	mre_extra_init(&e);
	if (peer) {
		e.has_port = (zend_bool)1;
		e.port = mre_redis_port_of(peer, strlen(peer));
		e.has_host = (zend_bool)1;
		mre_redis_host_of(e.host, sizeof(e.host), peer, strlen(peer));
	}
	e.truncated = p->trunc;
	mre_log_write(MRE_TYPE_REDIS, MRE_DIR_REQ, p->cmd, p->cmd_len, &e);
	p->cmd_len = 0;
	p->cmd[0] = '\0';
	p->trunc = (zend_bool)0;
}

/* feed written bytes into the request parser */
static void mre_req_feed(mre_req_parser *p, const char *buf, size_t n,
                         const char *peer)
{
	size_t i;

	if (!p->capture) {
		return;
	}
	for (i = 0; i < n; i++) {
		char c = buf[i];

		switch (p->state) {
			case MREQ_START:
				if (c == '*') {
					p->state = MREQ_NARGS;
					p->num_len = 0;
					p->cmd_len = 0;
					p->cmd[0] = '\0';
					p->trunc = (zend_bool)0;
				} else if (c != '\r' && c != '\n') {
					/* inline command (non-RESP client) */
					p->state = MREQ_INLINE;
					p->cmd_len = 0;
					p->cmd[0] = '\0';
					p->trunc = (zend_bool)0;
					mre_req_append(p, &c, 1);
				}
				break;

			case MREQ_NARGS:
				if (c >= '0' && c <= '9' && p->num_len < sizeof(p->num) - 1) {
					p->num[p->num_len++] = c;
				} else if (c == '\n') {
					p->num[p->num_len] = '\0';
					p->nargs = atoi(p->num);
					p->num_len = 0; /* ARGLEN reuses the digit buffer */
					p->arg_idx = 0;
					p->state = (p->nargs > 0) ? MREQ_ARGLEN : MREQ_START;
				}
				break;

			case MREQ_ARGLEN:
				if (c >= '0' && c <= '9' && p->num_len < sizeof(p->num) - 1) {
					p->num[p->num_len++] = c;
				} else if (c == '\n') {
					long al;

					p->num[p->num_len] = '\0';
					al = atol(p->num);
					p->num_len = 0;
					if (al < 0) {
						p->state = MREQ_ARGLEN; /* cannot happen; stay safe */
					} else {
						p->arg_len = (size_t)al;
						p->arg_pos = 0;
						p->state = MREQ_ARGDATA;
						if (p->arg_idx > 0) {
							mre_req_append(p, " ", 1);
						}
					}
				}
				break;

			case MREQ_ARGDATA:
				if (p->arg_pos < p->arg_len) {
					if (p->arg_pos < MRE_REDIS_ARG_MAX) {
						mre_req_append(p, &c, 1);
						if (p->arg_pos + 1 == MRE_REDIS_ARG_MAX) {
							mre_req_append(p, "...", 3);
						}
					}
					p->arg_pos++;
				} else if (p->arg_pos == p->arg_len && c == '\r') {
					p->arg_pos++; /* CR of the arg terminator */
				} else if (p->arg_pos == p->arg_len + 1 && c == '\n') {
					p->arg_pos++; /* LF of the arg terminator */
					p->arg_idx++;
					if (p->arg_idx < p->nargs) {
						p->state = MREQ_ARGLEN;
					} else {
						p->state = MREQ_START;
						mre_req_emit(p, peer);
					}
				}
				break;

			case MREQ_INLINE:
				if (c == '\n') {
					size_t cl = p->cmd_len;

					if (cl > 0 && p->cmd[cl - 1] == '\r') {
						cl--;
						p->cmd[cl] = '\0';
					}
					p->cmd_len = cl;
					mre_req_emit(p, peer);
					p->cmd_len = 0;
					p->cmd[0] = '\0';
				} else if (c != '\r') {
					mre_req_append(p, &c, 1);
				}
				break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* reply summarizer                                                     */
/* ------------------------------------------------------------------ */
static void mre_resp_init(mre_resp_parser *p, zend_bool capture)
{
	memset(p, 0, sizeof(*p));
	p->state = MRSP_IDLE;
	p->capture = capture;
}

static void mre_resp_reset(mre_resp_parser *p)
{
	p->len = 0;
	p->buf[0] = '\0';
	p->trunc = (zend_bool)0;
}

static void mre_resp_append(mre_resp_parser *p, const char *s, size_t n)
{
	size_t space;

	if (p->trunc) {
		return;
	}
	space = sizeof(p->buf) - p->len;
	if (n + 1 > space) {
		if (space > 4) {
			memcpy(p->buf + p->len, s, space - 4);
			memcpy(p->buf + p->len + (space - 4), "...", 3);
			p->len += space - 1;
			p->buf[p->len] = '\0';
		}
		p->trunc = (zend_bool)1;
		return;
	}
	memcpy(p->buf + p->len, s, n);
	p->len += n;
	p->buf[p->len] = '\0';
}

/*
 * One reply element finished. Track array nesting:
 * - top level (depth 0): emit the summary line and reset
 * - inside an array: decrement the pending counter; when the array is
 *   complete, restore its "*N" header as the current summary and treat
 *   the array itself as one completed element of its parent.
 */
static void mre_resp_complete(mre_resp_parser *p, const char *peer)
{
	while (1) {
		if (p->depth == 0) {
			mre_extra e;

			mre_extra_init(&e);
			if (peer) {
				e.has_port = (zend_bool)1;
				e.port = mre_redis_port_of(peer, strlen(peer));
			}
			e.truncated = p->trunc;
			mre_log_write(MRE_TYPE_REDIS, MRE_DIR_RESP, p->buf, p->len, &e);
			mre_resp_reset(p);
			return;
		}
		p->counts[p->depth - 1]--;
		if (p->counts[p->depth - 1] > 0) {
			mre_resp_reset(p); /* next element starts a fresh summary */
			return;
		}
		/* array finished: restore its header as the current summary */
		p->depth--;
		mre_resp_reset(p);
		mre_resp_append(p, p->hdrs[p->depth], strlen(p->hdrs[p->depth]));
	}
}

/* feed read bytes into the reply summarizer */
static void mre_resp_feed(mre_resp_parser *p, const char *buf, size_t n,
                          const char *peer)
{
	size_t i;

	if (!p->capture) {
		return;
	}
	for (i = 0; i < n; i++) {
		char c = buf[i];
		char tmp[2];

		switch (p->state) {
			case MRSP_IDLE:
				mre_resp_reset(p);
				if (c == '+' || c == '-' || c == ':') {
					tmp[0] = c; tmp[1] = '\0';
					mre_resp_append(p, tmp, 1);
					p->state = MRSP_LINE;
				} else if (c == '$') {
					p->bulk_len = 0;
					p->bulk_pos = 0;
					p->state = MRSP_BULK_LEN;
				} else if (c == '*') {
					p->bulk_len = 0;
					p->state = MRSP_ARRAY_LEN;
				}
				break;

			case MRSP_LINE:
				if (c == '\n') {
					if (p->len > 0 && p->buf[p->len - 1] == '\r') {
						p->len--;
						p->buf[p->len] = '\0';
					}
					mre_resp_complete(p, peer);
					p->state = MRSP_IDLE;
				} else {
					tmp[0] = c; tmp[1] = '\0';
					mre_resp_append(p, tmp, 1);
				}
				break;

			case MRSP_BULK_LEN:
				if (c >= '0' && c <= '9') {
					p->bulk_len = p->bulk_len * 10 + (size_t)(c - '0');
				} else if (c == '-') {
					p->bulk_len = (size_t)-1;
				} else if (c == '\n') {
					char hdr[32];

					if (p->bulk_len == (size_t)-1) {
						mre_resp_append(p, "$-1", sizeof("$-1") - 1);
						mre_resp_complete(p, peer);
						p->state = MRSP_IDLE;
					} else {
						snprintf(hdr, sizeof(hdr), "$%d:", (int)p->bulk_len);
						mre_resp_append(p, hdr, strlen(hdr));
						p->bulk_pos = 0;
						p->state = MRSP_BULK_DATA;
					}
				}
				break;

			case MRSP_BULK_DATA:
				p->bulk_pos++;
				if (p->bulk_pos <= p->bulk_len && p->bulk_pos <= MRE_REDIS_REPLY_MAX) {
					tmp[0] = c; tmp[1] = '\0';
					mre_resp_append(p, tmp, 1);
				}
				if (p->bulk_pos >= p->bulk_len + 2) { /* data + CRLF */
					mre_resp_complete(p, peer);
					p->state = MRSP_IDLE;
				}
				break;

			case MRSP_ARRAY_LEN:
				if (c >= '0' && c <= '9') {
					p->bulk_len = p->bulk_len * 10 + (size_t)(c - '0');
				} else if (c == '\n') {
					if (p->bulk_len == 0) {
						mre_resp_append(p, "*0", sizeof("*0") - 1);
						mre_resp_complete(p, peer);
						p->state = MRSP_IDLE;
					} else if (p->depth < MRE_REDIS_MAX_DEPTH) {
						snprintf(p->hdrs[p->depth], sizeof(p->hdrs[p->depth]),
							"*%d", (int)p->bulk_len);
						mre_resp_append(p, p->hdrs[p->depth],
							strlen(p->hdrs[p->depth]));
						p->counts[p->depth] = (int)p->bulk_len;
						p->depth++;
						p->state = MRSP_IDLE;
					} else {
						mre_resp_append(p, "...", 3);
						mre_resp_complete(p, peer); /* too deep: count as one elem */
						p->state = MRSP_IDLE;
					}
					p->bulk_len = 0;
				}
				break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* instrumented stream ops                                              */
/* ------------------------------------------------------------------ */
#define MRE_WRAPPER_FROM_OPS(ops_ptr) \
	((mre_sockops_wrapper *)(void *)((char *)(ops_ptr) - offsetof(mre_sockops_wrapper, ops)))

static ssize_t mre_sockop_write(php_stream *stream, const char *buf, size_t count)
{
	const php_stream_ops *ops = stream->ops;
	mre_sockops_wrapper *w = MRE_WRAPPER_FROM_OPS(ops);
	ssize_t ret = w->orig->write(stream, buf, count);
	if (ret > 0 && mre_redis_enabled()) {
		mre_req_feed(&w->req, buf, (size_t)ret, w->peer);
	}
	return ret;
}

static ssize_t mre_sockop_read(php_stream *stream, char *buf, size_t count)
{
	const php_stream_ops *ops = stream->ops;
	mre_sockops_wrapper *w = MRE_WRAPPER_FROM_OPS(ops);
	ssize_t ret = w->orig->read(stream, buf, count);

	if (ret > 0 && mre_redis_enabled()) {
		mre_resp_feed(&w->resp, buf, (size_t)ret, w->peer);
	}
	return ret;
}

static int mre_sockop_close(php_stream *stream, int close_handle)
{
	const php_stream_ops *ops = stream->ops;
	mre_sockops_wrapper *w = MRE_WRAPPER_FROM_OPS(ops);
	int ret;

	ret = w->orig->close(stream, close_handle);
	/* restore the original ops first: the engine may still read
	 * stream->ops after close returns, and the wrapper is freed here */
	stream->ops = w->orig;
	pefree(w, w->persistent);
	return ret;
}

static void mre_wrap_stream(php_stream *stream,
                            const char *resourcename, size_t resourcenamelen)
{
	mre_sockops_wrapper *w;
	int persistent = php_stream_is_persistent(stream) ? 1 : 0;
	mre_extra e;

	/* never double-wrap */
	if (stream->ops && stream->ops->label
			&& strcmp(stream->ops->label, "mre_redis") == 0) {
		return;
	}

	w = (mre_sockops_wrapper *)pemalloc(sizeof(mre_sockops_wrapper), persistent);
	memset(w, 0, sizeof(*w));
	w->orig = stream->ops;
	w->persistent = persistent;
	mre_copy_str(w->peer, sizeof(w->peer), resourcename, resourcenamelen);
	mre_req_init(&w->req, (zend_bool)1);
	mre_resp_init(&w->resp, (zend_bool)1);

	w->ops = *stream->ops;
	w->ops.label = "mre_redis";
	w->ops.write = mre_sockop_write;
	w->ops.read = mre_sockop_read;
	w->ops.close = mre_sockop_close;

	stream->ops = &w->ops;

	/* log the connection itself */
	mre_extra_init(&e);
	e.has_port = (zend_bool)1;
	e.port = mre_redis_port_of(resourcename, resourcenamelen);
	e.has_host = (zend_bool)1;
	mre_redis_host_of(e.host, sizeof(e.host), resourcename, resourcenamelen);
	mre_log_write(MRE_TYPE_REDIS, MRE_DIR_REQ, "CONNECT", sizeof("CONNECT") - 1, &e);
}

/* ------------------------------------------------------------------ */
/* tcp transport factory                                                */
/* ------------------------------------------------------------------ */
static php_stream *mre_tcp_factory(const char *proto, size_t protolen,
                                   const char *resourcename, size_t resourcenamelen,
                                   const char *persistent_id, int options, int flags,
                                   struct timeval *timeout,
                                   php_stream_context *context STREAMS_DC)
{
	php_stream *stream;

	if (!mre_orig_tcp_factory) {
		return NULL;
	}
	stream = mre_orig_tcp_factory(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_CC);

	if (stream && mre_redis_enabled()
			&& protolen == 3 && memcmp(proto, "tcp", 3) == 0) {
		int port = mre_redis_port_of(resourcename, resourcenamelen);
		if (port == MRE_G(cfg).redis_port) {
			mre_wrap_stream(stream, resourcename, resourcenamelen);
		}
	}
	return stream;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */
static void mre_redis_install(void)
{
	HashTable *h = php_stream_xport_get_hash();
	php_stream_transport_factory cur;

	if (!h) {
		return;
	}
	cur = (php_stream_transport_factory)zend_hash_str_find_ptr(h, "tcp", 3);
	if (cur == mre_tcp_factory) {
		mre_factory_installed = (zend_bool)1;
		return; /* already ours */
	}
	if (cur != NULL) {
		mre_orig_tcp_factory = cur;
	} else {
		/* no tcp transport registered: fall back to the built-in one */
		mre_orig_tcp_factory = php_stream_generic_socket_factory;
	}
	php_stream_xport_register("tcp", mre_tcp_factory);
	mre_factory_installed = (zend_bool)1;
}

void mre_redis_minit(void)
{
	mre_redis_install();
}

void mre_redis_rehook(void)
{
	/*
	 * Another extension (e.g. openssl loaded after us) may have replaced
	 * the tcp factory after our MINIT. Cheap pointer compare per request.
	 */
	if (!mre_factory_installed) {
		mre_redis_install();
		return;
	}
	{
		HashTable *h = php_stream_xport_get_hash();
		if (h) {
			php_stream_transport_factory cur =
				(php_stream_transport_factory)zend_hash_str_find_ptr(h, "tcp", 3);
			if (cur != mre_tcp_factory) {
				mre_redis_install();
			}
		}
	}
}

void mre_redis_mshutdown(void)
{
	HashTable *h = php_stream_xport_get_hash();

	if (mre_factory_installed && h) {
		php_stream_transport_factory cur =
			(php_stream_transport_factory)zend_hash_str_find_ptr(h, "tcp", 3);
		if (cur == mre_tcp_factory) {
			if (mre_orig_tcp_factory) {
				php_stream_xport_register("tcp", mre_orig_tcp_factory);
			} else {
				php_stream_xport_unregister("tcp");
			}
		}
	}
	mre_factory_installed = (zend_bool)0;
	mre_orig_tcp_factory = NULL;
}
