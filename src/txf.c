// SPDX-License-Identifier: WTFPL

#include <basic.h>
#include <bstdio.h>
#include <bstring.h>
#include <bstdlib.h>
#include <btron/file.h>
#include <btron/bsocket.h>
#include <tcode.h>
#include <tstring.h>
#include <errcode.h>

#define MAGIC_SEND	0x53454e44	// "SEND"
#define MAGIC_RCVD	0x72637664	// "rcvd"
#define FILENAME_LEN	20
#define BLOCKSIZE	1024
#define MAX_FILE_SIZE	0x7fffffff

struct txf_header {
	UW magic;
	UW filesize;		// big endian
	B filename[FILENAME_LEN];
	B filename_term;		// must be zero
	B unused[3];
} __attribute__((packed));

struct txf_workingset {
	VP (*init)(TC *arg);
	WERR (*process)(W d, VP handle);
	VOID (*finish)(VP handle);
};

struct txf_tx_workarea {
	W fd;
	W size;
	struct txf_header h;
};

LOCAL WERR send_block(W d, VP buf, W size)
{
	WERR pos, wsize;

	for (pos = 0; pos < size; pos += wsize) {
		if ((wsize = so_write(d, buf + pos, size - pos)) < 0)
			break;
	}

	return pos;
}

LOCAL WERR recv_block(W d, VP buf, W size)
{
	WERR pos, rsize;

	for (pos = 0; pos < size; pos += rsize) {
		if ((rsize = so_read(d, buf + pos, size - pos)) < 0)
			break;
	}

	return pos;
}

LOCAL W get_filename(TC *filename_tc, B *filename_ascii)
{
#define DELIMITER	'/'

	W i, len;
	TC *p;
	B *q;

	/* find the last delimiter character */
	len = tc_strlen(filename_tc);
	for (i = len - 1; i >= 0; i--) {
		if (filename_tc[i] == TC_FDLM)
			break;
	}

	/* filename starts after delimiter */
	p = filename_tc + i + 1;
	q = filename_ascii;

	/* convert filename to ASCII */
	for (i = 0; i < FILENAME_LEN; i++) {
		if (*p == TK_NULL || *p == TC_FSEP)
			break;

		/* multi-byte character is not supported */
		if (tctoeuc(q, *p) != 1 || *q == DELIMITER)
			*q = '_';

		p++;
		q++;
	}

	*q = '\0';

	return strlen(filename_ascii);
}

/* convert pathname */
LOCAL VOID convert_path(TC *out, TC *in, W maxlen)
{
	W len;

	len = tc_strlen(in);
	if (len > maxlen - 1)
		len = maxlen - 1;

	while (len-- > 0) {
		switch (*in) {
		case TK_SLSH:
			*out = TC_FDLM;
			break;
		case TK_COLN:
			*out = TC_FSEP;
			break;
		case TK_PCNT:
			in++;
			len--;
			if (len > 0)
				*out = *in;
			break;
		default:
			*out = *in;
			break;
		}
		in++;
		out++;
	}

	*out = TK_NULL;
}

LOCAL VP rx_init(TC *arg)
{
	/* do nothing */
	return rx_init;
}

