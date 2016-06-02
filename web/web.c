#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "kiwi.h"
#include "types.h"
#include "config.h"
#include "misc.h"
#include "timer.h"
#include "web.h"
#include "coroutines.h"
#include "mongoose.h"
#include "nbuf.h"
#include "cfg.h"

// Copyright (c) 2014-2016 John Seamons, ZL/KF6VO

user_iface_t user_iface[] = {
	KIWI_UI_LIST
	{0}
};

user_iface_t *find_ui(int port)
{
	user_iface_t *ui = user_iface;
	while (ui->port) {
		if (ui->port == port)
			return ui;
		ui++;
	}
	return NULL;
}

void webserver_connection_cleanup(conn_t *c)
{
	nbuf_cleanup(&c->w2a);
	nbuf_cleanup(&c->a2w);
}


// w2a
// web to app communication (client to server)
//		e.g. SET commands (no return data), and GET/POST for file transfer and commands with return data
// 1) browser => (websocket) => webserver => (mutex memcpy) => app  [half-duplex]
// 2) browser => (GET/POST) => webserver => (file, rx_server_request) => app


extern const char *edata_embed(const char *, size_t *);
extern const char *edata_always(const char *, size_t *);

static const char* edata(const char *uri, size_t *size, char **free_buf)
{
	const char* data = NULL;
	
#ifdef EDATA_EMBED
	// the normal background daemon loads files from in-memory embedded data for speed
	data = edata_embed(uri, size);
#endif

	// some large, seldom-changed files are always loaded from memory
	if (!data) data = edata_always(uri, size);

#ifdef EDATA_EMBED
	// only root-referenced files are opened from filesystem when embedded
	if (uri[0] != '/')
		return data;
#endif

	// to speed edit-copy-compile-debug development, load the files from the local filesystem
	bool free_uri2 = false;
	char *uri2 = (char *) uri;

#ifdef EDATA_DEVEL
	if (uri[0] != '/') {
		asprintf(&uri2, "web/%s", uri);
		free_uri2 = true;
	}
#endif

	// try as a local file
	if (!data) {
		int fd = open(uri2, O_RDONLY);
		if (fd >= 0) {
			struct stat st;
			fstat(fd, &st);
			*size = st.st_size;
			data = (char *) kiwi_malloc("file", *size);
			*free_buf = (char *) data;
			ssize_t rsize = read(fd, (void *) data, *size);
			assert(rsize == *size);
			close(fd);
		}
	}

	if (free_uri2) free(uri2);
	return data;
}

