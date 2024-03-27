#include "first.h"

#include <stdlib.h>
#include <string.h>
#include <jwt.h>

#include "plugin.h"

#include "log.h"
#include "buffer.h"
#include "request.h"
#include "http_header.h"
#include "mod_auth_api.h"

/**
 * this is an authentication module for jwts
 */


/* plugin config for all request/connections */

typedef struct {
    const buffer *keyfile;
} plugin_config;

typedef struct {
    PLUGIN_DATA;
    plugin_config defaults;
    plugin_config conf;
} plugin_data;


#if 0 /* (needed if module keeps state for request) */

typedef struct {
    size_t foo;
} handler_ctx;

__attribute_returns_nonnull__
static handler_ctx * handler_ctx_init(void) {
    return ck_calloc(1, sizeof(handler_ctx));
}

static void handler_ctx_free(handler_ctx *hctx) {
    free(hctx);
}

#endif

static handler_t mod_auth_check_bearer(request_st *r, void *p_d, const struct http_auth_require_t *require, const struct http_auth_backend_t *backend);

static handler_t mod_authn_jwt_bearer(request_st *r, void *p_d, const http_auth_require_t *require, const buffer *token, const char *);

/* init the plugin data */
INIT_FUNC(mod_authn_jwt_init) {
    static http_auth_scheme_t http_auth_scheme_bearer =
        { "bearer", mod_auth_check_bearer, NULL };

    /* NOTE Since http_auth_backend_t is limited to basic and digest handlers,
     * the bearer handler will just be assigned as the "basic" handler. It's
     * implementation will assume that the "user" parameter will be a token */
    static http_auth_backend_t http_auth_backend_jwt =
        { "jwt", mod_authn_jwt_bearer, NULL, NULL };

    plugin_data *p = ck_calloc(1, sizeof(plugin_data));

    /* register bearer scheme */
    http_auth_scheme_bearer.p_d = p;
    http_auth_scheme_set(&http_auth_scheme_bearer);

    /* register jwt backend */
    http_auth_backend_jwt.p_d = p;
    http_auth_backend_set(&http_auth_backend_jwt);

    return p;
}

/* handle plugin config and check values */

static void mod_authn_jwt_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* auth.backend.jwt.keyfile */
        pconf->keyfile = cpv->v.b;
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_authn_jwt_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_authn_jwt_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_authn_jwt_patch_config(request_st * const r, plugin_data * const p) {
    p->conf = p->defaults; /* copy small struct instead of memcpy() */
    /*memcpy(&p->conf, &p->defaults, sizeof(plugin_config));*/
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(r, (uint32_t)p->cvlist[i].k_id))
            mod_authn_jwt_merge_config(&p->conf, p->cvlist+p->cvlist[i].v.u2[0]);
    }
}

SETDEFAULTS_FUNC(mod_authn_jwt_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("auth.backend.jwt.keyfile"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_authn_jwt"))
        return HANDLER_ERROR;

    /* process and validate config directives
     * (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1]; i < p->nconfig; ++i) {
        config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
                case 0: /* auth.backend.jwt.keyfile */
                    if (buffer_is_blank(cpv->v.b))
                        cpv->v.b = NULL;
                    break;
                default:/* should not happen */
                    break;
            }
        }
    }

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_authn_jwt_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}

/*
 * auth schemes
 */

__attribute_cold__
__attribute_noinline__
static handler_t
mod_auth_send_400_bad_request (request_st * const r)
{
    /* a field was missing or invalid */
    r->http_status = 400; /* Bad Request */
    r->handler_module = NULL;
    return HANDLER_FINISHED;
}

__attribute_noinline__
static handler_t
mod_auth_send_401_unauthorized_bearer(request_st * const r, const buffer * const realm)
{
    log_notice(r->conf.errh, __FILE__, __LINE__, "Unauthorized bearer");

    r->http_status = 401;
    r->handler_module = NULL;

    /* TODO See [RFC-6750 3.1](https://datatracker.ietf.org/doc/html/rfc6750#section-3.1) */

    /* TODO *MAY* include a realm */
    /* TODO *SHOULD* include error, description, and uri */

    buffer_append_str3(
            http_header_response_set_ptr(r, HTTP_HEADER_WWW_AUTHENTICATE,
            CONST_STR_LEN("WWW-Authenticate")),
            CONST_STR_LEN("Bearer realm=\""),
            BUF_PTR_LEN(realm),
            CONST_STR_LEN("\", charset=\"UTF-8\""));
    return HANDLER_FINISHED;
}

