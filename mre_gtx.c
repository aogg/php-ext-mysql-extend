/*
  +----------------------------------------------------------------------+
  | mre - cross-fpm global transaction windows (stage B)                 |
  |                                                                      |
  | A window is opened with mre_gtx_begin() (gated by mre.gtx=1).  While |
  | it is open, every mysqlnd connection that enters a transaction       |
  | (explicit BEGIN / START TRANSACTION, or the first statement after    |
  | autocommit=0 - detected through SERVER_STATUS_IN_TRANS) is           |
  | registered into a shared registry file.                              |
  |                                                                      |
  |  - COMMIT of a tracked connection is intercepted: it is NOT sent to  |
  |    the server, the call returns true but the transaction stays open; |
  |    the connection is marked "pending" in the registry.               |
  |  - mre_gtx_rollback() / mre_gtx_commit() only flip a marker in the   |
  |    registry.  Every process holding tracked connections performs the |
  |    real ROLLBACK / COMMIT at its next mysqlnd call entry (query /    |
  |    prepare / execute hooks), then removes its entry; when the last   |
  |    entry is gone the window ends.                                    |
  |  - Entries of dead pids (kill(pid,0)) and windows older than         |
  |    mre.gtx_timeout (default 300s) are cleaned automatically; a timed |
  |    out window degrades to a rollback (safety first).                 |
  |                                                                      |
  | Registry (plain text, self-defined, see docs/设计.md §9):             |
  |     <state_dir>/mre-gtx.reg   data (replaced atomically via rename)  |
  |     <state_dir>/mre-gtx.lock  flock target serialising read-modify-  |
  |                               write cycles (held for microseconds)   |
  |                                                                      |
  | Cost control: the hooks consult a per-process snapshot keyed by      |
  | (mtime,size) - at most one stat() per request per connection.        |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#ifndef PHP_WIN32
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* registry model                                                       */
/* ------------------------------------------------------------------ */
#define MRE_GTX_MAX_CONNS 256

typedef struct {
	char key[128];
	long pid;
	long registered;
	int  pending;
} mre_gtx_conn;

#define MRE_GTX_ST_NONE       0
#define MRE_GTX_ST_ACTIVE     1
#define MRE_GTX_ST_MARK_RB    2
#define MRE_GTX_ST_MARK_CMT   3

typedef struct {
	int   state;
	char  gtx_id[64];
	long  started_at;
	long  timeout;
	long  owner_pid;
	mre_gtx_conn conns[MRE_GTX_MAX_CONNS];
	size_t nconns;
} mre_gtx_reg;

/* registry line format (space separated):
 *   version 1
 *   state active|mark_rollback|mark_commit
 *   gtx_id <hex>
 *   started_at <unix-ts>
 *   timeout <sec>
 *   owner_pid <pid>
 *   conn <key> <pid> <registered-ts> <0|1 pending>
 */
static const char *mre_gtx_state_name(int state)
{
	switch (state) {
		case MRE_GTX_ST_ACTIVE:   return "active";
		case MRE_GTX_ST_MARK_RB:  return "mark_rollback";
		case MRE_GTX_ST_MARK_CMT: return "mark_commit";
		default:                  return "none";
	}
}

static int mre_gtx_state_code(const char *s)
{
	if (!s) {
		return MRE_GTX_ST_NONE;
	}
	if (strcmp(s, "active") == 0)        return MRE_GTX_ST_ACTIVE;
	if (strcmp(s, "mark_rollback") == 0) return MRE_GTX_ST_MARK_RB;
	if (strcmp(s, "mark_commit") == 0)   return MRE_GTX_ST_MARK_CMT;
	return MRE_GTX_ST_NONE;
}

/* ------------------------------------------------------------------ */
/* registry path + snapshot helpers                                     */
/* ------------------------------------------------------------------ */
static void mre_gtx_snapshot_reset(void);

static const char *mre_gtx_reg_path(void)
{
	const mre_config *cfg;
	char expected[MRE_PATH_MAX];

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);
	snprintf(expected, sizeof(expected), "%s/mre-gtx.reg",
		cfg->state_dir[0] ? cfg->state_dir : "/tmp");
	if (strcmp(MRE_G(gtx_reg_path), expected) != 0) {
		/* state dir changed: drop the snapshot with it */
		mre_copy_str(MRE_G(gtx_reg_path), sizeof(MRE_G(gtx_reg_path)),
			expected, strlen(expected));
		mre_gtx_snapshot_reset();
	}
	return MRE_G(gtx_reg_path);
}

