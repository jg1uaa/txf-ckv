// SPDX-License-Identifier: WTFPL

#include <bstdio.h>
#include <bstring.h>
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

LOCAL W client(W fd, struct sockaddr_in *addr)
{
	WERR err;
	LINK l;
	W i, f, size, remain;
	struct txf_header h;
	B buf[BLOCKSIZE];
	TC tmp[FILENAME_LEN + 1];

	printf("* client\n");

	/* connect to server */
	if (so_connect(fd, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
		printf("client: connect\n");
		goto fin0;
	}

	printf("connected to %s\n", inet_ntoa(addr->sin_addr));

	/* receive header */
	if (recv_block(fd, &h, sizeof(h)) < sizeof(h)) {
		printf("client: recv_block (header)\n");
		goto fin0;
	}

	if (ntohl(h.magic) != MAGIC_SEND) {
		printf("client: invalid header\n");
		goto fin0;
	}

	h.filename_term = '\0';
	size = ntohl(h.filesize);
	eucstotcs(tmp, h.filename);

	printf("%S, %d byte\n", tmp, size);

	/* receive file */
	if ((err = get_lnk(NULL, &l, F_NORM)) < ER_OK) {
		printf("client: get_lnk\n");
		goto fin0;
	}

	if ((err = cre_fil(&l, tmp, NULL, 0, F_FIX)) < ER_OK) {
		printf("client: cre_fil\n");
		goto fin0;
	}
	f = err;

	/* XXX store as main-TAD record */
	if ((err = apd_rec(f, NULL, 0, 1, 0, 0)) < ER_OK) {
		printf("client: apd_rec\n");
		goto fin1;
	}

	if ((err = see_rec(f, -1, -1, NULL)) < ER_OK) {
		printf("client: see_rec\n");
		goto fin1;
	}

	for (i = 0; i < size; i += BLOCKSIZE) {
		remain = size - i;
		if (remain > BLOCKSIZE)
			remain = BLOCKSIZE;

		if (recv_block(fd, buf, remain) < remain) {
			printf("client: recv_block (data)\n");
			goto fin1;
		}

		if ((err = wri_rec(f, i, buf, remain, NULL, NULL, 0)) < ER_OK) {
			printf("client: wri_rec\n");
			goto fin1;
		}
	}

	/* send ack */
	h.magic = htonl(MAGIC_RCVD);
	if (send_block(fd, &h, sizeof(h)) < sizeof(h)) {
		printf("client: send_block (ack)\n");
		goto fin1;
	}

fin1:
	cls_fil(f);
fin0:
	return 0;
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

LOCAL W server(W fd, struct sockaddr_in *addr, TC *filename)
{
	WERR err;
	W d, f;
	LINK l;
	W i, size, remain;
	B buf[BLOCKSIZE];
	struct txf_header h;
	struct sockaddr_in peer;
	W peer_len;
	TC path[L_PATHNM];
	B fn[FILENAME_LEN + 1];

	printf("* server\n");

	convert_path(path, filename, L_PATHNM);

	/* file open */
	if (get_filename(path, fn) < 1) {
		printf("server: invalid file name\n");
		goto fin0;
	}

	if ((err = get_lnk(path, &l, F_NORM)) < ER_OK) {
		printf("server: get_lnk\n");
		goto fin0;
	}

	if ((err = opn_fil(&l, F_READ | F_EXCL, NULL)) < ER_OK) {
		printf("server: opn_fil\n");
		goto fin0;
	}
	f = err;

	/* find main-TAD record */
	if ((err = fnd_rec(f, F_TOPEND, (1 << 1), 0, NULL)) >= ER_OK)
		printf("server: main TAD record\n");
	else if ((err = see_rec(f, 0, 1, NULL)) >= ER_OK)
		/* no main-TAD record; use top record */
		printf("server: top record\n");
	else {
		printf("server: see_rec\n");
		goto fin1;
	}

	if ((err = rea_rec(f, 0, NULL, 0, &size, NULL)) < ER_OK) {
		printf("server: rea_rec (filesize)\n");
		goto fin1;
	}

	printf("%s, %d byte\n", fn, size);

	/* wait for connect */
	if (so_bind(fd, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
		printf("server: bind\n");
		goto fin1;
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

	printf("connected from %s\n", inet_ntoa(peer.sin_addr));

	/* send header */
	memset(&h, 0, sizeof(h));
	h.magic = htonl(MAGIC_SEND);
	h.filesize = htonl(size);
	strcpy(h.filename, fn);

	if (send_block(d, &h, sizeof(h)) < sizeof(h)) {
		printf("server: send_block (header)\n");
		goto fin2;
	}

	/* send file */
	for (i = 0; i < size; i += BLOCKSIZE) {
		remain = size - i;
		if (remain > BLOCKSIZE)
			remain = BLOCKSIZE;

		if ((err = rea_rec(f, i, buf, remain, NULL, NULL)) < ER_OK) {
			printf("server: rea_rec (data)\n");
			goto fin2;
		}

		if (send_block(d, buf, remain) < remain) {
			printf("server: send_block (data)\n");
			goto fin2;
		}
	}

	/* receive ack */
	if (recv_block(d, &h, sizeof(h)) < sizeof(h)) {
		printf("server: recv_block (ack)\n");
		goto fin2;
	}

	if (ntohl(h.magic) != MAGIC_RCVD) {
		printf("server: invalid ack\n");
		goto fin2;
	}

fin2:
	so_close(d);
fin1:
	cls_fil(f);
fin0:
	return 0;
}

EXPORT W main(W argc, TC *argv[])
{
	WERR fd;
	struct sockaddr_in addr;
	B tmp[256];

	if (argc < 3) {
		printf("%S [ipv4-addr] [port] [(filename to send)]\n",
		       argv[0]);
		goto fin0;
	}

	if ((fd = so_socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("main: socket\n");
		goto fin0;
	}

	tcstoeucs(tmp, argv[1]);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(tmp);
	addr.sin_port = htons(tc_atoi(argv[2]));

	if (argc == 3)
		client(fd, &addr);
	else if (argc == 4)
		server(fd, &addr, argv[3]);

	so_close(fd);
fin0:
	return 0;
}
