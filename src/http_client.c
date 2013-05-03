/*  
 * Copyright 2012-2013 Paul Ionkin <paul.ionkin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/
#include "http_client.h"
#include "auth_client.h"
#include "hfs_stats_srv.h"

/*{{{ declaration */

// current HTTP state
typedef enum {
    R_expected_first_line = 0,
    R_expected_headers = 1,
    R_expected_data = 2,
} HttpClientResponseState;

typedef enum {
    C_disconnected = 0,
    C_connecting = 1,
    C_connected = 2,
} HttpClientConnectionState;

// HTTP structure
struct _HttpClient {
    Application *app;
    ConfData *conf;

    struct event_base *evbase;
    struct evdns_base *dns_base;
    
    HttpClientConnectionState connection_state;
    HttpClientResponseState response_state;
    HttpClientRequestMethod method; // HTTP method: GET, PUT, etc

    struct bufferevent *bev;
    gchar *url;
    struct evhttp_uri *http_uri; // parsed URI
    
    // outgoing HTTP request
    GList *l_output_headers;
    struct evbuffer *output_buffer;
    
    // incomming HTTP response
    GList *l_input_headers;
    struct evbuffer *input_buffer;
    
    // HTTP response information
    gint response_code;
    gchar *response_code_line;
    gint major; // HTTP version
    gint minor;
    
    // total expected incoming response data length
    guint64 input_length;
    // read so far
    guint64 input_read;

    // total data to send
    guint64 output_length;
    // data sent so far
    guint64 output_sent;

    // is taken by high level
    gboolean is_acquired;

    // context data for callback functions
    gpointer cb_ctx;
    HttpClient_on_chunk_cb on_chunk_cb;
    HttpClient_on_chunk_cb on_last_chunk_cb;
    HttpClient_on_close_cb on_close_cb;
    HttpClient_on_connection_cb on_connection_cb;

    gpointer pool_ctx;
    ClientPool_on_released_cb client_on_released_cb;

    gchar *s_status;
    guint64 upload_bytes;
    struct timeval start_tv;

    gchar *auth_token;
};

// HTTP header: key, value
typedef struct {
    gchar *key;
    gchar *value;
} HttpClientHeader;

#define HTTP_LOG "http"
#define IDLE "idle"

static void http_client_read_cb (struct bufferevent *bev, void *ctx);
static void http_client_write_cb (struct bufferevent *bev, void *ctx);
static void http_client_event_cb (struct bufferevent *bev, short what, void *ctx);
static gboolean http_client_send_initial_request (HttpClient *http);
static void http_client_free_headers (GList *l_headers);
static gboolean http_client_is_response_code_ok (HttpClient *http);

/*}}}*/

/*{{{ HttpClient create / destroy */

/**
Create HttpClient object.
method: HTTP method: GET, PUT, etc
url: string which contains  server's URL

Returns: new HttpClient object
or NULL if failed
 */
gpointer http_client_create (Application *app)
{
    HttpClient *http;

    http = g_new0 (HttpClient, 1);
    http->app = app;
    http->conf = application_get_conf (app);
    http->evbase = application_get_evbase (app);
    http->dns_base = application_get_dnsbase (app);

    http->is_acquired = FALSE;
  
    // default state
    http->connection_state = C_disconnected;
    
    http->output_buffer = evbuffer_new ();
    http->input_buffer = evbuffer_new ();
    
    http->bev = NULL;
    http->http_uri = NULL;
    http->l_input_headers = NULL;
    http->l_output_headers = NULL;

    http->s_status = g_strdup (IDLE);
    timeval_zero (&http->start_tv);

    http->auth_token = NULL;

    http_client_request_reset (http, TRUE);

    return (gpointer) http;
}

// destroy HttpClient object
void http_client_destroy (gpointer data)
{
    HttpClient *http = (HttpClient *) data;
    
    if (http->bev)
        bufferevent_free (http->bev);
    if (http->http_uri)
        evhttp_uri_free (http->http_uri);
    evbuffer_free (http->output_buffer);
    evbuffer_free (http->input_buffer);
    if (http->l_input_headers)
        http_client_free_headers (http->l_input_headers);
    if (http->l_output_headers)
        http_client_free_headers (http->l_output_headers);
    if (http->response_code_line)
        g_free (http->response_code_line);
    if (http->auth_token)
        g_free (http->auth_token);
    if (http->s_status)
        g_free (http->s_status);
    g_free (http->url);
    g_free (http);
}