static void mre_gtx_snapshot_reset(void)
{
	MRE_G(gtx_snap_valid) = (zend_bool)0;
	MRE_G(gtx_snap_state) = MRE_GTX_ST_NONE;
	MRE_G(gtx_snap_id)[0] = '\0';
	MRE_G(gtx_snap_started) = 0;
	MRE_G(gtx_snap_timeout) = 0;
	MRE_G(gtx_snap_owner) = 0;
	MRE_G(gtx_snap_expired) = (zend_bool)0;
	MRE_G(gtx_snap_nconns) = 0;
}

static void mre_gtx_pid_alive_hint(long pid, zend_bool *alive)
{
#ifndef PHP_WIN32
	if (pid <= 1) {
		*alive = (zend_bool)0; /* pid 0/1 are never tracked workers */
		return;
	}
	if (kill((pid_t)pid, 0) == 0) {
		*alive = (zend_bool)1;
		return;
	}
	*alive = (zend_bool)(errno == EPERM); /* exists, other user: alive */
#else
	(void)pid;
	*alive = (zend_bool)1; /* best effort on Windows: treat as alive */
#endif
}

/* parse the registry text into reg; returns 0 on unusable content */
static int mre_gtx_parse(const char *data, size_t len, mre_gtx_reg *reg)
{
	char line[512];
	size_t ls = 0;

	memset(reg, 0, sizeof(*reg));
	reg->state = MRE_GTX_ST_NONE;

	while (ls < len) {
		size_t le = ls;
		while (le < len && data[le] != '\n') {
			le++;
		}
		if (le - ls < sizeof(line)) {
			char word[32], arg[160];
			char line_key[128];
			long line_pid = 0, line_reg = 0;
			int line_pending = 0;

			memcpy(line, data + ls, le - ls);
			line[le - ls] = '\0';

			if (sscanf(line, "conn %127s %ld %ld %d",
					line_key, &line_pid, &line_reg, &line_pending) == 4
					|| sscanf(line, "conn %127s %ld %ld",
						line_key, &line_pid, &line_reg) == 3) {
				if (reg->nconns < MRE_GTX_MAX_CONNS) {
					mre_gtx_conn *c = &reg->conns[reg->nconns];
					memset(c, 0, sizeof(*c));
					mre_copy_str(c->key, sizeof(c->key),
						line_key, strlen(line_key));
					c->pid = line_pid;
					c->registered = line_reg;
					c->pending = line_pending;
					reg->nconns++;
				}
			} else {
				word[0] = arg[0] = '\0';
				if (sscanf(line, "%31s %159s", word, arg) >= 2) {
					if (strcmp(word, "version") == 0) {
						if (atoi(arg) != 1) {
							return 0; /* unknown registry version */
						}
					} else if (strcmp(word, "state") == 0) {
						reg->state = mre_gtx_state_code(arg);
					} else if (strcmp(word, "gtx_id") == 0) {
						mre_copy_str(reg->gtx_id, sizeof(reg->gtx_id),
							arg, strlen(arg));
					} else if (strcmp(word, "started_at") == 0) {
						reg->started_at = atol(arg);
					} else if (strcmp(word, "timeout") == 0) {
						reg->timeout = atol(arg);
					} else if (strcmp(word, "owner_pid") == 0) {
						reg->owner_pid = atol(arg);
					}
				}
			}
		}
		ls = le + 1;
	}
	return (reg->state != MRE_GTX_ST_NONE);
}

/* read + parse the registry into reg (no lock; rename is atomic) */
static zend_bool mre_gtx_read(const char *path, mre_gtx_reg *reg)
{
	FILE *fp;
	char *data;
	long size;

	fp = fopen(path, "rb");
	if (!fp) {
		return (zend_bool)0;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size < 0 || size > (1 << 20)) {
		fclose(fp);
		return (zend_bool)0;
	}
	data = (char *)malloc((size_t)size + 1);
	if (!data) {
		fclose(fp);
		return (zend_bool)0;
	}
	if (size > 0 && fread(data, 1, (size_t)size, fp) != (size_t)size) {
		free(data);
		fclose(fp);
		return (zend_bool)0;
	}
	fclose(fp);
	data[size] = '\0';

	{
		zend_bool ok = (zend_bool)mre_gtx_parse(data, (size_t)size, reg);
		free(data);
		return ok;
	}
}