LOCAL WERR rx_process(W fd, VP handle)
{
	WERR err;
	LINK l;
	W i, f, size, remain;
	struct txf_header h;
	B buf[BLOCKSIZE];
	TC tmp[FILENAME_LEN + 1];

	/* receive header */
	if (recv_block(fd, &h, sizeof(h)) < sizeof(h)) {
		printf("rx_process: recv_block (header)\n");
		err = ER_IO;
		goto fin0;
	}

	if (ntohl(h.magic) != MAGIC_SEND) {
		printf("rx_process: invalid header\n");
		err = ER_OBJ;
		goto fin0;
	}

	h.filename_term = '\0';
	size = ntohl(h.filesize);
	eucstotcs(tmp, h.filename);

	printf("%S, %d byte\n", tmp, size);

	/* receive file */
	if ((err = get_lnk(NULL, &l, F_NORM)) < ER_OK) {
		printf("rx_process: get_lnk\n");
		goto fin0;
	}

	if ((err = cre_fil(&l, tmp, NULL, 0, F_FIX)) < ER_OK) {
		printf("rx_process: cre_fil\n");
		goto fin0;
	}
	f = err;

	/* XXX store as main-TAD record */
	if ((err = apd_rec(f, NULL, 0, 1, 0, 0)) < ER_OK) {
		printf("rx_process: apd_rec\n");
		goto fin1;
	}

	if ((err = see_rec(f, -1, -1, NULL)) < ER_OK) {
		printf("rx_process: see_rec\n");
		goto fin1;
	}

	for (i = 0; i < size; i += BLOCKSIZE) {
		remain = size - i;
		if (remain > BLOCKSIZE)
			remain = BLOCKSIZE;

		if (recv_block(fd, buf, remain) < remain) {
			printf("rx_process: recv_block (data)\n");
			err = ER_IO;
			goto fin1;
		}

		if ((err = wri_rec(f, i, buf, remain, NULL, NULL, 0)) < ER_OK) {
			printf("rx_process: wri_rec\n");
			goto fin1;
		}
	}

	/* send ack */
	h.magic = htonl(MAGIC_RCVD);
	if (send_block(fd, &h, sizeof(h)) < sizeof(h)) {
		printf("rx_process: send_block (ack)\n");
		err = ER_IO;
		goto fin1;
	}

	err = ER_OK;
fin1:
	cls_fil(f);
fin0:
	return err;
}

LOCAL void rx_finish(VP handle)
{
	/* do nothing */
}

LOCAL VP tx_init(TC *filename)
{
	struct txf_tx_workarea *wk;
	WERR err;
	W f, size;
	LINK l;
	TC path[L_PATHNM];
	B fn[FILENAME_LEN + 1];

	wk = malloc(sizeof(*wk));
	if (wk == NULL) {
		printf("tx_init: malloc\n");
		err = ER_NOMEM;
		goto fin0;
	}

	convert_path(path, filename, L_PATHNM);

	/* file open */
	if (get_filename(path, fn) < 1) {
		printf("tx_init: invalid file name\n");
		err = ER_PAR;
		goto fin1;
	}

	if ((err = get_lnk(path, &l, F_NORM)) < ER_OK) {
		printf("tx_init: get_lnk\n");
		goto fin1;
	}

	if ((err = opn_fil(&l, F_READ | F_EXCL, NULL)) < ER_OK) {
		printf("tx_init: opn_fil\n");
		goto fin1;
	}
	f = err;

	/* find main-TAD record */
	if ((err = fnd_rec(f, F_TOPEND, (1 << 1), 0, NULL)) >= ER_OK) {
		printf("tx_init: main TAD record\n");
	} else if ((err = see_rec(f, 0, 1, NULL)) >= ER_OK) {
		/* no main-TAD record; use top record */
		printf("tx_init: top record\n");
	} else {
		printf("tx_init: see_rec\n");
		goto fin2;
	}

	if ((err = rea_rec(f, 0, NULL, 0, &size, NULL)) < ER_OK) {
		printf("tx_init: rea_rec (filesize)\n");
		goto fin2;
	}

	/* store file information to workarea */
	wk->fd = f;
	wk->size = size;

	memset(&wk->h, 0, sizeof(wk->h));
	wk->h.magic = htonl(MAGIC_SEND);
	wk->h.filesize = htonl(size);
	strcpy(wk->h.filename, fn);

	printf("%s, %d byte\n", fn, size);
	goto fin0;

fin2:
	cls_fil(f);
fin1:
	free(wk);
	wk = NULL;
fin0:
	return wk;
}

LOCAL WERR tx_process(W d, VP handle)
{
	struct txf_tx_workarea *wk = handle;
	W i, remain;
	struct txf_header h;
	B buf[BLOCKSIZE];
	WERR err;

	/* send header */
	if (send_block(d, &wk->h, sizeof(wk->h)) < sizeof(wk->h)) {
		printf("tx_process: send_block (header)\n");
		err = ER_IO;
		goto fin0;
	}

	/* send file */
	for (i = 0; i < wk->size; i += BLOCKSIZE) {
		remain = wk->size - i;
		if (remain > BLOCKSIZE)
			remain = BLOCKSIZE;

		if ((err = rea_rec(wk->fd, i, buf, remain, NULL, NULL)) < ER_OK) {
			printf("tx_process: rea_rec (data)\n");
			goto fin0;
		}

		if (send_block(d, buf, remain) < remain) {
			printf("tx_process: send_block (data)\n");
			err = ER_IO;
			goto fin0;
		}
	}

	/* receive ack */
	if (recv_block(d, &h, sizeof(h)) < sizeof(h)) {
		printf("tx_process: recv_block (ack)\n");
		err = ER_IO;
		goto fin0;
	}

	if (ntohl(h.magic) != MAGIC_RCVD) {
		printf("tx_process: invalid ack\n");
		err = ER_OBJ;
		goto fin0;
	}

	err = ER_OK;
fin0:
	return err;
}