// resets all http request values, 
// set initial request state
void http_client_request_reset (HttpClient *http, gboolean free_output_headers)
{   
    http->response_state = R_expected_first_line;

    if (free_output_headers) {
        if (http->l_output_headers)
            http_client_free_headers (http->l_output_headers);
        http->l_output_headers = NULL;
    }

    if (http->l_input_headers)
        http_client_free_headers (http->l_input_headers);
    http->l_input_headers = NULL;

    if (http->url)
        g_free (http->url);
    http->url = NULL;

    if (http->http_uri)
        evhttp_uri_free (http->http_uri);
    http->http_uri = NULL;

    
    if (http->response_code_line)
        g_free (http->response_code_line);
    http->response_code_line = NULL;
    
    evbuffer_drain (http->input_buffer, -1);
    evbuffer_drain (http->output_buffer, -1);

    http->input_length = 0;
    http->input_read = 0;
    http->output_length = 0;
    http->output_sent = 0;
}
/*}}}*/

/*{{{ bufferevent callback functions*/

// outgoing data buffer is sent
static void http_client_write_cb (G_GNUC_UNUSED struct bufferevent *bev, G_GNUC_UNUSED void *ctx)
{
    //HttpClient *http = (HttpClient *) ctx;
    //LOG_debug (HTTP_LOG, "Data sent !");
}

// parse the first HTTP response line
// return TRUE if line is parsed, FALSE if failed
static gboolean http_client_parse_response_line (HttpClient *http, char *line)
{
	char *protocol;
	char *number;
	const char *readable = "";
	int major, minor;
	char ch;
    int n;

	protocol = strsep(&line, " ");
	if (line == NULL)
		return FALSE;
	number = strsep(&line, " ");
	if (line != NULL)
		readable = line;

	http->response_code = atoi (number);
	n = sscanf (protocol, "HTTP/%d.%d%c", &major, &minor, &ch);
	if (n != 2 || major > 1) {
		LOG_err (HTTP_LOG, "Bad HTTP version %s", line);
		return FALSE;
	}
	http->major = major;
	http->minor = minor;

	if (http->response_code == 0) {
		LOG_err (HTTP_LOG, "Bad HTTP response code \"%s\"", number);
		return FALSE;
	}

	if ((http->response_code_line = g_strdup (readable)) == NULL) {
		LOG_err (HTTP_LOG, "Failed to strdup() !");
		return FALSE;
	}

	return TRUE;
}

// read and parse the first HTTP response's line 
// return TRUE if it's sucessfuly read and parsed, or FALSE if failed
static gboolean http_client_parse_first_line (HttpClient *http, struct evbuffer *input_buf)
{
    char *line;
    size_t line_length = 0;

    line = evbuffer_readln (input_buf, &line_length, EVBUFFER_EOL_CRLF);
    // need more data ?
    if (!line || line_length < 1) {
        LOG_debug (HTTP_LOG, "Failed to read the first HTTP response line !");
        return FALSE;
    }

    if (!http_client_parse_response_line (http, line)) {
        LOG_debug (HTTP_LOG, "Failed to parse the first HTTP response line !");
        g_free (line);
        return FALSE;
    }

    g_free (line);
    
    return TRUE;
}

// read HTTP headers
// return TRUE if all haders are read, FALSE if not enough data
static gboolean http_client_parse_headers (HttpClient *http, struct evbuffer *input_buf)
{
    size_t line_length = 0;
    char *line = NULL;
    HttpClientHeader *header;

	while ((line = evbuffer_readln (input_buf, &line_length, EVBUFFER_EOL_CRLF)) != NULL) {
		char *skey, *svalue;
        
        // the last line
		if (*line == '\0') {
			g_free (line);
			return TRUE;
		}

     //   LOG_debug (HTTP_LOG, "HEADER line: %s\n", line);

		svalue = line;
		skey = strsep (&svalue, ":");
		if (svalue == NULL) {
            LOG_debug (HTTP_LOG, "Wrong header data received !");
	        g_free (line);
			return FALSE;
        }

		svalue += strspn (svalue, " ");

        header = g_new0 (HttpClientHeader, 1);
        header->key = g_strdup (skey);
        header->value = g_strdup (svalue);
        http->l_input_headers = g_list_append (http->l_input_headers, header);
        
        if (!strcmp (skey, "Content-Length")) {
            char *endp;
		    http->input_length = evutil_strtoll (svalue, &endp, 10);
		    if (*svalue == '\0' || *endp != '\0') {
                LOG_debug (HTTP_LOG, "Illegal content length: %s", svalue);
                http->input_length = 0;
            }
        }

        g_free (line);        
    }
    LOG_debug (HTTP_LOG, "Wrong header line: %s", line);

    // if we are here - not all headers have been received !
    return FALSE;
}