/* write reg atomically: tmp file + rename */
static zend_bool mre_gtx_write(const char *path, const mre_gtx_reg *reg)
{
	char tmp[MRE_PATH_MAX + 32];
	FILE *fp;
	size_t i;

	if (reg->state == MRE_GTX_ST_NONE) {
		unlink(path); /* window ended */
		mre_gtx_snapshot_reset();
		return (zend_bool)1;
	}

	snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
	fp = fopen(tmp, "wb");
	if (!fp) {
		return (zend_bool)0;
	}
	fprintf(fp, "version 1\n");
	fprintf(fp, "state %s\n", mre_gtx_state_name(reg->state));
	fprintf(fp, "gtx_id %s\n", reg->gtx_id);
	fprintf(fp, "started_at %ld\n", reg->started_at);
	fprintf(fp, "timeout %ld\n", reg->timeout);
	fprintf(fp, "owner_pid %ld\n", reg->owner_pid);
	for (i = 0; i < reg->nconns; i++) {
		const mre_gtx_conn *c = &reg->conns[i];
		fprintf(fp, "conn %s %ld %ld %d\n",
			c->key, c->pid, c->registered, c->pending ? 1 : 0);
	}
	fflush(fp);
#ifndef PHP_WIN32
	fsync(fileno(fp));
#endif
	fclose(fp);

	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return (zend_bool)0;
	}
	return (zend_bool)1;
}

/* drop entries whose owning process is gone */
static void mre_gtx_purge_dead(mre_gtx_reg *reg)
{
	size_t i, o = 0;

	for (i = 0; i < reg->nconns; i++) {
		zend_bool alive;
		mre_gtx_pid_alive_hint(reg->conns[i].pid, &alive);
		if (alive) {
			if (o != i) {
				reg->conns[o] = reg->conns[i];
			}
			o++;
		}
	}
	reg->nconns = o;
}

/* a mark_* window with no tracked connection left ends immediately */
static void mre_gtx_settle(mre_gtx_reg *reg)
{
	if ((reg->state == MRE_GTX_ST_MARK_RB || reg->state == MRE_GTX_ST_MARK_CMT)
			&& reg->nconns == 0) {
		reg->state = MRE_GTX_ST_NONE;
	}
}

static zend_bool mre_gtx_lock(const char *path, char *lockpath, size_t cap, int *lfd)
{
	snprintf(lockpath, cap, "%s.lock", path);
	*lfd = open(lockpath, O_RDWR | O_CREAT, 0666);
	if (*lfd < 0) {
		return (zend_bool)0;
	}
#ifndef PHP_WIN32
	if (flock(*lfd, LOCK_EX) != 0) {
		close(*lfd);
		return (zend_bool)0;
	}
#endif
	return (zend_bool)1;
}

static void mre_gtx_unlock(int lfd)
{
#ifndef PHP_WIN32
	flock(lfd, LOCK_UN);
#endif
	close(lfd);
}

/*
 * read-modify-write under the registry lock.  mutate() returns 1 when
 * the registry changed (it is written back), 0 when untouched, -1 to
 * force deletion (window end).
 */
typedef int (*mre_gtx_mutator)(mre_gtx_reg *reg, void *ud);

static zend_bool mre_gtx_rmw(mre_gtx_mutator mutate, void *ud)
{
	char lockpath[MRE_PATH_MAX + 32];
	const char *path = mre_gtx_reg_path();
	mre_gtx_reg reg;
	int lfd;
	int rc;
	zend_bool ok = (zend_bool)1;

	if (!mre_gtx_lock(path, lockpath, sizeof(lockpath), &lfd)) {
		return (zend_bool)0;
	}

	if (!mre_gtx_read(path, &reg)) {
		memset(&reg, 0, sizeof(reg));
		reg.state = MRE_GTX_ST_NONE;
	}

	rc = mutate(&reg, ud);
	if (rc > 0) {
		mre_gtx_settle(&reg);
		ok = mre_gtx_write(path, &reg);
	} else if (rc < 0) {
		reg.state = MRE_GTX_ST_NONE;
		unlink(path);
		mre_gtx_snapshot_reset();
	}
	mre_gtx_unlock(lfd);
	return ok;
}

/*
 * Refresh the per-process snapshot.  Cost: one stat() per call; the
 * hooks call this at most once per request per connection (epoch guard)
 * plus once per transaction-control statement.
 */
static int mre_gtx_mutate_clean(mre_gtx_reg *reg, void *ud);

