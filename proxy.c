
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <ev.h>


#include "proxy.h"
#include "utils.h"


static int dtls_send_to_peer(struct dtls_context_t *dtls_ctx,
                             session_t *session, uint8 *data, size_t len)
{
    DBG("%s: len=%lu", __func__, len);
    return 0;
}

static int dtls_read_from_peer(struct dtls_context_t *dtls_ctx,
                          session_t *session, uint8 *data, size_t len)
{
    DBG("%s: len=%lu", __func__, len);
    return 0;
}

static int dtls_event(struct dtls_context_t *dtls_ctx, session_t *dtls_session,
                      dtls_alert_level_t level, unsigned short code)
{
    DBG("%s: alert=%d, code=%u", __func__, level, code);
    return 0;
}

static int get_psk_info(struct dtls_context_t *dtls_ctx, const session_t *session,
                        dtls_credentials_type_t type, const unsigned char *id, size_t id_len,
                        unsigned char *result, size_t result_length)
{
    proxy_context_t *ctx = (proxy_context_t *)dtls_get_app_data(dtls_ctx);
    keystore_item_t *psk;
    ssize_t length;

    DBG("%s: type=%d", __func__, type);
    switch(type)
    {
    case DTLS_PSK_HINT:
        DBG("type=HINT");
        return 0;
    case DTLS_PSK_IDENTITY:
        DBG("type=IDENTITY, id=%s", id);
        if (id_len) {
            DBG("got psk_identity_hint: '%.*s'", (int)id_len, id);
        }

        psk = keystore_find_psk(ctx->keystore, id, id_len, NULL, 0);
        if (!psk) {
            ERR("no PSK identity");
            return dtls_alert_fatal_create(DTLS_ALERT_CLOSE_NOTIFY);
        }

        length = psk_set_identity(psk, result, result_length);
        if (length < 0) {
            ERR("cannot set psk_identity -- buffer too small");
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
        }
        return length;
    case DTLS_PSK_KEY:
        DBG("type=KEY");
        psk = keystore_find_psk(ctx->keystore, NULL, 0, id, id_len);
        if (!psk) {
            ERR("PSK for unknown id requested");
            return dtls_alert_fatal_create(DTLS_ALERT_HANDSHAKE_FAILURE);
        }

        length = psk_set_key(psk, result, result_length);
        if (length < 0) {
            ERR("cannot set psk -- buffer too small");
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
        }

        return length;
    }
    return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
}

static dtls_handler_t dtls_cb = {
  .write = dtls_send_to_peer,
  .read  = dtls_read_from_peer,
  .event = dtls_event,
  .get_psk_info = get_psk_info,
#ifdef DTLS_ECC
  .get_ecdsa_key = NULL,
  .verify_ecdsa_key = NULL
#endif /* DTLS_ECC */
};

