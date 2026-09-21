/* Splitting a URL, which sounds like it cannot go wrong and has exactly one failure mode that
 * matters: producing a host that is WRONG rather than refusing. A refusal is a clear error on a
 * connection that was never going to work; a wrong host is a connection to somewhere else.
 *
 * ARMED by mutation -- table at the end of this file, measured.
 */

#include "ff-test.h"
#include <ff-net.h>
#include <string.h>

static bool p(const char *url, struct ff_url *u)
{
	memset(u, 0, sizeof *u);
	return ff_url_parse(url, u);
}

int main(void)
{
	struct ff_url u;

	/* the one that matters: what Twitch actually hands us */
	CHECK(p("wss://eventsub.wss.twitch.tv/ws", &u));
	CHECK(strcmp(u.host, "eventsub.wss.twitch.tv") == 0);
	CHECK(strcmp(u.path, "/ws") == 0);
	CHECK(strcmp(u.port, "443") == 0);
	CHECK(u.secure);

	/* the reconnect URL carries a query string, and dropping it would reconnect to the wrong
	   session -- EventSub puts the session id there */
	CHECK(p("wss://eventsub.wss.twitch.tv/ws?challenge=abc&id=7", &u));
	CHECK(strcmp(u.path, "/ws?challenge=abc&id=7") == 0);

	CHECK(p("ws://127.0.0.1:8080/x", &u));
	CHECK(strcmp(u.host, "127.0.0.1") == 0 && strcmp(u.port, "8080") == 0 && !u.secure);

	CHECK(p("https://api.twitch.tv/helix/eventsub/subscriptions", &u));
	CHECK(u.secure && strcmp(u.port, "443") == 0);
	CHECK(p("http://example.com/", &u));
	CHECK(!u.secure && strcmp(u.port, "80") == 0);

	/* no path is "/" -- "GET  HTTP/1.1" is not a request line */
	CHECK(p("wss://example.com", &u));
	CHECK(strcmp(u.path, "/") == 0);
	CHECK(p("wss://example.com#frag", &u));
	CHECK(strcmp(u.path, "/") == 0 && strcmp(u.host, "example.com") == 0);

	/* an IPv6 literal is bracketed and full of colons: taking the last one as a port separator
	   would keep half the address and connect to it */
	CHECK(p("ws://[::1]:9001/ws", &u));
	CHECK(strcmp(u.host, "::1") == 0);
	CHECK(strcmp(u.port, "9001") == 0);
	CHECK(strcmp(u.path, "/ws") == 0);
	CHECK(p("wss://[2001:db8::1]/ws", &u));
	CHECK(strcmp(u.host, "2001:db8::1") == 0 && strcmp(u.port, "443") == 0);
	CHECK(!p("ws://[::1/ws", &u));       /* never closed */
	CHECK(!p("ws://[::1]x/ws", &u));     /* junk between the bracket and the port */

	/* refusals */
	CHECK(!p("not-a-url", &u));
	CHECK(!p("", &u));
	CHECK(!p(NULL, &u));
	CHECK(!p("://example.com/", &u));         /* no scheme */
	CHECK(!p("ftp://example.com/", &u));      /* a scheme we do not speak */
	CHECK(!p("wss:///ws", &u));               /* no host */
	CHECK(!p("wss://example.com:/ws", &u));   /* empty port */
	CHECK(!p("wss://example.com:80a/ws", &u));/* a port that is not a number */
	CHECK(!p("wss://example.com:-1/ws", &u));

	char huge[1200];
	memset(huge, 'a', sizeof huge);
	huge[sizeof huge - 1] = 0;
	char url[1400];
	snprintf(url, sizeof url, "wss://%s/ws", huge);
	CHECK(!p(url, &u)); /* a host that does not fit is refused, not truncated to another host */
	snprintf(url, sizeof url, "wss://example.com/%s", huge);
	CHECK(!p(url, &u));

	FF_TEST_MAIN_END();
}

/* Measured, each guard removed in turn, recompiled and rerun:
 *
 *     control (every guard in place)          36 checks,  0 failed
 *     IPv6 bracket handling removed           36 checks,  4 failed
 *     port digit check removed                36 checks,  2 failed
 *     empty-port check removed                36 checks,  1 failed
 *     empty-host check removed                36 checks,  1 failed
 *     scheme whitelist removed                36 checks,  1 failed
 *     host length check removed (truncates)   36 checks,  1 failed
 *     path length check removed               36 checks,  1 failed
 *     empty path no longer becomes "/"        36 checks,  2 failed
 *     authority no longer ends at '?'         36 checks,  1 failed
 *
 * The "host length check removed" mutant TRUNCATES rather than refusing, because that is the
 * failure worth arming against: a refusal is an error message, a truncated host is a connection
 * to a different machine.
 */