static void mre_gtx_snapshot_refresh(zend_bool allow_mutate)
{
	const char *path = mre_gtx_reg_path();
	struct stat st;
	mre_gtx_reg reg;

	if (stat(path, &st) != 0) {
		/* no registry: no window (or already ended) */
		if (MRE_G(gtx_snap_valid)) {
			mre_gtx_snapshot_reset();
		}
		return;
	}
	if (MRE_G(gtx_snap_valid)
			&& MRE_G(gtx_snap_mtime) == st.st_mtime
			&& MRE_G(gtx_snap_size) == (long)st.st_size) {
		return; /* up to date */
	}

	if (!mre_gtx_read(path, &reg)) {
		mre_gtx_snapshot_reset();
		return;
	}

	MRE_G(gtx_snap_mtime) = st.st_mtime;
	MRE_G(gtx_snap_size) = (long)st.st_size;
	MRE_G(gtx_snap_valid) = (zend_bool)1;
	MRE_G(gtx_snap_state) = reg.state;
	mre_copy_str(MRE_G(gtx_snap_id), sizeof(MRE_G(gtx_snap_id)),
		reg.gtx_id, strlen(reg.gtx_id));
	MRE_G(gtx_snap_started) = reg.started_at;
	MRE_G(gtx_snap_timeout) = reg.timeout;
	MRE_G(gtx_snap_owner) = reg.owner_pid;
	MRE_G(gtx_snap_nconns) = reg.nconns;
	MRE_G(gtx_snap_expired) = (zend_bool)0;

	if (reg.state == MRE_GTX_ST_ACTIVE && reg.started_at > 0) {
		long timeout = reg.timeout > 0 ? reg.timeout : 300;
		if ((long)time(NULL) - reg.started_at > timeout) {
			MRE_G(gtx_snap_expired) = (zend_bool)1;
			if (allow_mutate) {
				/* safety first: a timed out window degrades to rollback */
				mre_gtx_rmw(mre_gtx_mutate_clean, NULL);
				/* re-read so the snapshot reflects the mutation */
				if (mre_gtx_read(path, &reg)) {
					MRE_G(gtx_snap_state) = reg.state;
					MRE_G(gtx_snap_nconns) = reg.nconns;
					MRE_G(gtx_snap_expired) =
						(zend_bool)(reg.state == MRE_GTX_ST_ACTIVE
							&& reg.started_at > 0
							&& (long)time(NULL) - reg.started_at
								> (reg.timeout > 0 ? reg.timeout : 300));
				}
			}
		}
	}
}

/* housekeeping on read access: expire timed-out windows (safety
 * rollback) and drop entries of dead processes */
static int mre_gtx_mutate_clean(mre_gtx_reg *reg, void *ud)
{
	int changed = 0;
	long timeout = reg->timeout > 0 ? reg->timeout : 300;

	if (reg->state == MRE_GTX_ST_ACTIVE && reg->started_at > 0
			&& (long)time(NULL) - reg->started_at > timeout) {
		reg->state = MRE_GTX_ST_MARK_RB;
		changed = 1;
	}
	{
		size_t before = reg->nconns;
		mre_gtx_purge_dead(reg);
		if (reg->nconns != before) {
			changed = 1;
		}
	}
	(void)ud;
	return changed;
}