// a part of input data is received
static void http_client_read_cb (struct bufferevent *bev, void *ctx)
{
    HttpClient *http = (HttpClient *) ctx;
    struct evbuffer *in_buf;

    in_buf = bufferevent_get_input (bev);
    
    if (http->response_state == R_expected_first_line) {
        if (!http_client_parse_first_line (http, in_buf)) {
            g_free (http->response_code_line);
            http->response_code_line = NULL;
            LOG_debug (HTTP_LOG, "More first line data expected !");
            return;
        }
        LOG_debug (HTTP_LOG, "Response:  HTTP %d.%d, code: %d, code_line: %s", 
                http->major, http->minor, http->response_code, http->response_code_line);
        http->response_state = R_expected_headers;
    }

    if (http->response_state == R_expected_headers) {
        if (!http_client_parse_headers (http, in_buf)) {
            LOG_debug (HTTP_LOG, "More headers data expected !");
            http_client_free_headers (http->l_input_headers);
            http->l_input_headers = NULL;
            return;
        }

        // LOG_debug (HTTP_LOG, "ALL headers received !");
        http->response_state = R_expected_data;
    }

    if (http->response_state == R_expected_data) {
        // add input data to the input buffer
        http->input_read += evbuffer_get_length (in_buf);
        evbuffer_add_buffer (http->input_buffer, in_buf);
        // LOG_debug (HTTP_LOG, "INPUT buf: %zd bytes", evbuffer_get_length (http->input_buffer));
        

        // request is fully downloaded
        if (http->input_read >= http->input_length) {
            HfsStatsSrv *stats;

            LOG_debug (HTTP_LOG, "DONE downloading last chunk, in buf size: %zd !", evbuffer_get_length (http->input_buffer));


            stats = application_get_stats_srv (http->app);
            if (stats) {
                hfs_stats_srv_set_storage_srv_status (stats, http->response_code, http->response_code_line);

                hfs_stats_srv_add_up_bytes (stats, http->upload_bytes);
            }

            if (http->s_status)
                g_free (http->s_status);
            http->s_status = g_strdup (IDLE);
            timeval_zero (&http->start_tv);
            http->upload_bytes = 0;


            // inform client that a end of data is received
            if (http->on_last_chunk_cb)
                http->on_last_chunk_cb (http, http->input_buffer, http_client_is_response_code_ok (http), http->cb_ctx);

            //XXX:  rest
            http_client_request_reset (http, TRUE);
            
            
            // inform pool client
            // XXX:
            /*
            if (http->on_request_done_pool_cb)
                http->on_request_done_pool_cb (http, http->pool_cb_ctx);
            */

        // only the part of it
        } else {
            // inform client that a part of data is received
            if (http->on_chunk_cb) {
                http->on_chunk_cb (http, http->input_buffer, http_client_is_response_code_ok (http), http->cb_ctx);
            }
        }
    }
}

// socket event during downloading / uploading
static void http_client_event_cb (G_GNUC_UNUSED struct bufferevent *bev, short what, void *ctx)
{
    HttpClient *http = (HttpClient *) ctx;
    
    LOG_debug (HTTP_LOG, "[http: %p] Disconnection event: %d !", http, what);

    http->connection_state = C_disconnected;
    // XXX: reset

    // inform client that we are disconnected
    if (http->on_close_cb)
        http->on_close_cb (http, http->cb_ctx);
    
    http_client_release (http);
}