// requests created by data coming in to the web server
static int request(struct mg_connection *mc) {
	int i;
	size_t edata_size=0;
	const char *edata_data;
	char *free_buf = NULL;

	if (mc->is_websocket) {
		// This handler is called for each incoming websocket frame, one or more
		// times for connection lifetime.
		char *s = mc->content;
		int sl = mc->content_len;
		//printf("WEBSOCKET: len %d uri <%s>\n", sl, mc->uri);
		if (sl == 0) {
			//printf("----KA %d\n", mc->remote_port);
			return MG_TRUE;	// keepalive?
		}
		
		conn_t *c = rx_server_websocket(mc, WS_MODE_ALLOC);
		if (c == NULL) {
			s[sl]=0;
			//if (!down) lprintf("rx_server_websocket(alloc): msg was %d <%s>\n", sl, s);
			return MG_FALSE;
		}
		if (c->stop_data) return MG_FALSE;
		
		nbuf_allocq(&c->w2a, s, sl);
		
		if (mc->content_len == 4 && !memcmp(mc->content, "exit", 4)) {
			//printf("----EXIT %d\n", mc->remote_port);
			return MG_FALSE;
		}
		
		return MG_TRUE;
	} else {
		if (strcmp(mc->uri, "/") == 0) mc->uri = "index.html"; else
		if (mc->uri[0] == '/') mc->uri++;
		
		char *ouri = (char *) mc->uri;
		char *uri;
		bool free_uri = FALSE, has_prefix = FALSE;
		
		if (strncmp(ouri, "kiwi/", 5) == 0) {
			uri = ouri;
			has_prefix = TRUE;
		} else
		if (strncmp(ouri, "pkgs/", 5) == 0) {
			uri = ouri;
			has_prefix = TRUE;
		} else
		if (strncmp(ouri, "config/", 7) == 0) {
			asprintf(&uri, "%s/%s", DIR_CFG, &ouri[7]);
			free_uri = TRUE;
			has_prefix = TRUE;
		} else
		if (strncmp(ouri, "kiwi.config/", 12) == 0) {
			asprintf(&uri, "%s/%s", DIR_CFG, &ouri[12]);
			free_uri = TRUE;
			has_prefix = TRUE;
		} else {
			user_iface_t *ui = find_ui(mc->local_port);
			// should never not find match since we only listen to ports in ui table
			assert(ui);
			asprintf(&uri, "%s/%s", ui->name, ouri);
			free_uri = TRUE;
		}
		//printf("---- HTTP: uri %s (%s)\n", ouri, uri);

		// try as file from in-memory embedded data or local filesystem
		edata_data = edata(uri, &edata_size, &free_buf);
		
		// try with ".html" appended
		if (!edata_data) {
			char *uri2;
			asprintf(&uri2, "%s.html", uri);
			if (free_uri) free(uri);
			uri = uri2;
			free_uri = TRUE;
			edata_data = edata(uri, &edata_size, &free_buf);
		}
		
		if (!edata_data && !has_prefix) {
			if (free_uri) free(uri);
			asprintf(&uri, "kiwi/%s", ouri);
			free_uri = TRUE;

			// try as file from in-memory embedded data or local filesystem
			edata_data = edata(uri, &edata_size, &free_buf);
			
			// try with ".html" appended
			if (!edata_data) {
				if (free_uri) free(uri);
				asprintf(&uri, "kiwi/%s.html", ouri);
				free_uri = TRUE;
				edata_data = edata(uri, &edata_size, &free_buf);
			}
		}

		// try as AJAX request
		if (!edata_data) {
			free_buf = (char*) kiwi_malloc("req", NREQ_BUF);
			edata_data = rx_server_request(mc, free_buf, &edata_size);	// mc->uri is ouri without ui->name prefix
			if (!edata_data) { kiwi_free("req", free_buf); free_buf = NULL; }
		}

		if (!edata_data) {
			lprintf("unknown URL: %s (%s) query=%s from %s\n", ouri, uri, mc->query_string, mc->remote_ip);
			return MG_FALSE;
		}
		
		// for index.html process %[substitution]
		// fixme: don't just panic because the config params are bad
		if (strcmp(ouri, "index.html") == 0) {
			static bool index_init;
			static char *index_html, *index_buf;
			static size_t index_size;

#ifdef EDATA_EMBED
			if (!index_init) {		// only have to do once
#else
			if (true) {		// file might change anytime during development
#endif
				if (!index_buf) index_buf = (char*) kiwi_malloc("index_buf", edata_size*3/2);
				char *cp = (char*) edata_data, *np = index_buf, *pp;
				int cl, sl, nl=0, pl;

				config_setting_t *ihp = cfg_lookup("index_html_params", CFG_REQUIRED);
				assert(config_setting_type(ihp) == CONFIG_TYPE_GROUP);

				for (cl=0; cl < edata_size;) {
					if (*cp == '%' && *(cp+1) == '[') {
						cp += 2; cl += 2; pp = cp; pl = 0;
						while (*cp != ']' && cl < edata_size) { cp++; cl++; pl++; }
						cp++; cl++;
						
						const config_setting_t *ihpe;
						for (i=0; (ihpe = config_setting_get_elem(ihp, i)) != NULL; i++) {
							const char *pname = config_setting_name(ihpe);
							assert(pname != NULL);
							const char *pval = config_setting_get_string(ihpe);
							assert(pval != NULL);
							if (strncmp(pp, pname, pl) == 0) {
								sl = strlen(pval);
								strcpy(np, pval); np += sl;
								break;
							}
						}
						if (ihpe == NULL) {
							// not found, put back original
							strcpy(np, "%["); np += 2;
							strncpy(np, pp, pl); np += pl;
							*np++ = ']';
						}
					} else {
						*np++ = *cp++; cl++;
					}
				}
				
				index_html = index_buf;
				index_size = np - index_buf;
				index_init = true;
			}
			edata_data = index_html;
			edata_size = index_size;
		}

		mg_send_header(mc, "Content-Type", mg_get_mime_type(uri, "text/plain"));
		
		// needed by auto-discovery port scanner
		mg_send_header(mc, "Access-Control-Allow-Origin", "*");
		
		mg_send_data(mc, edata_data, edata_size);
		
		if (free_uri) free(uri);
		if (free_buf) kiwi_free("req", free_buf);
		
		http_bytes += edata_size;
		return MG_TRUE;
	}
}

// events created by data coming in to the web server
static int ev_handler(struct mg_connection *mc, enum mg_event ev) {
  int r;
  
  //printf("ev_handler %d:%d len %d ", mc->local_port, mc->remote_port, (int) mc->content_len);
  if (ev == MG_REQUEST) {
  	//printf("MG_REQUEST: URI:%s query:%s\n", mc->uri, mc->query_string);
    r = request(mc);
    //printf("\n");
    return r;
  } else
  if (ev == MG_CLOSE) {
  	//printf("MG_CLOSE\n");
  	rx_server_websocket(mc, WS_MODE_CLOSE);
  	mc->connection_param = NULL;
    return MG_TRUE;
  } else
  if (ev == MG_AUTH) {
  	//printf("MG_AUTH\n");
    return MG_TRUE;
  } else {
  	//printf("MG_OTHER\n");
    return MG_FALSE;
  }
}

int web_to_app(conn_t *c, char *s, int sl)
{
	nbuf_t *nb;
	
	if (c->stop_data) return 0;
	nb = nbuf_dequeue(&c->w2a);
	if (!nb) return 0;
	assert(!nb->done && nb->buf && nb->len);
	memcpy(s, nb->buf, MIN(nb->len, sl));
	nb->done = TRUE;
	//NextTask("w2a");
	
	return nb->len;
}


// a2w
// app to web communication (server to client) e.g. audio, waterfall and MSG data
// app => (mutex memcpy) => webserver => (websocket) => browser  [half-duplex]

void app_to_web(conn_t *c, char *s, int sl)
{
	if (c->stop_data) return;
	nbuf_allocq(&c->a2w, s, sl);
	//NextTask("a2w");
}

// polled send of data to web server
static int iterate_callback(struct mg_connection *mc, enum mg_event ev)
{
	int ret;
	nbuf_t *nb;
	
	if (ev == MG_POLL && mc->is_websocket) {
		conn_t *c = rx_server_websocket(mc, WS_MODE_LOOKUP);
		if (c == NULL)  return MG_FALSE;

		while (TRUE) {
			if (c->stop_data) break;
			nb = nbuf_dequeue(&c->a2w);
			//printf("a2w CHK port %d nb %p\n", mc->remote_port, nb);
			
			if (nb) {
				assert(!nb->done && nb->buf && nb->len);

				#if 1
				// check timing of audio output
				if (c->type == STREAM_SOUND && strncmp(nb->buf, "AUD ", 4) == 0) {
					if (c->spkt == 0) {
						c->sepoch = timer_ms();
					}
					u4_t expected = c->sepoch + (u4_t)((1.0/audio_rate * (512*4) * c->spkt)*1000.0);
					s4_t diff = (s4_t)(timer_ms() - expected);
					if (diff < -30 || diff > 30)
						printf("RX%d %4d %6.3f %f\n", c->rx_channel, c->spkt, (float)diff/1000.0, audio_rate);
					c->spkt++;
				}
				#endif

				//printf("a2w %d WEBSOCKET: %d %p\n", mc->remote_port, nb->len, nb->buf);
				ret = mg_websocket_write(mc, WS_OPCODE_BINARY, nb->buf, nb->len);
				if (ret<=0) printf("$$$$$$$$ socket write ret %d\n", ret);
				nb->done = TRUE;
			} else {
				break;
			}
		}
	} else {
		if (ev != MG_POLL) printf("$$$$$$$$ a2w %d OTHER: %d len %d\n", mc->remote_port, (int) ev, (int) mc->content_len);
	}
	
	//NextTask("web callback");
	
	return MG_TRUE;
}

void web_server(void *param)
{
	user_iface_t *ui = (user_iface_t *) param;
	struct mg_server *server = ui->server;
	const char *err;
	
	while (1) {
		mg_poll_server(server, 0);		// passing 0 effects a poll
		mg_iterate_over_connections(server, iterate_callback);
		TaskSleep(WEB_SERVER_POLL_US);
        TaskStat(TSTAT_INCR|TSTAT_ZERO, 0, 0, 0);
	}
	
	//mg_destroy_server(&server);
}

ddns_t ddns;

// we've seen the ident.me site respond very slowly at times, so do this in a separate task
void dynamic_DNS()
{
	int i, n, status;

	if (!do_dyn_dns)
		return;
		
	ddns.serno = serial_number;
	ddns.port = user_iface[0].port;
	
	char buf[256];
	
	// send private/public ip addrs to registry
	//n = non_blocking_cmd("hostname --all-ip-addresses", buf, sizeof(buf), NULL);
	n = non_blocking_cmd("ip addr show dev eth0 | grep 'inet ' | cut -d ' ' -f 6", buf, sizeof(buf), NULL);
	assert (n > 0);
	n = sscanf(buf, "%[^/]/%d", ddns.ip_pvt, &ddns.netmask);
	assert (n == 2);
	
	//n = non_blocking_cmd("ifconfig eth0", buf, sizeof(buf), NULL);
	n = non_blocking_cmd("cat /sys/class/net/eth0/address", buf, sizeof(buf), NULL);
	assert (n > 0);
	//n = sscanf(buf, "eth0 Link encap:Ethernet HWaddr %17s", ddns.mac);
	n = sscanf(buf, "%17s", ddns.mac);
	assert (n == 1);
	
	n = non_blocking_cmd("curl -s ident.me", buf, sizeof(buf), &status);
	if (n > 0) {
		i = sscanf(buf, "%16s", ddns.ip_pub);
		assert (i == 1);
	}
	
	ddns.valid = true;

	// no Internet access or no serial number available, so no point in registering
	bool noInternet = (status < 0 || WEXITSTATUS(status) != 0);
	if (ddns.serno == 0) lprintf("DDNS: no serial number?\n");
	if (noInternet) lprintf("DDNS: no Internet access?\n");
	if (noInternet || ddns.serno == 0)
		return;
	
	lprintf("DDNS: private ip %s/%d, public ip %s\n", ddns.ip_pvt, ddns.netmask, ddns.ip_pub);

	if (register_on_kiwisdr_dot_com) {
		char *bp;
		asprintf(&bp, "curl -s -o /dev/null http://%s/php/register.php?reg=%d.%s.%d.%s.%d.%s",
			DYN_DNS_SERVER, ddns.serno, ddns.ip_pub, ddns.port, ddns.ip_pvt, ddns.netmask, ddns.mac/*, VERSION_MAJ, VERSION_MIN*/);
		n = system(bp);
		lprintf("registering: <%s> returned %d\n", bp, n);
		free(bp);
	}
}


static void register_SDR_hu()
{
	int n, retrytime_mins=0;
	char *cmd_p;
	
	// reply is a bunch of HTML, buffer has to be big enough not to miss/split status
	#define NBUF 32768
	char *reply = (char *) kiwi_malloc("register_SDR_hu", NBUF);
	
	asprintf(&cmd_p, "wget --timeout=15 -qO- http://sdr.hu/update --post-data \"url=http://%s:%d&apikey=%s\" 2>&1",
		cfg_string("server_url", NULL, CFG_OPTIONAL), user_iface[0].port, cfg_string("api_key", NULL, CFG_OPTIONAL));

	while (1) {
		n = non_blocking_cmd(cmd_p, reply, NBUF/2, NULL);
		//printf("sdr.hu: REPLY <%s>\n", reply);
		char *sp, *sp2;
		if (n > 0 && (sp = strstr(reply, "UPDATE:")) != 0) {
			sp += 7;
			if (strncmp(sp, "SUCCESS", 7) == 0) {
				if (retrytime_mins != 20) lprintf("sdr.hu registration: WORKED\n");
				retrytime_mins = 20;
			} else {
				if ((sp2 = strchr(sp, '\n')) != NULL)
					*sp2 = '\0';
				lprintf("sdr.hu registration: \"%s\"\n", sp);
				retrytime_mins = 2;
			}
		} else {
			lprintf("sdr.hu registration: FAILED n=%d sp=%p\n", n, sp);
			retrytime_mins = 2;
		}
		TaskSleep(MINUTES_TO_SECS(retrytime_mins) * 1000000);
	}
	kiwi_free("register_SDR_hu", reply);
	free(cmd_p);
	#undef NBUF
}

void web_server_init(ws_init_t type)
{
	user_iface_t *ui = user_iface;
	static bool init;
	
	if (!init) {
		nbuf_init();
		init = TRUE;
	}
	
	if (type == WS_INIT_CREATE) {
		// if specified, override the default port number of the first UI
		int port;
		if (alt_port)
			port = alt_port;
		else
			port = cfg_int("port", NULL, CFG_REQUIRED);
		if (port) {
			lprintf("listening on port %d for \"%s\"\n", port, ui->name);
			ui->port = port;
		} else {
			lprintf("listening on default port %d for \"%s\"\n", ui->port, ui->name);
		}
	} else

	if (type == WS_INIT_START) {
		CreateTask(dynamic_DNS, WEBSERVER_PRIORITY);

		if (!down && !alt_port && cfg_bool("sdr_hu_register", NULL, CFG_PRINT) == true)
			CreateTask(register_SDR_hu, WEBSERVER_PRIORITY);
	}

	// create webserver port(s)
	while (ui->port) {
		if (type == WS_INIT_CREATE) {
			ui->server = mg_create_server(NULL, ev_handler);
			char *s_port;
			asprintf(&s_port, "%d", ui->port);
			if (mg_set_option(ui->server, "listening_port", s_port) != NULL) {
				lprintf("network port %d for \"%s\" in use\n", ui->port, ui->name);
				lprintf("app already running in background?\ntry \"make stop\" (or \"m stop\") first\n");
				xit(-1);
			}
			lprintf("webserver for \"%s\" on port %s\n", ui->name, mg_get_option(ui->server, "listening_port"));
			free(s_port);

		} else {	// WS_INIT_START
			CreateTaskP(web_server, WEBSERVER_PRIORITY, ui);
		}
		ui++;
		if (down) break;
	}
}