/* ------------------------------------------------------------------ */
/* connection keys                                                      */
/* ------------------------------------------------------------------ */
#if MRE_HAVE_MYSQLND
static void mre_gtx_build_key(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx)
{
	const char *host;
	char sh[80];
	size_t i, n;

	if (gx->key[0] != '\0') {
		return;
	}
	host = (conn->hostname.s && conn->hostname.l)
		? conn->hostname.s : (conn->host_info ? conn->host_info : "unknown");
	n = strlen(host);
	if (n > sizeof(sh) - 1) {
		n = sizeof(sh) - 1;
	}
	for (i = 0; i < n; i++) {
		char c = host[i];
		sh[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
			? c : '_';
	}
	sh[n] = '\0';
	snprintf(gx->key, sizeof(gx->key), "%s:%u:%llu:%ld",
		sh, conn->port, (unsigned long long)conn->thread_id, (long)getpid());
}
#endif

/* ------------------------------------------------------------------ */
/* registry mutations used by the hooks                                 */
/* ------------------------------------------------------------------ */
typedef struct {
	const char *key;
	long pid;
	int  pending;     /* -1 = remove entry, 0/1 = set pending, 2 = add */
	zend_bool found;
} mre_gtx_mutation;

static int mre_gtx_mutate_conn(mre_gtx_reg *reg, void *ud)
{
	mre_gtx_mutation *m = (mre_gtx_mutation *)ud;
	size_t i;

	switch (m->pending) {
		case 2: /* add (only into a live window) */
			if (reg->state != MRE_GTX_ST_ACTIVE) {
				return 0;
			}
			for (i = 0; i < reg->nconns; i++) {
				if (strcmp(reg->conns[i].key, m->key) == 0) {
					return 0; /* already tracked */
				}
			}
			if (reg->nconns >= MRE_GTX_MAX_CONNS) {
				return 0;
			}
			memset(&reg->conns[reg->nconns], 0, sizeof(mre_gtx_conn));
			mre_copy_str(reg->conns[reg->nconns].key,
				sizeof(reg->conns[reg->nconns].key), m->key, strlen(m->key));
			reg->conns[reg->nconns].pid = m->pid;
			reg->conns[reg->nconns].registered = (long)time(NULL);
			reg->conns[reg->nconns].pending = 0;
			reg->nconns++;
			m->found = (zend_bool)1;
			return 1;

		case -1: /* remove own entry */
			for (i = 0; i < reg->nconns; i++) {
				if (strcmp(reg->conns[i].key, m->key) == 0) {
					size_t j;
					for (j = i; j + 1 < reg->nconns; j++) {
						reg->conns[j] = reg->conns[j + 1];
					}
					reg->nconns--;
					m->found = (zend_bool)1;
					/* dead-pid entries of others are reaped opportunistically */
					mre_gtx_purge_dead(reg);
					return 1;
				}
			}
			return 0;

		default: /* set pending flag */
			for (i = 0; i < reg->nconns; i++) {
				if (strcmp(reg->conns[i].key, m->key) == 0) {
					if (reg->state != MRE_GTX_ST_ACTIVE
							|| reg->conns[i].pending == m->pending) {
						return 0;
					}
					reg->conns[i].pending = m->pending;
					m->found = (zend_bool)1;
					return 1;
				}
			}
			return 0;
	}
}

#if MRE_HAVE_MYSQLND
static void mre_gtx_unregister(mre_gtx_ctx *gx)
{
	mre_gtx_mutation m;

	if (gx->key[0] == '\0') {
		return;
	}
	m.key = gx->key;
	m.pid = (long)getpid();
	m.pending = -1;
	m.found = (zend_bool)0;
	mre_gtx_rmw(mre_gtx_mutate_conn, &m);
}

/* returns 1 when a tracked entry still exists and was marked pending */
static zend_bool mre_gtx_mark_pending(mre_gtx_ctx *gx)
{
	mre_gtx_mutation m;

	if (gx->key[0] == '\0') {
		return (zend_bool)0;
	}
	m.key = gx->key;
	m.pid = (long)getpid();
	m.pending = 1;
	m.found = (zend_bool)0;
	mre_gtx_rmw(mre_gtx_mutate_conn, &m);
	return m.found;
}

static zend_bool mre_gtx_register(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx)
{
	mre_gtx_mutation m;

	mre_gtx_build_key(conn, gx);
	m.key = gx->key;
	m.pid = (long)getpid();
	m.pending = 2;
	m.found = (zend_bool)0;
	mre_gtx_rmw(mre_gtx_mutate_conn, &m);
	return m.found;
}

/* forced finalize (mark_rollback / mark_commit seen at a call entry) */
static void mre_gtx_finalize_conn(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx,
                                  int state)
{
	const char *sql = (state == MRE_GTX_ST_MARK_RB) ? "ROLLBACK" : "COMMIT";
	const mre_config *cfg = &MRE_G(cfg);
	zend_bool ok;
	mre_extra e;

	if (!mre_mysql_raw_send(conn, sql)) {
		/* the real statement failed (e.g. unread result set); still
		 * unregister so the window can end - the server side of this
		 * connection will roll back when it closes. */
		ok = (zend_bool)0;
	} else {
		ok = (zend_bool)1;
	}

	if (cfg->enable && cfg->capture_mysql) {
		char resp[32];

		mre_extra_init(&e);
		if (conn->host_info) {
			e.has_host = (zend_bool)1;
			mre_copy_str(e.host, sizeof(e.host), conn->host_info,
				strlen(conn->host_info));
		}
		e.has_port = (zend_bool)1;
		e.port = (int)conn->port;
		e.has_cmd = (zend_bool)1;
		mre_copy_str(e.cmd, sizeof(e.cmd), "query", 5);
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_REQ, sql, strlen(sql), &e);

		mre_extra_init(&e);
		if (conn->host_info) {
			e.has_host = (zend_bool)1;
			mre_copy_str(e.host, sizeof(e.host), conn->host_info,
				strlen(conn->host_info));
		}
		e.has_port = (zend_bool)1;
		e.port = (int)conn->port;
		if (!ok) {
			e.has_error = (zend_bool)1;
			e.err_no = 1;
			mre_copy_str(e.err_msg, sizeof(e.err_msg),
				"gtx finalize failed", 19);
		}
		snprintf(resp, sizeof(resp), "GTX_%s",
			(state == MRE_GTX_ST_MARK_RB) ? "ROLLBACK" : "COMMIT");
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, resp, strlen(resp), &e);
	}

	mre_gtx_unregister(gx);
	gx->tracked = (zend_bool)0;
	gx->pending = (zend_bool)0;
}
#endif /* MRE_HAVE_MYSQLND */

