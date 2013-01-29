/* Copyright (C) 2012-2013 Paul Ionkin <paul.ionkin@gmail.com>
 * This file is subject to the terms and conditions defined in
 * file 'LICENSE.txt', which is part of this source code package.
 */
#include "http_connection.h"

/*{{{ struct*/

#define CON_LOG "con"

static void http_connection_on_close (struct evhttp_connection *evcon, void *ctx);

/*}}}*/

/*{{{ create / destroy */
// create HttpConnection object
// establish HTTP connections to 
gpointer http_connection_create (Application *app)
{
    HttpConnection *con;
    int port;
    AppConf *conf;

    con = g_new0 (HttpConnection, 1);
    if (!con) {
        LOG_err (CON_LOG, "Failed to create HttpConnection !");
        return NULL;
    }
    
    conf = application_get_conf (app);
    con->app = app;
    con->bucket_name = g_strdup (application_get_bucket_name (app));

    con->is_acquired = FALSE;

    port = application_get_port (app);
    // if no port is specified, libevent returns -1
    if (port == -1) {
        port = conf->http_port;
    }

    LOG_debug (CON_LOG, "Connecting to %s:%d", 
        application_get_host (app),
        port
    );

    // XXX: implement SSL
    con->evcon = evhttp_connection_base_new (
        application_get_evbase (app),
        application_get_dnsbase (app),
        application_get_host (app),
        port
    );

    if (!con->evcon) {
        LOG_err (CON_LOG, "Failed to create evhttp_connection !");
        return NULL;
    }
    
    if (conf) {
        evhttp_connection_set_timeout (con->evcon, conf->timeout);
        evhttp_connection_set_retries (con->evcon, conf->retries);
    } else {
        evhttp_connection_set_timeout (con->evcon, 60);
        evhttp_connection_set_retries (con->evcon, -1);
    }

    evhttp_connection_set_closecb (con->evcon, http_connection_on_close, con);

    return (gpointer)con;
}

// destory HttpConnection)
void http_connection_destroy (gpointer data)
{
    HttpConnection *con = (HttpConnection *) data;
    
    if (con->evcon)
        evhttp_connection_free (con->evcon);
    g_free (con->bucket_name);
    g_free (con);
}
/*}}}*/

void http_connection_set_on_released_cb (gpointer client, ClientPool_on_released_cb client_on_released_cb, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;

    con->client_on_released_cb = client_on_released_cb;
    con->pool_ctx = ctx;
}

gboolean http_connection_check_rediness (gpointer client)
{
    HttpConnection *con = (HttpConnection *) client;

    return !con->is_acquired;
}

gboolean http_connection_acquire (HttpConnection *con)
{
    con->is_acquired = TRUE;

    return TRUE;
}

gboolean http_connection_release (HttpConnection *con)
{
    con->is_acquired = FALSE;

    if (con->client_on_released_cb)
        con->client_on_released_cb (con, con->pool_ctx);
    
    return TRUE;
}

// callback connection is closed
static void http_connection_on_close (struct evhttp_connection *evcon, void *ctx)
{
    HttpConnection *con = (HttpConnection *) ctx;

    LOG_debug (CON_LOG, "Connection closed !");
}

/*{{{ getters */
Application *http_connection_get_app (HttpConnection *con)
{
    return con->app;
}

struct evhttp_connection *http_connection_get_evcon (HttpConnection *con)
{
    return con->evcon;
}

/*}}}*/

/*{{{ get_auth_string */
// create  auth string
gchar *http_connection_get_auth_string (Application *app, 
        const gchar *method, const gchar *content_type, const gchar *resource, const gchar *time_str)
{
    gchar *string_to_sign;
    unsigned int md_len;
    unsigned char md[EVP_MAX_MD_SIZE];
    gchar *res;
    BIO *bmem, *b64;
    BUF_MEM *bptr;
    int ret;
    gchar *tmp;

    tmp = g_strdup_printf ("/%s%s", application_get_bucket_name (app), resource);

    string_to_sign = g_strdup_printf (
        "%s\n"  // HTTP-Verb + "\n"
        "%s\n"  // Content-MD5 + "\n"
        "%s\n"  // Content-Type + "\n"
        "%s\n"  // Date + "\n" 
        "%s"    // CanonicalizedAmzHeaders
        "%s",    // CanonicalizedResource

        method, "", content_type, time_str, "", tmp
    );

    g_free (tmp);

   // LOG_debug (CON_LOG, "%s", string_to_sign);

    HMAC (EVP_sha1(), 
        application_get_secret_access_key (app),
        strlen (application_get_secret_access_key (app)),
        (unsigned char *)string_to_sign, strlen (string_to_sign),
        md, &md_len
    );
    g_free (string_to_sign);
    
    b64 = BIO_new (BIO_f_base64 ());
    bmem = BIO_new (BIO_s_mem ());
    b64 = BIO_push (b64, bmem);
    BIO_write (b64, md, md_len);
    ret = BIO_flush (b64);
    if (ret != 1) {
        LOG_err (CON_LOG, "Failed to create base64 of auth string !");
        return NULL;
    }
    BIO_get_mem_ptr (b64, &bptr);

    res = g_malloc (bptr->length);
    memcpy (res, bptr->data, bptr->length);
    res[bptr->length - 1] = '\0';

    BIO_free_all (b64);

    return res;
}
/*}}}*/