// socket event during connection
static void http_client_connection_event_cb (struct bufferevent *bev, short what, void *ctx)
{
    HttpClient *http = (HttpClient *) ctx;

	if (!(what & BEV_EVENT_CONNECTED)) {
        // XXX: reset
        http->connection_state = C_disconnected;
        LOG_msg (HTTP_LOG, "Failed to establish connection !");
        // inform client that we are disconnected
        if (http->on_close_cb)
            http->on_close_cb (http, http->cb_ctx);

        http_client_release (http);
        return;
    }
    
    LOG_debug (HTTP_LOG, "Connected to the server ! %p", http);

    http->connection_state = C_connected;
    evbuffer_drain (bufferevent_get_input (bev), -1);
    evbuffer_drain (bufferevent_get_output (bev), -1);
    http->response_state = R_expected_first_line;
    
    bufferevent_enable (http->bev, EV_READ);
    bufferevent_setcb (http->bev, 
        http_client_read_cb, http_client_write_cb, http_client_event_cb,
        http
    );
    
    // inform client that we are connected
    if (http->on_connection_cb)
        http->on_connection_cb (http, http->cb_ctx);
    
    // send initial request as soon as we are connected
    http_client_send_initial_request (http);
}
/*}}}*/

/*{{{ connection */
// connect to the remote server
static void http_client_connect (HttpClient *http)
{
    int port;
    
    if (http->connection_state == C_connecting) {
        LOG_err (HTTP_LOG, "%p Wrong connection state: %d", http, http->connection_state);
        return;
    }

    if (http->bev)
        bufferevent_free (http->bev);

    if (uri_is_https (http->http_uri)) {
        SSL *ssl;

		ssl = SSL_new (application_get_ssl_ctx (http->app));

		http->bev = bufferevent_openssl_socket_new (
            http->evbase,
            -1,
            ssl,
		    BUFFEREVENT_SSL_CONNECTING,
		    BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS
        );

        // LOG_err (CON_LOG, "Using SSL !");
    } else {
		http->bev = bufferevent_socket_new (
            http->evbase,
            -1,
		    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
    }

    if (!http->bev) {
        LOG_err (HTTP_LOG, "Failed to create HTTP object!");
    }
    // XXX: 
    // bufferevent_set_timeouts (http->bev, 

    port = uri_get_port (http->http_uri);
    
    LOG_debug (HTTP_LOG, "Connecting to %s:%d .. %p",
        evhttp_uri_get_host (http->http_uri),
        port, http
    );

    http->connection_state = C_connecting;
    
    bufferevent_enable (http->bev, EV_WRITE);
    bufferevent_setcb (http->bev, 
        NULL, NULL, http_client_connection_event_cb,
        http
    );

    bufferevent_socket_connect_hostname (http->bev, http->dns_base, 
        AF_UNSPEC,
        evhttp_uri_get_host (http->http_uri),
        port
    );
}

static gboolean http_client_is_connected (HttpClient *http)
{
    return (http->connection_state == C_connected);
}
/*}}}*/

/*{{{ request */
static void http_client_header_free (HttpClientHeader *header)
{
    g_free (header->key);
    g_free (header->value);
    g_free (header);
}

static void http_client_free_headers (GList *l_headers)
{
    GList *l;
    for (l = g_list_first (l_headers); l; l = g_list_next (l)) {
        HttpClientHeader *header = (HttpClientHeader *) l->data;
        http_client_header_free (header);
    }

    g_list_free (l_headers);
}

void http_client_set_output_length (HttpClient *http, guint64 output_length)
{
    http->output_length = output_length;
}

// add an header to the outgoing request
void http_client_add_output_header (HttpClient *http, const gchar *key, const gchar *value)
{
    HttpClientHeader *header;

    header = g_new0 (HttpClientHeader, 1);
    header->key = g_strdup (key);
    header->value = g_strdup (value);

    http->l_output_headers = g_list_append (http->l_output_headers, header);
}

// add a part of output buffer to the outgoing request
void http_client_add_output_data (HttpClient *http, char *buf, size_t size)
{
    http->output_sent += size;

    evbuffer_add (http->output_buffer, buf, size);

    // send date, if we already are connected and received server's response
    if (http->connection_state == C_connected) {
        bufferevent_write_buffer (http->bev, http->output_buffer);
    }
}

// return resonce's header value, or NULL if header not found
const gchar *http_client_get_input_header (HttpClient *http, const gchar *key)
{
    GList *l;
    
    for (l = g_list_first (http->l_input_headers); l; l = g_list_next (l)) {
        HttpClientHeader *header = (HttpClientHeader *) l->data;
        if (!strcmp (header->key, key))
            return header->value;
    }

    return NULL;
}

// return input data length
gint64 http_client_get_input_length (HttpClient *http)
{
    return http->input_length;
}

static const gchar *http_client_method_to_string (HttpClientRequestMethod method)
{
    switch (method) {
        case Method_get:
            return "GET";
        case Method_put:
            return "PUT";
        default:
            return "GET";
    }
}

// create HTTP headers and send first part of buffer
static gboolean http_client_send_initial_request (HttpClient *http)
{
    struct evbuffer *out_buf;
    GList *l;

    // output length must be set !
    /*
    if (!http->output_length) {
        LOG_err (HTTP_LOG, "Output length is not set !");
        return FALSE;
    }
    */

    gettimeofday (&http->start_tv, NULL);
    http->upload_bytes = http->output_length;
    if (http->s_status)
        g_free (http->s_status);
    http->s_status = g_strdup (http_client_method_to_string (http->method));

    out_buf = evbuffer_new ();

    // first line
    evbuffer_add_printf (out_buf, "%s %s HTTP/1.1\r\n", 
        http_client_method_to_string (http->method),
        evhttp_uri_get_path (http->http_uri)
    );

    // host
    // evbuffer_add_printf (out_buf, "Host: %s\r\n",
    //     evhttp_uri_get_host (http->http_uri)
    // );

    // length
    // XXX:
    
    // must be set by the user !!!
   // if (http->output_length > 0) {
        evbuffer_add_printf (out_buf, "Content-Length: %"G_GUINT64_FORMAT"\r\n",
            http->output_length
        );
    //}

    // add headers
    for (l = g_list_first (http->l_output_headers); l; l = g_list_next (l)) {
        HttpClientHeader *header = (HttpClientHeader *) l->data;
        evbuffer_add_printf (out_buf, "%s: %s\r\n",
            header->key, header->value
        );
    }

    // ask to keep connection opened
    evbuffer_add_printf (out_buf, "Connection: %s\r\n", "keep-alive");
    evbuffer_add_printf (out_buf, "Accept-Encoding: %s\r\n", "identify");
    evbuffer_add_printf (out_buf, "X-Auth-Token: %s\r\n", http->auth_token);

    // end line
    evbuffer_add_printf (out_buf, "\r\n");

    // add current data in the output buffer
    LOG_debug (HTTP_LOG, "OUTPUT len: %zd", evbuffer_get_length (http->output_buffer));
    evbuffer_add_buffer (out_buf, http->output_buffer);
    
    http->output_sent += evbuffer_get_length (out_buf);

    LOG_debug (HTTP_LOG, "%s %s  Request is sent !", http_client_method_to_string (http->method), evhttp_uri_get_path (http->http_uri));
   // g_printf ("\n==============================\n%s\n======================\n",
   //     evbuffer_pullup (out_buf, -1));

    // send it
    bufferevent_write_buffer (http->bev, out_buf);

    evbuffer_free (out_buf);

    return TRUE;
}

// connect (if necessary) to the server and send an HTTP request
// internal
gboolean http_client_start_request_ (HttpClient *http, HttpClientRequestMethod method, const gchar *url)
{
    http->method = method;
    
    http->http_uri = evhttp_uri_parse_with_flags (url, 0);
    if (!http->http_uri) {
        LOG_err (HTTP_LOG, "Failed to parse URL string: %s", url);
        return FALSE;
    }
    if (http->url)
        g_free (http->url);
    http->url = g_strdup (url);

    LOG_debug (HTTP_LOG, "Start Req: %s %s", http->url, evhttp_uri_get_path (http->http_uri));

    // connect if it's not
    if (!http_client_is_connected (http)) {
        http_client_connect (http);
    } else {
        LOG_debug (HTTP_LOG, "Already connected");
        return http_client_send_initial_request (http);
    }

    return TRUE;
}

typedef struct {
    HttpClient *http;
    HttpClientRequestMethod method;
    gchar *path;
} ARequest;

// on AuthServer reply
static void http_client_on_auth_data_cb (gpointer ctx, gboolean success, 
    const gchar *auth_token, const gchar *storage_uri)
{
    ARequest *req = (ARequest *) ctx;
    gchar *url;

    if (!success) {
        LOG_err (HTTP_LOG, "Failed to get AuthToken !");
        goto done;
    }

    if (req->http->auth_token) {
        if (strcmp (req->http->auth_token, auth_token)) {
            g_free (req->http->auth_token);
            req->http->auth_token = g_strdup (auth_token);
        }
    } else 
        req->http->auth_token = g_strdup (auth_token);


    url = g_strdup_printf ("%s%s", storage_uri, req->path);

    http_client_start_request_ (req->http, req->method, url);
    g_free (url);

done:
    g_free (req->path);
    g_free (req);
}

// get AuthData and perform HTTP request to StorageURL
gboolean http_client_start_request_to_storage_url (HttpClient *http, HttpClientRequestMethod method, 
    const gchar *path,
    struct evbuffer *out_buffer,
    gpointer ctx
    )
{
    ARequest *req;

    http_client_request_reset (http, FALSE);

    req = g_new0 (ARequest, 1);
    req->http = http;
    req->method = method;
    req->path = g_strdup (path);

    if (out_buffer) {
        http->output_length = evbuffer_get_length (out_buffer);
        evbuffer_add_buffer (http->output_buffer, out_buffer);
    }

    if (ctx)
        http_client_set_cb_ctx (http, ctx);


    auth_client_get_data (application_get_auth_client (http->app), FALSE, http_client_on_auth_data_cb, req);
    
    return TRUE;
}

/*}}}*/

/*{{{*/
// return TRUE if http client is ready to execute a new request
gboolean http_client_check_rediness (gpointer client)
{
    HttpClient *http = (HttpClient *) client;

    return !http->is_acquired;
}

gboolean http_client_acquire (gpointer client)
{
    HttpClient *http = (HttpClient *) client;
    
    http->is_acquired = TRUE;

    return TRUE;
}

gboolean http_client_release (gpointer client)
{
    HttpClient *http = (HttpClient *) client;
    
    http->is_acquired = FALSE;

    if (http->client_on_released_cb)
        http->client_on_released_cb (http, http->pool_ctx);
    
    return TRUE;
}

void http_client_set_on_released_cb (gpointer client, ClientPool_on_released_cb client_on_released_cb, gpointer ctx)
{
    HttpClient *http = (HttpClient *) client;

    http->pool_ctx = ctx;
    http->client_on_released_cb = client_on_released_cb;
}


void http_client_set_cb_ctx (HttpClient *http, gpointer ctx)
{
    http->cb_ctx = ctx;
}

void http_client_set_on_chunk_cb (HttpClient *http, HttpClient_on_chunk_cb on_chunk_cb)
{
    http->on_chunk_cb = on_chunk_cb;
}

void http_client_set_on_last_chunk_cb (HttpClient *http, HttpClient_on_chunk_cb on_last_chunk_cb)
{
    http->on_last_chunk_cb = on_last_chunk_cb;
}

void http_client_set_close_cb (HttpClient *http, HttpClient_on_close_cb on_close_cb)
{
    http->on_close_cb = on_close_cb;
}

void http_client_set_connection_cb (HttpClient *http, HttpClient_on_connection_cb on_connection_cb)
{
    http->on_connection_cb = on_connection_cb;
}

static gboolean http_client_is_response_code_ok (HttpClient *http)
{
    // 200 (Ok), 201 (Created), 202 (Accepted), 204 (No Content) are ok
    
    if (http->response_code == 200 ||
            http->response_code == 201 ||
            http->response_code == 202 ||
            http->response_code == 204 
        )
        return TRUE;
    return FALSE;
}

ClientInfo *http_client_get_info (gpointer client)
{
    HttpClient *http = (HttpClient *) client;
    ClientInfo *info = g_new0 (ClientInfo, 1);
    
    info->con = http;
    info->status = g_strdup (http->s_status);
    if (http->upload_bytes)
        info->bytes = http->upload_bytes;
    timeval_copy (&info->start_tv, &http->start_tv);
    
    return info;
}
/*}}}*/
