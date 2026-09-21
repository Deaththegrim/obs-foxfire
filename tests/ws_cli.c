/* Drives ff-net.c against a real socket, for tools/ws-server-proof.py.
 *
 * A separate binary rather than a unit test because the point is the part that scripted function
 * pointers cannot reach: curl's CONNECT_ONLY socket, a real TCP connection, and bytes arriving
 * when the kernel feels like delivering them. It reports what it saw on stdout, one line per
 * event, and the Python side judges.
 */

#include <ff-net.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: ws_cli <url> [message to send]\n");
		return 2;
	}
	char err[256] = {0};
	struct ff_net *n = ff_net_ws_open(argv[1], err, sizeof err);
	if (!n) {
		printf("REFUSED:%s\n", err);
		fflush(stdout);
		return 0; /* refusing is a RESULT, not a crash: the proof checks which refusals happen */
	}
	printf("OPEN\n");
	fflush(stdout);

	if (argc > 2) {
		if (!ff_ws_conn_send(ff_net_conn(n), FF_WS_TEXT, argv[2], strlen(argv[2])))
			printf("SENDFAIL\n");
		else
			printf("SENT:%s\n", argv[2]);
		fflush(stdout);
	}

	/* ~10s of patience: enough for a local server to run its whole script, short enough that a
	   wedged proof fails rather than hanging the suite. */
	for (int i = 0; i < 1000; i++) {
		struct ff_ws_msg m;
		int r = ff_ws_conn_poll(ff_net_conn(n), &m);
		if (r == 1) {
			printf("MSG:%.*s\n", (int)m.len, (const char *)m.payload);
			fflush(stdout);
		} else if (r < 0) {
			printf("END:%s\n", ff_net_conn(n)->err);
			fflush(stdout);
			break;
		} else {
			ff_net_wait(n, 10);
		}
	}
	ff_net_close(n);
	return 0;
}