__attribute_cold__
static handler_t
mod_auth_bearer_misconfigured (request_st * const r, const struct http_auth_backend_t * const backend)
{
    if (NULL == backend)
        log_error(r->conf.errh, __FILE__, __LINE__,
          "auth.backend not configured for %s", r->uri.path.ptr);
    else
        log_error(r->conf.errh, __FILE__, __LINE__,
          "auth.require \"method\" => \"...\" is invalid "
          "(try \"bearer\"?) for %s", r->uri.path.ptr);

    r->http_status = 500;
    r->handler_module = NULL;
    return HANDLER_FINISHED;
}

static handler_t
mod_auth_check_bearer(request_st *r, void *p_d, const struct http_auth_require_t *require, const struct http_auth_backend_t *backend)
{
    if (backend == NULL || backend->basic == NULL)
        return mod_auth_bearer_misconfigured(r, backend);

    /* Parse token from authorization header */
    const buffer * const vb =
        http_header_request_get(r, HTTP_HEADER_AUTHORIZATION,
                CONST_STR_LEN("Authorization"));

    if (NULL == vb || !buffer_eq_icase_ssn(vb->ptr, CONST_STR_LEN("Bearer ")))
        return mod_auth_send_401_unauthorized_bearer(r, require->realm);

    size_t ulen = buffer_clen(vb) - (sizeof("Bearer ")-1);
    char token[2048];

    if (ulen > 2047) {
        log_error(r->conf.errh, __FILE__, __LINE__, "Token too large");
        return mod_auth_send_401_unauthorized_bearer(r, require->realm);
    }

    strcpy(token, vb->ptr+sizeof("Bearer ")-1);

    /* TODO Here is where we can do authentication caching */

    const buffer tokenb = { token, ulen+1, 0 };
    handler_t rc = backend->basic(r, backend->p_d, require, &tokenb, "");

    switch (rc) {
        case HANDLER_GO_ON:
        case HANDLER_WAIT_FOR_EVENT:
        case HANDLER_FINISHED:
            break;
        default:
            /* TODO
             * - include "error" attribute and give correct codes
             *   - invalid_request
             *   - invalid_token
             *   - insufficient_scope
             * - include "error_description" attribute (developer-readable)
             * - include "error_uri" attribute (human-readable)
             *
             * We probably need to check plugin data or similar for this operation
             */
            rc = mod_auth_send_401_unauthorized_bearer(r, require->realm);
            break;
    }

    return rc;
}

handler_t mod_authn_jwt_bearer(request_st *r, void *p_d, const http_auth_require_t *require, const buffer *token, const char *)
{
    plugin_data *p = (plugin_data *)p_d;

    mod_authn_jwt_patch_config(r, p);

    handler_t rc = HANDLER_ERROR;

    /* Read token into jwt_t */
    jwt_t *jwt = NULL;
    size_t keylength;
    unsigned char key[10240];
    char *keyhandle = NULL;
    const buffer *keyfile = p->conf.keyfile;

    if (keyfile) {
        FILE *fp_pub_key = fopen(p->conf.keyfile->ptr, "r");

        if (fp_pub_key) {
            keylength = fread(key, 1, sizeof(key), fp_pub_key);
            fclose(fp_pub_key);
            key[keylength] = '\0';
            keyhandle = key;
            log_notice(r->conf.errh, __FILE__, __LINE__, "pub key loaded %s (%zu)", keyfile->ptr, keylength);
        } else {
            log_error(r->conf.errh, __FILE__, __LINE__, "could not open %s", keyfile->ptr);
            return HANDLER_ERROR;
        }
    }

    if (0 != jwt_decode(&jwt, token->ptr, keyhandle, keylength) || jwt == NULL) {
        log_error(r->conf.errh, __FILE__, __LINE__, "Failed to decode jwt: %s", token->ptr);
        goto jwt_finish;
    }

    log_notice(r->conf.errh, __FILE__, __LINE__, "Valid JWT: %s", token->ptr);
    rc = HANDLER_GO_ON;

jwt_finish:
    jwt_free(jwt);

    return rc;
}

/* this function is called at dlopen() time and inits the callbacks */
__attribute_cold__
__declspec_dllexport__
int mod_authn_jwt_plugin_init(plugin *p);
int mod_authn_jwt_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = "authn_jwt";
	p->init        = mod_authn_jwt_init;
	p->set_defaults= mod_authn_jwt_set_defaults;

	return 0;
}