int proxy_init(proxy_context_t *ctx,
               const proxy_option_t *opt,
               const proxy_psk_t *psk)
{
    DBG("%s", __func__);
    assert(NULL!=ctx);

    memset(ctx, 0, sizeof(proxy_context_t));
    ctx->options = opt;

    DBG("backend = %s:%s", ctx->options->backend_host, ctx->options->backend_port);
    DBG("listen = %s:%s", ctx->options->listen_host, ctx->options->listen_port);
    DBG("psk = %s:%s", psk->id, psk->key);

    struct sockaddr_in6 listen_addr;
    memset(&listen_addr, 0, sizeof(struct sockaddr_in6));

    if (resolve_address(opt->listen_host, opt->listen_port,
                        (struct sockaddr *)&listen_addr) < 0) {
        ERR("failed to resolve listen address");
        return -1;
    }

    ctx->listen_fd = socket(listen_addr.sin6_family, SOCK_DGRAM, 0);

    if (ctx->listen_fd <= 0) {
        ERR("socket: %s", strerror(errno));
        return -1;
    }

    if (fcntl(ctx->listen_fd, F_SETFL, O_NONBLOCK) < 0) {
        ERR("socket: %s", strerror(errno));
        return -1;
    }

    int on = 1;
    if (setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on) ) < 0) {
        ERR("setsockopt SO_REUSEADDR: %s", strerror(errno));
    }

    on = 1;
  #ifdef IPV6_RECVPKTINFO
    if (setsockopt(ctx->listen_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on) ) < 0) {
  #else /* IPV6_RECVPKTINFO */
    if (setsockopt(ctx->listen_fd, IPPROTO_IPV6, IPV6_PKTINFO, &on, sizeof(on) ) < 0) {
  #endif /* IPV6_RECVPKTINFO */
      ERR("setsockopt IPV6_PKTINFO: %s", strerror(errno));
    }

    if (bind(ctx->listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ERR("bind: %s", strerror(errno));
        return -1;
    }


    dtls_init();
    ctx->dtls_ctx = dtls_new_context(ctx);
    if(NULL == ctx->dtls_ctx) {
        ERR("failed to create dtls context");
        return -1;
    }

    ctx->keystore = new_keystore();
    keystore_item_t *item = keystore_new_psk(NULL, 0, psk->id, strlen(psk->id),
                                             psk->key, strlen(psk->key), 0);
    if ((NULL==ctx->keystore) || (NULL==item)) {
        ERR("failed to create psk keystore");
        return -1;
    }

    keystore_store_item(ctx->keystore, item);
    dtls_set_handler(ctx->dtls_ctx, &dtls_cb);

    return 0;
}

static int dtls_handle_read(struct dtls_context_t *dtls_ctx)
{
    proxy_context_t *ctx = (proxy_context_t *)dtls_get_app_data(dtls_ctx);

    session_t session;
    memset(&session, 0, sizeof(session_t));
    session.size = sizeof(session.addr);

    static uint8 buf[DTLS_MAX_BUF];
    int len = recvfrom(ctx->listen_fd, buf, sizeof(buf), MSG_TRUNC,
                       &session.addr.sa, &session.size);

    if (len < 0) {
        perror("recvfrom");
        return -1;
    } else {
        DBG("got %d bytes from port %d", len,
            ntohs(session.addr.sin6.sin6_port));
        if (sizeof(buf) < len) {
            ERR("packet was truncated (%d bytes lost)\n", len - (int)sizeof(buf));
        }
    }

    return dtls_handle_message(dtls_ctx, &session, buf, len);
}

static void proxy_cb(EV_P_ ev_io *w, int revents)
{
    DBG("%s revents=%X", __func__, revents);
    proxy_context_t *ctx = (proxy_context_t *)w->data;
    static int count = 0;

    DBG("%s fds: %d,%d revents: 0x%02x count: %d",
        __func__, w->fd, ctx->listen_fd, revents, count);
    count++;

    struct sockaddr_storage local_addr;
    socklen_t local_addr_size = sizeof(local_addr);
    int ret = getsockname(ctx->listen_fd, (struct sockaddr *)&local_addr, &local_addr_size);
    if (ret < 0) {
        ERR("getsockname()=%d errno=%d", ret, errno);
        return;
    }

    dtls_handle_read(ctx->dtls_ctx);
}

static void start_listen_io(EV_P_ ev_io *w, proxy_context_t *ctx)
{
    DBG("%s fd=%d", __func__, ctx->listen_fd);
    ev_io_init(w, proxy_cb, ctx->listen_fd, EV_READ);
    w->data = ctx;
    ev_io_start(EV_A_ w);
}

int proxy_loop(proxy_context_t *ctx)
{
    DBG("%s", __func__);

    ev_io proxy_watcher;
    struct ev_loop *loop = ev_default_loop(0);
    start_listen_io(EV_A_ &proxy_watcher, ctx);

    DBG("call libev loop()");
    ev_loop(EV_A_ 0);

    return 0;
}

void proxy_deinit(proxy_context_t *ctx)
{
    DBG("%s", __func__);
    assert(NULL!=ctx);
    if (ctx->listen_fd > 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if(NULL != ctx->dtls_ctx) {
        dtls_free_context(ctx->dtls_ctx);
        ctx->dtls_ctx = NULL;
    }
}