/* ------------------------------------------------------------------ */
/* mysqlnd hook entries                                                 */
/* ------------------------------------------------------------------ */
#if MRE_HAVE_MYSQLND
zend_bool mre_gtx_pre_call(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx,
                           const char *query, size_t qlen, int qkind)
{
	const mre_config *cfg;
	zend_bool fresh;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);
	if (!cfg->gtx) {
		return (zend_bool)0;
	}

	/* cost control: one registry stat per request per connection; a
	 * transaction-control statement always re-checks (cheap and rare) */
	fresh = (gx->epoch != MRE_G(gtx_epoch));
	if (fresh) {
		gx->epoch = MRE_G(gtx_epoch);
		mre_gtx_snapshot_refresh(1);
	} else if (qkind != MRE_GTX_Q_NONE) {
		mre_gtx_snapshot_refresh(1);
	}

	if (!MRE_G(gtx_snap_valid)) {
		/* window gone (or never existed): drop stale local tracking */
		if (gx->tracked || gx->pending) {
			gx->tracked = (zend_bool)0;
			gx->pending = (zend_bool)0;
		}
		return (zend_bool)0;
	}

	/* expire check without mutation still degrades to rollback */
	if (MRE_G(gtx_snap_state) == MRE_GTX_ST_ACTIVE && MRE_G(gtx_snap_expired)) {
		MRE_G(gtx_snap_state) = MRE_GTX_ST_MARK_RB;
	}

	if (MRE_G(gtx_snap_state) == MRE_GTX_ST_MARK_RB
			|| MRE_G(gtx_snap_state) == MRE_GTX_ST_MARK_CMT) {
		if (gx->tracked || gx->pending) {
			mre_gtx_finalize_conn(conn, gx, MRE_G(gtx_snap_state));
		}
		return (zend_bool)0;
	}

	/* active window: intercept COMMIT of tracked connections */
	if (gx->tracked && (qkind & MRE_GTX_Q_COMMIT)) {
		if (mre_gtx_mark_pending(gx)) {
			gx->pending = (zend_bool)1;
			return (zend_bool)1; /* swallowed: transaction stays open */
		}
		gx->tracked = (zend_bool)0; /* entry vanished; let it pass */
	}
	return (zend_bool)0;
}

void mre_gtx_post_call(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx,
                       zend_bool ok, const char *query, int qkind)
{
	const mre_config *cfg;
	unsigned int server_status;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);
	if (!cfg->gtx || !ok || !conn->upsert_status) {
		return;
	}
	server_status = conn->upsert_status->server_status;

	if (!gx->tracked) {
		zend_bool in_trans =
			(zend_bool)((server_status & SERVER_STATUS_IN_TRANS) != 0);
		if ((in_trans || (qkind & MRE_GTX_Q_BEGIN))
				&& MRE_G(gtx_snap_valid)
				&& MRE_G(gtx_snap_state) == MRE_GTX_ST_ACTIVE
				&& !MRE_G(gtx_snap_expired)) {
			if (mre_gtx_register(conn, gx)) {
				gx->tracked = (zend_bool)1;
				gx->pending = (zend_bool)0;
			}
		}
	} else if (qkind & MRE_GTX_Q_ROLLBACK) {
		/* business rolled back its own transaction: leave the window */
		mre_gtx_unregister(gx);
		gx->tracked = (zend_bool)0;
		gx->pending = (zend_bool)0;
	}
}

void mre_gtx_conn_closed(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx)
{
	const mre_config *cfg;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);
	if (!cfg->gtx) {
		return;
	}
	if (gx->tracked || gx->pending) {
		mre_gtx_unregister(gx);
	}
	gx->tracked = (zend_bool)0;
	gx->pending = (zend_bool)0;
}
#endif /* MRE_HAVE_MYSQLND */

/* ------------------------------------------------------------------ */
/* sql classification                                                   */
/* ------------------------------------------------------------------ */
static zend_bool mre_gtx_word_is(const char *s, size_t len, size_t *pos,
                                 const char *word, size_t word_len)
{
	size_t p = *pos;

	while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'
			|| s[p] == '(' || s[p] == '/')) {
		p++;
	}
	/* skip a C-style line comment */
	if (p + 1 < len && s[p] == '-' && s[p + 1] == '-') {
		while (p < len && s[p] != '\n') {
			p++;
		}
		return mre_gtx_word_is(s, len, &p, word, word_len) ? (*pos = p, 1) : 0;
	}
	if (p + word_len > len) {
		return (zend_bool)0;
	}
	if (strncasecmp(s + p, word, word_len) != 0) {
		return (zend_bool)0;
	}
	/* must not be a longer identifier */
	if (p + word_len < len) {
		char c = s[p + word_len];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '_') {
			return (zend_bool)0;
		}
	}
	*pos = p + word_len;
	return (zend_bool)1;
}