// create  and setup HTTP connection request
struct evhttp_request *http_connection_create_request (HttpConnection *con,
    void (*cb)(struct evhttp_request *, void *), void *arg,
    const gchar *auth_str)
{    
    struct evhttp_request *req;
    gchar auth_key[300];
    struct tm *cur_p;
	time_t t = time(NULL);
    struct tm cur;
    char date[50];
    //char hostname[1024];

	gmtime_r(&t, &cur);
	cur_p = &cur;

    snprintf (auth_key, sizeof (auth_key), "AWS %s:%s", application_get_access_key_id (con->app), auth_str);

    req = evhttp_request_new (cb, arg);
    evhttp_add_header (req->output_headers, "Authorization", auth_key);
    evhttp_add_header (req->output_headers, "Host", application_get_host_header (con->app));
		
    if (strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", cur_p) != 0) {
			evhttp_add_header (req->output_headers, "Date", date);
		}
    return req;
}


typedef struct {
    HttpConnection *con;
    HttpConnection_responce_cb responce_cb;
    HttpConnection_error_cb error_cb;
    gpointer ctx;
} RequestData;

static void http_connection_on_responce_cb (struct evhttp_request *req, void *ctx)
{
    RequestData *data = (RequestData *) ctx;
    struct evbuffer *inbuf;
    const char *buf = NULL;
    size_t buf_len;

    LOG_debug (CON_LOG, "Got HTTP response from server !");

    if (!req) {
        LOG_err (CON_LOG, "Request failed !");
        if (data->error_cb)
            data->error_cb (data->con, data->ctx);
        goto done;
    }

    // XXX: handle redirect
    // 200 and 204 (No Content) are ok
    if (evhttp_request_get_response_code (req) != 200 && evhttp_request_get_response_code (req) != 204
        && evhttp_request_get_response_code (req) != 307 && evhttp_request_get_response_code (req) != 301) {
        LOG_err (CON_LOG, "Server returned HTTP error: %d !", evhttp_request_get_response_code (req));
        LOG_debug (CON_LOG, "Error str: %s", req->response_code_line);
        if (data->error_cb)
            data->error_cb (data->con, data->ctx);
        goto done;
    }

    inbuf = evhttp_request_get_input_buffer (req);
    buf_len = evbuffer_get_length (inbuf);
    buf = (const char *) evbuffer_pullup (inbuf, buf_len);
    
    if (data->responce_cb)
        data->responce_cb (data->con, data->ctx, buf, buf_len, evhttp_request_get_input_headers (req));
    else
        LOG_debug (CON_LOG, ">>> NO callback function !");

done:
    g_free (data);
}

gboolean http_connection_make_request (HttpConnection *con, 
    const gchar *resource_path, const gchar *request_str,
    const gchar *http_cmd,
    struct evbuffer *out_buffer,
    HttpConnection_responce_cb responce_cb,
    HttpConnection_error_cb error_cb,
    gpointer ctx)
{
    gchar *auth_str;
    struct evhttp_request *req;
    gchar auth_key[300];
	time_t t;
    char time_str[50];
    RequestData *data;
    int res;
    enum evhttp_cmd_type cmd_type;
    AppConf *conf;

    data = g_new0 (RequestData, 1);
    data->responce_cb = responce_cb;
    data->error_cb = error_cb;
    data->ctx = ctx;
    data->con = con;
    
    if (!strcasecmp (http_cmd, "GET")) {
        cmd_type = EVHTTP_REQ_GET;
    } else if (!strcasecmp (http_cmd, "PUT")) {
        cmd_type = EVHTTP_REQ_PUT;
    } else if (!strcasecmp (http_cmd, "DELETE")) {
        cmd_type = EVHTTP_REQ_DELETE;
    } else if (!strcasecmp (http_cmd, "HEAD")) {
        cmd_type = EVHTTP_REQ_HEAD;
    } else {
        LOG_err (CON_LOG, "Unsupported HTTP method: %s", http_cmd);
        return FALSE;
    }
    
    t = time (NULL);
    strftime (time_str, sizeof (time_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t));
    auth_str = http_connection_get_auth_string (con->app, http_cmd, "", resource_path, time_str);
    snprintf (auth_key, sizeof (auth_key), "AWS %s:%s", application_get_access_key_id (con->app), auth_str);
    g_free (auth_str);

    req = evhttp_request_new (http_connection_on_responce_cb, data);
    if (!req) {
        LOG_err (CON_LOG, "Failed to create HTTP request object !");
        return FALSE;
    }

    evhttp_add_header (req->output_headers, "Authorization", auth_key);
    evhttp_add_header (req->output_headers, "Host", application_get_host_header (con->app));
	evhttp_add_header (req->output_headers, "Date", time_str);

    if (out_buffer) {
        evbuffer_add_buffer (req->output_buffer, out_buffer);
    }

    conf = application_get_conf (con->app);
    if (conf->path_style) {
        request_str = g_strdup_printf("/%s%s", application_get_bucket_name (con->app), request_str);
    }

    LOG_debug (CON_LOG, "[%p] bucket: %s path: %s", con, application_get_bucket_name (con->app), request_str);

    res = evhttp_make_request (http_connection_get_evcon (con), req, cmd_type, request_str);

    if (res < 0)
        return FALSE;
    else
        return TRUE;
}    