LOCAL VOID tx_finish(VP handle)
{
	struct txf_tx_workarea *wk = handle;

	cls_fil(wk->fd);
	free(handle);
}

LOCAL WERR client(W fd, struct sockaddr_in *addr, TC *arg, struct txf_workingset *work)
{
	VP handle;
	WERR err = ER_IO;

	printf("* client\n");

	if ((handle = (*work->init)(arg)) == NULL) {
		printf("client: init\n");
		goto fin0;
	}

	/* connect to server */
	if (so_connect(fd, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
		printf("client: connect\n");
		goto fin1;
	}

	printf("connected to %s\n", inet_ntoa(addr->sin_addr));

	if ((*work->process)(fd, handle)) {
		printf("client: process\n");
		goto fin1;
	}

	err = ER_OK;
fin1:
	(*work->finish)(handle);
fin0:
	return err;
}

LOCAL W server(W fd, struct sockaddr_in *addr, TC *arg, struct txf_workingset *work)
{
	VP handle;
	WERR err = ER_IO;
	W d, en = 1, peer_len;
	struct sockaddr_in peer;

	printf("* server\n");

	if ((handle = (*work->init)(arg)) == NULL) {
		printf("server: init\n");
		goto fin0;
	}

	/* wait for connect */
	so_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (B *)&en, sizeof(en));
	if (so_bind(fd, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
		printf("server: bind\n");
		goto fin1;
	}

	peer_len = sizeof(peer);
	if (so_getsockname(fd, (struct sockaddr *)&peer, &peer_len) >= 0) {
		printf("address %s port %d\n",
		       inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
	}

	if (so_listen(fd, 1) < 0) {
		printf("server: listen\n");
		goto fin1;
	}

	peer_len = sizeof(peer);
	if ((d = so_accept(fd, (struct sockaddr *)&peer, &peer_len)) < 0) {
		printf("server: accept\n");
		goto fin1;
	}

	printf("connected from %s port %d\n",
	       inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

	if ((*work->process)(d, handle)) {
		printf("server: process\n");
		goto fin2;
	}

	err = ER_OK;
fin2:
	so_close(d);
fin1:
	(*work->finish)(handle);
fin0:
	return err;
}

EXPORT W main(W argc, TC *argv[])
{
	WERR fd;
	W tx_file, rx_server, port;
	struct sockaddr_in addr;
	struct txf_workingset rx_set = {rx_init, rx_process, rx_finish};
	struct txf_workingset tx_set = {tx_init, tx_process, tx_finish};
	B tmp[256];

	if (argc < 3 || argc > 4) {
		printf("%S [ipv4-addr] [port] [(filename to send)]\n",
		       argv[0]);
		goto fin0;
	}

	if ((fd = so_socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("main: socket\n");
		goto fin0;
	}

	/* default: tx-server/rx-client mode */
	port = tc_atoi(argv[2]);
	tx_file = (argc == 4) ? 1 : 0;
	rx_server = 0;

	/* if port is negative, rx-server/tx-client mode */
	if (*argv[2] == TK_MINS) {
		port = -port;
		tx_file ^= 1;
		rx_server = 1;
	}

	tcstoeucs(tmp, argv[1]);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(tmp);
	addr.sin_port = htons(port);

	switch ((rx_server << 1) | tx_file) {
	case 0:
		client(fd, &addr, NULL, &rx_set);
		break;
	case 1:
		server(fd, &addr, argv[3], &tx_set);
		break;
	case 2:
		client(fd, &addr, argv[3], &tx_set);
		break;
	case 3:
		server(fd, &addr, NULL, &rx_set);
		break;
	}

	so_close(fd);
fin0:
	return 0;
}