static zend_bool mre_gtx_tail_empty(const char *s, size_t len, size_t pos)
{
	while (pos < len) {
		char c = s[pos];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';') {
			pos++;
			continue;
		}
		if (c == '/' && pos + 1 < len && s[pos + 1] == '*') {
			pos += 2;
			while (pos + 1 < len && !(s[pos] == '*' && s[pos + 1] == '/')) {
				pos++;
			}
			pos = (pos + 2 > len) ? len : pos + 2;
			continue;
		}
		return (zend_bool)0;
	}
	return (zend_bool)1;
}

int mre_gtx_sql_kind(const char *sql, size_t len)
{
	size_t pos = 0;
	int kind = MRE_GTX_Q_NONE;

	if (!sql || len == 0) {
		return MRE_GTX_Q_NONE;
	}
	if (mre_gtx_word_is(sql, len, &pos, "COMMIT", 6)
			&& mre_gtx_tail_empty(sql, len, pos)) {
		return MRE_GTX_Q_COMMIT;
	}
	if (mre_gtx_word_is(sql, len, &pos, "ROLLBACK", 8)
			&& mre_gtx_tail_empty(sql, len, pos)) {
		return MRE_GTX_Q_ROLLBACK;
	}
	pos = 0;
	if (mre_gtx_word_is(sql, len, &pos, "BEGIN", 5)
			&& mre_gtx_tail_empty(sql, len, pos)) {
		return MRE_GTX_Q_BEGIN;
	}
	pos = 0;
	if (mre_gtx_word_is(sql, len, &pos, "START", 5)
			&& mre_gtx_word_is(sql, len, &pos, "TRANSACTION", 11)
			&& mre_gtx_tail_empty(sql, len, pos)) {
		return MRE_GTX_Q_BEGIN;
	}
	(void)kind;
	return MRE_GTX_Q_NONE;
}

/* ------------------------------------------------------------------ */
/* gtx id                                                               */
/* ------------------------------------------------------------------ */
static void mre_gtx_new_id(char *buf, size_t cap)
{
	unsigned char rnd[16];
	size_t i;
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd >= 0) {
		ssize_t r = read(fd, rnd, sizeof(rnd));
		close(fd);
		if (r == (ssize_t)sizeof(rnd)) {
			for (i = 0; i < sizeof(rnd) && (i * 2 + 1) < cap; i++) {
				snprintf(buf + i * 2, 3, "%02x", rnd[i]);
			}
			return;
		}
	}
	snprintf(buf, cap, "%lx%lx", (unsigned long)time(NULL),
		(unsigned long)(getpid() * 2654435761UL));
}

static int mre_gtx_mutate_begin(mre_gtx_reg *reg, void *ud)
{
	zend_bool *created = (zend_bool *)ud;

	if (reg->state != MRE_GTX_ST_NONE) {
		long t = reg->timeout > 0 ? reg->timeout : 300;
		if (reg->state == MRE_GTX_ST_ACTIVE
				&& (long)time(NULL) - reg->started_at <= t) {
			*created = (zend_bool)0;
			return 0; /* a live window already exists */
		}
		if (reg->state != MRE_GTX_ST_ACTIVE && reg->nconns > 0) {
			mre_gtx_purge_dead(reg);
			if (reg->nconns > 0) {
				*created = (zend_bool)0;
				return 0; /* finishing window still has live members */
			}
		}
	}

	{
		long timeout = (long)MRE_G(cfg).gtx_timeout;
		if (timeout <= 0) {
			timeout = 300;
		}
		memset(reg, 0, sizeof(*reg));
		reg->state = MRE_GTX_ST_ACTIVE;
		mre_gtx_new_id(reg->gtx_id, sizeof(reg->gtx_id));
		reg->started_at = (long)time(NULL);
		reg->timeout = timeout;
		reg->owner_pid = (long)getpid();
		reg->nconns = 0;
	}
	*created = (zend_bool)1;
	return 1;
}

typedef struct {
	int state;   /* MRE_GTX_ST_MARK_RB / MRE_GTX_ST_MARK_CMT */
	int result;  /* 0 = no window, 1 = marked / already marked */
} mre_gtx_mark_ud;

static int mre_gtx_mutate_mark(mre_gtx_reg *reg, void *ud)
{
	mre_gtx_mark_ud *mu = (mre_gtx_mark_ud *)ud;

	if (reg->state == MRE_GTX_ST_NONE) {
		mu->result = 0; /* nothing to finalize */
		return 0;
	}
	mre_gtx_purge_dead(reg);
	if (reg->nconns == 0) {
		/* nobody left: end the window right away */
		mu->result = 1;
		return -1;
	}
	mu->result = 1;
	if (reg->state == mu->state) {
		return 0; /* already marked as requested */
	}
	reg->state = mu->state;
	return 1;
}

