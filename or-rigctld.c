/* Copyright (c) 2014 OpenHam
 * Developers:
 * Stephen Hurd (K6BSD/VE5BSD) <shurd@FreeBSD.org>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice, developer list, and this permission notice shall
 * be included in all copies or substantial portions of the Software. If you meet
 * us some day, and you think this stuff is worth it, you can buy us a beer in
 * return
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <string.h>

#include <api.h>
#include <iniparser.h>

struct rig_entry {
	struct rig			*rig;
	struct rig_entry	*next_rig_entry;
	struct rig_entry	*prev_rig_entry;
};
struct rig_entry	*rigs = NULL;

struct listener {
	struct rig		*rig;
	int				socket;
	struct listener	*next_listener;
	struct listener	*prev_listener;
};
struct listener		*listeners = NULL;

struct connection {
	int					socket;
	struct rig			*rig;
	char				*rx_buf;
	size_t				rx_buf_size;
	size_t				rx_buf_pos;
	char				*tx_buf;
	size_t				tx_buf_size;
	size_t				tx_buf_pos;
	struct connection	*next_connection;
	struct connection	*prev_connection;
};
struct connection	*connections = NULL;

int add_rig(dictionary *d, char *section)
{
	char				*port;
	char				*addr;
	struct addrinfo		hints, *res, *res0;
	struct listener 	*ret;
	struct rig			*rig;
	int					listener_count = 0;
	struct rig_entry	*entry;
	struct listener		*listener;

	addr = getstring(d, section, "rigctld_address", NULL);
	if (addr == NULL)
		return 0;
	port = getstring(d, section, "rigctld_port", "4532");
	entry = (struct rig_entry *)calloc(1, sizeof(struct rig_entry));
	if (entry == NULL)
		return 0;
	entry->rig = init_rig(d, section);
	if (entry->rig == NULL) {
		free(entry);
		return 0;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_ADDRCONFIG|AI_PASSIVE;
	if (getaddrinfo(addr, port, &hints, &res0) != 0) {
		close_rig(entry->rig);
		free(entry);
		return 0;
	}
	for (res = res0; res; res = res->ai_next) {
		listener = (struct listener *)calloc(1, sizeof(struct listener));
		listener->socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (listener->socket == -1) {
			free(listener);
			continue;
		}
		if (fcntl(listener->socket, F_SETFL, O_NONBLOCK) == -1) {
			close(listener->socket);
			free(listener);
			continue;
		}
		if (bind(listener->socket, res->ai_addr, res->ai_addrlen) < 0) {
			close(listener->socket);
			free(listener);
			continue;
		}
		listen(listener->socket, 5);
		listener->rig = entry->rig;
		listener->next_listener = listeners;
		listeners = listener;
		listener_count++;
	}
	if (listener_count) {
		entry->next_rig_entry = rigs;
		if (rigs)
			rigs->prev_rig_entry = entry;
		rigs = entry;
		return listener_count;
	}
	close_rig(entry->rig);
	free(entry);
	return 0;
}

void close_connection(struct connection *c)
{
	close(c->socket);
	if (c->tx_buf)
		free(c->tx_buf);
	if (c->rx_buf)
		free(c->rx_buf);
	if (c->prev_connection)
		c->prev_connection->next_connection = c->next_connection;
	else
		connections = c->next_connection;
	if (c->next_connection)
		c->next_connection->prev_connection = c->prev_connection;
	free(c);
}

void tx_append(struct connection *c, const char *str)
{
	char	*buf;
	size_t	len = strlen(str);

	if ((c->tx_buf_size - c->tx_buf_pos) < len) {
		buf = (char *)realloc(c->tx_buf, c->tx_buf_size + len);
		if (buf == NULL)
			return;
		c->tx_buf = buf;
		c->tx_buf_size += len;
	}
	memcpy(c->tx_buf + c->tx_buf_pos, str, len);
}

void tx_printf(struct connection *c, const char *format, ...)
{
	va_list	args;
	char	*buf;
	int		ret;

	va_start(args, format);
	ret = vasprintf(&buf, format, args);
	if (ret == -1)
		tx_append(c, "RPRT -1\n");
	else {
		tx_append(c, buf);
		free(buf);
	}
}

void tx_rprt(struct connection *c, int ret)
{
	char *buf;

	if (ret > 0)
		ret = 0-ret;
	if (asprintf(&buf, "RPRT %d\n", ret) != -1) {
		tx_append(c, buf);
		free(buf);
	}
	else
		tx_append(c, "RPRT -1\n");
}

void handle_command(struct connection *c, size_t len)
{
	char			*cmd = strdup(c->rx_buf);
	char			*arg;
	size_t			remain = c->rx_buf_size - (c->rx_buf_pos + len + 1);
	uint64_t		u64;
	unsigned		u;
	int				i;
	char			*buf;
	int				ret;
	enum rig_modes	mode;
	enum vfos		vfo;

	buf = strchr(cmd, '\r');
	if (buf)
		*buf = 0;
fprintf(stderr, "Got command: '%s'\n", cmd);

	// First, clean up the buffer...
	if (remain) {
		memmove(c->rx_buf, c->rx_buf + len + 1, c->rx_buf_pos - len);
		c->rx_buf_pos = 0;
	}
	else {
		free(c->rx_buf);
		c->rx_buf_pos = 0;
		c->rx_buf_size = 0;
		c->rx_buf = NULL;
	}
	// Now handle the command...
	if ((strncmp(cmd, "F ", 2) == 0) || (strncmp(cmd, "set_freq ", 9) == 0)) {
		arg = strchr(cmd, ' ');
		arg++;
		if (sscanf(arg, "%"SCNu64, &u64) != 1)
			tx_append(c, "RPRT -1\n");
		else
			tx_rprt(c, set_frequency(c->rig, u64));
	}
	else if ((strcmp(cmd, "f") == 0) || (strcmp(cmd, "get_freq") == 0)) {
		u64 = get_frequency(c->rig);
		if (u64 == 0)
			tx_append(c, "RPRT -1\n");
		else
			tx_printf(c, "%"PRIu64"\n", u64);
	}
	else if ((strncmp(cmd, "M ", 2) == 0) || (strncmp(cmd, "set_mode ", 9) == 0) || (strncmp(cmd, "X ", 2) == 0) || (strncmp(cmd, "set_split_mode ", 15) == 0)) {
		arg = strchr(cmd, ' ');
		arg++;
		mode = MODE_UNKNOWN;
		if (strncmp(arg, "USB ", 4)==0)
			mode = MODE_USB;
		else if (strncmp(arg, "LSB ", 4)==0)
			mode = MODE_LSB;
		else if (strncmp(arg, "CW ", 3)==0)
			mode = MODE_CW;
		else if (strncmp(arg, "CWR ", 4)==0)
			mode = MODE_CWR;
		else if (strncmp(arg, "RTTY ", 5)==0)
			mode = MODE_FSK;
		else if (strncmp(arg, "AM ", 3)==0)
			mode = MODE_AM;
		else if (strncmp(arg, "FM ", 3)==0)
			mode = MODE_FM;
		if (mode == MODE_UNKNOWN)
			tx_append(c, "RPRT -1\n");
		else
			tx_rprt(c, set_mode(c->rig, mode));
	}
	else if ((strcmp(cmd, "m") == 0) || (strcmp(cmd, "get_mode") == 0) || (strcmp(cmd, "x") == 0) || (strcmp(cmd, "get_split_mode") == 0)) {
		mode = get_mode(c->rig);
		switch (mode) {
			case MODE_USB:
				buf="USB";
				break;
			case MODE_LSB:
				buf="LSB";
				break;
			case MODE_CW:
				buf="CW";
				break;
			case MODE_CWR:
				buf="CWR";
				break;
			case MODE_FSK:
				buf="RTTY";
				break;
			case MODE_AM:
				buf="AM";
				break;
			case MODE_FM:
				buf="FM";
				break;
			default:
				buf=NULL;
				break;
		}
		if (buf == NULL)
			tx_append(c, "RPRT -1\n");
		else
			tx_printf(c, "%s\n0\n", buf);
	}
	else if ((strncmp(cmd, "V ", 2) == 0) || (strncmp(cmd, "set_vfo ", 8) == 0)) {
		arg = strchr(cmd, ' ');
		arg++;
		vfo = VFO_UNKNOWN;
		if (strcmp(arg, "VFOA")==0)
			vfo = VFO_A;
		else if (strcmp(arg, "VFOB")==0)
			vfo = VFO_B;
		else if (strcmp(arg, "MEM")==0)
			vfo = VFO_MEMORY;
		if (vfo == VFO_UNKNOWN)
			tx_append(c, "RPRT -1\n");
		else
			tx_rprt(c, set_vfo(c->rig, vfo));
	}
	else if ((strcmp(cmd, "v") == 0) || (strcmp(cmd, "get_vfo") == 0)) {
		vfo = get_vfo(c->rig);
		switch(vfo) {
			case VFO_A:
				buf = "VFOA";
				break;
			case VFO_B:
				buf = "VFOB";
				break;
			case VFO_MEMORY:
				buf = "MEM";
				break;
			default:
				buf = NULL;
				break;
		}
		if (buf == NULL)
			tx_append(c, "RPRT -1\n");
		else
			tx_printf(c, "%s\n", buf);
	}
	else if ((strncmp(cmd, "T ", 2) == 0) || (strncmp(cmd, "set_ptt ", 8) == 0)) {
		arg = strchr(cmd, ' ');
		arg++;
		if (sscanf(arg, "%d", &i) != 1)
			tx_append(c, "RPRT -1\n");
		else
			tx_rprt(c, set_ptt(c->rig, i));
	}
	else if ((strcmp(cmd, "t") == 0) || (strcmp(cmd, "get_ptt") == 0)) {
		switch (get_ptt(c->rig)) {
			case 0:
				tx_append(c, "0\n");
				break;
			case 1:
				tx_append(c, "1\n");
				break;
			default:
				tx_append(c, "RPRT -1\n");
				break;
		}
	}
	else if (strcmp(cmd, "chk_vfo")==0) {
		tx_append(c, "CHKVFO 0\n");
	}
	else if (strcmp(cmd, "\\dump_state")==0) {
		// Output copied from the dummy driver...
		tx_append(c,
			"0\n"
			"1\n"
			"2\n"
			"150000.000000 1500000000.000000 0x1ff -1 -1 0x10000003 0x3\n"
			"0 0 0 0 0 0 0\n"
			"0 0 0 0 0 0 0\n"
			"0x1ff 1\n"
			"0x1ff 0\n"
			"0 0\n"
			"0x1e 2400\n"
			"0x2 500\n"
			"0x1 8000\n"
			"0x1 2400\n"
			"0x20 15000\n"
			"0x20 8000\n"
			"0x40 230000\n"
			"0 0\n"
			"9990\n"
			"9990\n"
			"10000\n"
			"0\n"
			"10 \n"
			"10 20 30 \n"
			"0x0\n"	// has get func
			"0x0\n" // has set func
			"0x0\n"	// get level
			"0x0\n"	// set level
			"0x0\n"	// get param
			"0x0\n" // set param
		);
	}
	else {
		tx_append(c, "RPRT -1\n");
	}
	free(cmd);
}

void main_loop(void) {
	fd_set				rx_set;
	fd_set				tx_set;
	fd_set				err_set;
	int					max_sock;
	int					ret;
	int					avail;
	struct listener		*l;
	struct connection	*c;
	char				*buf;

	for (;;) {
		max_sock = 0;
		FD_ZERO(&rx_set);
		FD_ZERO(&tx_set);
		FD_ZERO(&err_set);
		// First, add all listening sockets to the rx_set
		for (l = listeners; l; l=l->next_listener) {
			FD_SET(l->socket, &rx_set);
			if (l->socket > max_sock)
				max_sock = l->socket;
		}
		// Next, add all active connections to the other sets as appropriate
		for (c = connections; c; c=c->next_connection) {
			FD_SET(c->socket, &rx_set);
			FD_SET(c->socket, &err_set);
			if (c->socket > max_sock)
				max_sock = c->socket;
			if (c->tx_buf)
				FD_SET(c->socket, &tx_set);
		}
		// select()
		ret = select(max_sock+1, &rx_set, &tx_set, &err_set, NULL);
		if (ret==-1) {
			fprintf(stderr, "Error %d selecting sockets!  Aborting.\n", errno);
			return;
		}
		if (ret == 0)
			continue;
		// Read/write data as appropriate...
		for (c = connections; c; c=c->next_connection) {
			// First, the exceptions... we'll just close it for now.
			if (FD_ISSET(c->socket, &err_set)) {
				close_connection(c);
			}
			// Next the writes
			if (FD_ISSET(c->socket, &tx_set)) {
				if (c->tx_buf != NULL) {
					ret = send(c->socket, c->tx_buf + c->tx_buf_pos, c->tx_buf_size-c->tx_buf_pos, 0);
					if (ret > 0) {
						c->tx_buf_pos += ret;
						if (c->tx_buf_pos >= c->tx_buf_size) {
							free(c->tx_buf);
							c->tx_buf = NULL;
							c->tx_buf_size = 0;
							c->tx_buf_pos = 0;
						}
					}
					else if (ret < 0)
						close_connection(c);
				}
			}
			// Now the read()s.
			if (FD_ISSET(c->socket, &rx_set)) {
				ret = ioctl(c->socket, FIONREAD, &avail);
				if (ret == -1)
					close_connection(c);
				else if (avail > 0) {
					buf = realloc(c->rx_buf, c->rx_buf_size + avail);
					if (buf == NULL)
						close_connection(c);
					else {
						c->rx_buf = buf;
						c->rx_buf_size += avail;
						ret = recv(c->socket, c->rx_buf + c->rx_buf_pos, avail, MSG_DONTWAIT);
						if (ret <= 0)
							close_connection(c);
						else {
							c->rx_buf_pos += ret;
							if ((buf = memchr(c->rx_buf, '\n', c->rx_buf_pos)) != NULL) {
								*buf = 0;
								handle_command(c, (size_t)(buf - c->rx_buf));
							}
						}
					}
				}
				else
					close_connection(c);
			}
		}
		// Accept() new connections...
		for (l = listeners; l; l=l->next_listener) {
			if (FD_ISSET(l->socket, &rx_set)) {
				c = (struct connection *)calloc(1, sizeof(struct connection));
				if (c == NULL)
					continue;
				c->socket = accept(l->socket, NULL, NULL);
				if (c->socket == -1) {
					free(c);
					continue;
				}
				c->rig = l->rig;
				c->next_connection = connections;
				connections = c;
			}
		}
	}
}

int main(int argc, char **argv)
{
	int			i;
	int			rig_count;
	int			thread_count = 0;
	dictionary	*d = NULL;
	int			active_rig_count = 0;

	// Parse command-line arguments to find the INI file...
	for (i=1; i<argc; i++) {
		if (argv[i][0]=='-') {
			switch(argv[i][1]) {
				case 'c':
					i++;
					if (i >= argc)
						goto usage;
					d = iniparser_load(argv[i]);
					if (d == NULL) {
						fprintf(stderr, "Unable to parse %s\n", argv[i]);
						return 1;
					}
			}
		}
		else
			goto usage;
	}
	if (d == NULL)
		goto usage;

	/*
	 * Now for each rig in the INI file, fire up a thread to do the
	 * socket interface
	 */
	rig_count = iniparser_getnsec(d);
	if (rig_count <= 0) {
		fprintf(stderr, "No rigs found!  Aborting.\n");
		return 1;
	}
	for (i=0; i<rig_count; i++)
		active_rig_count += add_rig(d, iniparser_getsecname(d, i));

	if (active_rig_count == 0) {
		fprintf(stderr, "Unable to set up any sockets!  Aborting\n");
		return 1;
	}

	main_loop();

	if (d)
		iniparser_freedict(d);
	return 0;
usage:
	printf("Usage:\n"
		"%s -c <config>\n\n"
		"Where <config> is the path to the ini file\n\n",
		argv[0]);
	if (d)
		iniparser_freedict(d);
	return 1;
}