PHP_FUNCTION(mre_gtx_begin)
{
	const mre_config *cfg;
	zend_bool created = (zend_bool)0;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);
	if (!cfg->gtx) {
		RETURN_FALSE;
	}

	/* make sure the state dir exists before the first registry write
	 * (mre_makedirs creates the parent chain of a file path) */
	mre_makedirs(mre_gtx_reg_path());

	if (!mre_gtx_rmw(mre_gtx_mutate_begin, &created)) {
		RETURN_FALSE;
	}
	mre_gtx_snapshot_reset();
	RETURN_BOOL(created);
}

PHP_FUNCTION(mre_gtx_commit)
{
	const mre_config *cfg;
	mre_gtx_mark_ud mu;
	zend_bool ok;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);
	if (!cfg->gtx) {
		RETURN_FALSE;
	}
	mu.state = MRE_GTX_ST_MARK_CMT;
	mu.result = 0;
	ok = mre_gtx_rmw(mre_gtx_mutate_mark, &mu);
	mre_gtx_snapshot_reset();
	RETURN_BOOL(ok && mu.result == 1);
}

PHP_FUNCTION(mre_gtx_rollback)
{
	const mre_config *cfg;
	mre_gtx_mark_ud mu;
	zend_bool ok;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);
	if (!cfg->gtx) {
		RETURN_FALSE;
	}
	mu.state = MRE_GTX_ST_MARK_RB;
	mu.result = 0;
	ok = mre_gtx_rmw(mre_gtx_mutate_mark, &mu);
	mre_gtx_snapshot_reset();
	RETURN_BOOL(ok && mu.result == 1);
}

PHP_FUNCTION(mre_gtx_status)
{
	const mre_config *cfg;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);

	if (!cfg->gtx) {
		array_init(return_value);
		add_assoc_bool_ex(return_value, "active", sizeof("active") - 1, 0);
		add_assoc_string_ex(return_value, "state", sizeof("state") - 1, "disabled");
		add_assoc_null_ex(return_value, "gtx_id", sizeof("gtx_id") - 1);
		add_assoc_null_ex(return_value, "started_at", sizeof("started_at") - 1);
		add_assoc_long_ex(return_value, "tracked_connections",
			sizeof("tracked_connections") - 1, 0);
		add_assoc_null_ex(return_value, "owner_pid", sizeof("owner_pid") - 1);
		add_assoc_long_ex(return_value, "timeout", sizeof("timeout") - 1,
			cfg->gtx_timeout);
		return;
	}

	/* refresh + expire (safety rollback) through the standard RMW path */
	mre_gtx_rmw(mre_gtx_mutate_clean, NULL);
	mre_gtx_snapshot_refresh(0);

	array_init(return_value);
	add_assoc_bool_ex(return_value, "active", sizeof("active") - 1,
		(zend_bool)(MRE_G(gtx_snap_valid)
			&& MRE_G(gtx_snap_state) == MRE_GTX_ST_ACTIVE
			&& !MRE_G(gtx_snap_expired)));
	add_assoc_string_ex(return_value, "state", sizeof("state") - 1,
		MRE_G(gtx_snap_valid)
			? mre_gtx_state_name(MRE_G(gtx_snap_state)) : "none");
	if (MRE_G(gtx_snap_valid) && MRE_G(gtx_snap_id)[0] != '\0') {
		add_assoc_string_ex(return_value, "gtx_id", sizeof("gtx_id") - 1,
			MRE_G(gtx_snap_id));
		add_assoc_long_ex(return_value, "started_at", sizeof("started_at") - 1,
			MRE_G(gtx_snap_started));
		add_assoc_long_ex(return_value, "owner_pid", sizeof("owner_pid") - 1,
			MRE_G(gtx_snap_owner));
	} else {
		add_assoc_null_ex(return_value, "gtx_id", sizeof("gtx_id") - 1);
		add_assoc_null_ex(return_value, "started_at", sizeof("started_at") - 1);
		add_assoc_null_ex(return_value, "owner_pid", sizeof("owner_pid") - 1);
	}
	add_assoc_long_ex(return_value, "tracked_connections",
		sizeof("tracked_connections") - 1,
		(zend_long)(MRE_G(gtx_snap_valid) ? MRE_G(gtx_snap_nconns) : 0));
	add_assoc_long_ex(return_value, "timeout", sizeof("timeout") - 1,
		cfg->gtx_timeout);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */
void mre_gtx_rinit(void)
{
	MRE_G(gtx_epoch)++;
}
