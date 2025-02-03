/*
 * imapshell.c
 *
 * a shell for searching an IMAP folder
 */

/*
 * main functions
 *
 * fdgets()
 * 	read a line or a ()-balanced string from the server
 * 
 * sendcommand()
 * recvanswer()
 * recvmail()
 * 	send command and receive answer
 * 	answer is different when fetching a mail body
 * 
 * sendrecv()
 * fetch()
 * 	send + receive or receive mail body
 * 
 * imaprun()
 * 	retrieve list of mails
 * 	retrieve envelopes and show them
 * 	retrieve flags or bodies or delete
 * loop()
 * 	call imaprun() on list, show, save and delete
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <term.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <signal.h>
#include <sys/wait.h>
#include <readline/readline.h>
#include <readline/history.h>

/*
 * default file names
 */
#define DEFAULTVIEWER   "imapenvelope"
#define DEFAULTPAGER    "more"
#define DEFAULTSIMULATE "imapsimulate"

/*
 * ports
 */
#define PLAINPORT 143
#define SSLPORT   993

/*
 * size of buffers
 */
#define BUFLEN 4096

/*
 * debug print
 */
int debug = 0;
#define dprintf if (debug) printf

/*
 * accounts
 */
struct account {
	char name[100];
	char srv[100];
	char usr[100];
	char psw[100];
	char dir[100];
} *accts;

/*
 * read account file
 */
struct account *readaccounts(char *filename) {
	struct account *accts;
	FILE *file;
	int i, j;

	file = fopen(filename, "r");
	if (file == NULL) {
		perror(filename);
		return NULL;
	}

	i = 0;
	j = 10;
	accts = malloc(j * sizeof(*accts));
	while (5 == fscanf(file, "%99s	%99s	%99s	%99s	%99s\n",
	                   accts[i].name, accts[i].srv, accts[i].usr,
	                   accts[i].psw, accts[i].dir)) {
		i++;
		if (i > j) {
			j += 10;
			accts = realloc(accts, j * sizeof(*accts));
		}

	}
	accts[i].srv[0] = '\0';

	fclose(file);
	return accts;
}

/*
 * find account
 */
struct account *findaccount(char *name) {
	struct account *account;
	char buf[BUFLEN];
	int i, accno;
	char *res;

			/* account from argument */

	if (name != NULL && strchr(name, ':') != NULL) {
		account = malloc(sizeof(struct account));
		strcpy(account->usr, name);
		strcpy(account->srv, strchrnul(account->usr, '@') + 1);
		strcpy(account->dir, strchr(account->srv, '/') ?
				strchrnul(account->srv, '/') + 1 : "INBOX");
		*strchrnul(account->usr, ':') = '\0';
		*strchrnul(account->srv, '/') = '\0';
		strcpy(account->psw, "NULL");
		return account;
	}

			/* read account file */

	if (getenv("HOME") == NULL) {
		printf("no home directory\n");
		return NULL;
	}
	strcpy(buf, getenv("HOME"));
	strcat(buf, "/.config/imapshell/accounts.txt");
	accts = readaccounts(buf);
	if (accts == NULL) {
		printf("cannot read account file\n");
		return NULL;
	}

	if (name == NULL) {
		printf("accounts in %s\n", buf);
		for (i = 0; accts[i].srv[0] != '\0'; i++)
			printf("%d: user %s, mail in %s/%s\n",
			       i, accts[i].usr, accts[i].srv, accts[i].dir);
		return &accts[0];
	}

			/* find by account number */

	accno = strtol(name, &res, 10);
	if (*res == '\0') {
		if (accno < 0) {
			printf("account number cannot be negative\n");
			return NULL;
		}
		return &accts[accno];
	}

			/* find by account name */

	for (accno = 0; accts[accno].srv[0] != '\0'; accno++)
		if (! strcmp(accts[accno].name, name))
			return &accts[accno];

	printf("no such account\n");
	return NULL;
}

/*
 * simulated errors
 *
 */
char *simulate_error_where;
#define INITIALIZE_ERROR() srandom(time(NULL))
#define SIMULATE_ERROR(where, what)					\
	if (strstr(simulate_error_where, where)) {			\
		printf("SIMULATED ERROR %s\n", where);			\
		what[random() % strlen(what)] =				\
			random() % ('~' - ' ') + ' '; \
	}
void simulate_error_usage() {
	printf("simulated errors:\n");
	printf("\tfetch-envelope\n\tfetch-flags\n\tfetch-body-structure\n");
	printf("\tfetch-body\n\tlogin\n\tlist-inboxes\n\tselect-inbox\n");
	printf("\tsearch\n\tfetch-all-envelopes\n\trestore-flags\n\tdelete\n");
	printf("\tnoop\n");
}

/*
 * colorized output
 */
char *it = "";
char *rm = "";

/* 
 * log and error printing
 */
void (*printstring) (char *);

FILE *outfile;
char *external;

void printstringtofile(char *s) {
	fprintf(outfile, "%s", s);
}

void printstringonscreen(char *s) {
	printf("%s", s);
	fflush(stdout);
}

void printstringtocommand(char *s) {
	FILE *pipe;
	fflush(stdout);
	pipe = popen(external, "w");
	if (pipe == NULL) {
		perror(external);
		return;
	}
	fwrite(s, 1, strlen(s), pipe);
	pclose(pipe);
}

void printstringnowhere(char *s) {
	(void) s;
}

/*
 * open a plaintext connection
 */
int openconnection(char *hostname, int port) {
	int sock;
	struct sockaddr_in server;
	struct hostent *hp, *gethostbyname();
	char buf[BUFLEN];

			/* open socket */

	printstring("opening socket\n") ;
	sock = socket(AF_INET , SOCK_STREAM , 0);
	if (sock == -1) {
		perror("opening socket stream");
		exit(2);
	}

			/* get host */

	server.sin_family = AF_INET;
	hp = gethostbyname(hostname);
	if (hp == (struct hostent *) 0) {
		fprintf(stderr , "unknow host: %s\n" , hostname);
		exit(2);
	}
	printstring("address ");
	sprintf(buf, "%u:%u:%u:%u %d\n",
		(unsigned char) hp->h_addr[0], (unsigned char) hp->h_addr[1],
		(unsigned char) hp->h_addr[2], (unsigned char) hp->h_addr[3],
		port);
	printstring(buf);

			/* connect */
	
	printstring("connecting\n") ;
	memcpy((char *) &server.sin_addr , (char *) hp->h_addr, hp->h_length);
	server.sin_port = htons(port);
	if (connect(sock, (struct sockaddr *) &server , sizeof server) == -1) {
		perror("connecting stream socket");
		exit(1);
	}
	printstring("connected\n");

	return sock;
}

#ifdef SSLFT
/*
 * print all errors in the ssl error queue
 */
void printsslerrors() {
	char buf[120];
	u_long err;

	while (0 != (err = ERR_get_error())) {
		ERR_error_string(err, buf);
		printf("*** %s\n", buf);
	}
}

/*
 * open a SSL connection
 */
SSL *openssl(char *hostname, int port) {
	int sock;
	int res;
	SSL *sl;
	SSL_CTX *sc;

			/* connect */

	sock = openconnection(hostname, port);
	if (sock == -1)
		return NULL;

			/* SSL init */

	ERR_load_crypto_strings();
	SSL_load_error_strings();
	SSL_library_init();

	// sc = SSL_CTX_new(SSLv3_client_method());
	sc = SSL_CTX_new(SSLv23_client_method());
	if (sc == NULL) {
		printf("Error creating SSL_CTX object\n");
		exit(EXIT_FAILURE);
	}

	sl = SSL_new(sc);
	if (sl == NULL) {
		printf("Error creating SSL object\n");
		exit(EXIT_FAILURE);
	}

  			/* SSL connect */

	res = SSL_set_fd(sl, sock);
	if (res != 1) {
		printf("Error on SSL_set_fd\n");
		exit(EXIT_FAILURE);
	}

	res = SSL_connect(sl);
	if (res != 1) {
		printf("Error on SSL_connect: res=%d, SSL_get_error=%d\n",
		       res, SSL_get_error(sl, res));
		printf("%s\n", ERR_error_string(SSL_get_error(sl, res), NULL));
		printsslerrors();
		exit(EXIT_FAILURE);
	}
	return sl;
}
#endif

/*
 * IMAP server: either SSL, plaintext or local program with pipe
 */
struct server {
	SSL *ssl;
	int fd;
	FILE *pipe;
	char *program;
};

/*
 * connect to an IMAP server
 */
int imapconnect(struct server *server, char *hostname) {
	if (server->program == NULL && server->fd == -1) {
		server->ssl = openssl(hostname, SSLPORT);
		return server->ssl == NULL ? -1 : 0;
	}
	server->ssl = NULL;
	if (server->program == NULL) {
		server->fd = openconnection(hostname, PLAINPORT);
		return server->fd;
	}
	server->fd = -1;
	server->pipe = NULL;
	return 0;
}

/*
 * close connection
 */
int imapclose(struct server *server) {
	if (server->ssl != NULL)
		return SSL_shutdown(server->ssl);
	if (server->fd >= 0)
		return close(server->fd);
	if (server->pipe != NULL)
		return pclose(server->pipe);
	return 0;
}

/*
 * read from an imap server
 */
int FD_read(struct server *server, char *buf, int n) {
	int len, leof;
	char ceof;

	if (server->ssl != NULL)
		return SSL_read(server->ssl, buf, n);
	if (server->fd != -1) 
		return read(server->fd, buf, n);
	if (server->pipe == NULL) // read without write: initial greeting
		return 0;

	len = fread(buf, 1, n, server->pipe);

	leof = fread(&ceof, 1, 1, server->pipe);
	if (len < n || leof < 1 || feof(server->pipe)) {
		pclose(server->pipe);
		server->pipe = NULL;
	}
	else if (leof == 1)
		ungetc(ceof, server->pipe);

	return len;
}

/*
 * write to an IMAP server
 */
int FD_write(struct server *server, char *buf, int n) {
	char *cmdline;

	if (server->ssl != NULL)
		return SSL_write(server->ssl, buf, n);
	if (server->fd != -1) 
		return write(server->fd, buf, n);
	if (server->pipe == NULL) {
		cmdline = malloc(strlen(server->program) + n + 2);
		sprintf(cmdline, "%s %s", server->program, buf);
		server->pipe = popen(cmdline, "r");
		return server->pipe == NULL;
	}
	return -1;
}

/*
 * line reading 
 * 
 * read a line if balanced == 0
 *
 * otherwise, read until the next newline where parentheses are balanced and
 * the quote count is even
 *	1. reset the quote count on an open or closed parentesis
 *	2. do not count parenthesis if the quote count is uneven
 *	3. if {n} is encountered, skip n bytes (not including \r or \n)
 */
char *fdgets(char *buf, int size, struct server *server, int balanced) {
	int len, pos;
	char c;
	int parent = 0, quote = 0, backslash = 0;
	int skip = 0;

	for (pos = 0; ; pos++) {

		if (pos >= size - 2)
			pos--;

				/* read char */

		len = FD_read(server, &c, 1);
		if (len == 0) {
			buf[0] = '\0';
			break;
		}
		buf[pos] = c;

				/* skip because of a previous {n} */

		if (skip != 0) {
			if (c == '\n' || c == '\r')
				pos--;
			else {
				buf[pos] = c;
				skip--;
				if (skip == 0)
					buf[++pos] = '"';
			}
			continue;
		}

				/* previous char was a backslash */

		if (backslash) {
			backslash = 0;
			continue;
		}

		switch (c) {

				/* current character is a backslash */
		case '\\':
			backslash = 1;
			break;

				/* parenthesis and quote */
		case '(':
			if (quote % 2)
				break;
			parent++;
			quote = 0;
			break;
		case ')':
			if (quote % 2)
				break;
			parent--;
			quote = 0;
			break;
		case '"':
			quote++;
			break;

				/* {n} means: skip n chars */
		case '{':
			if (! balanced || (quote % 2))
				break;
			skip = 0;
			while (1) {
				len = FD_read(server, &c, 1);
				if (c == '}' || len == 0)
					break;
				skip = skip * 10 + (c - '0');
			}
			buf[pos] = '"';
			break;

				/* \n ends line */
		case '\n':
			// printf("%d %d\n", parent, quote);
			if (! balanced || (parent <= 0 && quote % 2 == 0)) {
				buf[pos + 1] = '\0';
				if (pos + 1 >= size - 2)
					strcpy(buf, "* TOOLARGE\n");
				return buf;
			}
			break;
		}
	}

	return buf;
}

/*
 * functions for sending commands and receiving answers
 * the protocol is:
 * 
 * send: tag command
 * recv: * line
 *       * line
 *       * line
 *       tag OK
 * 
 * tag is an arbitrary string, unique for each command
 */

int cnum = 0;

/*
 * send a command
 */
void sendcommand(struct server *server, char *comm) {
	char *c;
	char *res;

	printstring(">>> ");

	c = (char *) malloc(10 + strlen(comm) + 3);
	sprintf(c, "A%03d %s\r\n", cnum, comm);
	if (strstr(comm, "LOGIN") != comm)
		printstring(c);
	else {
		res = (char *) malloc(strlen(comm) + 10);
		sprintf(res, "A%03d LOGIN ...\n", cnum);
		printstring(res);
		free(res);
	}

	FD_write(server, c, strlen(c));
	free(c);
}

/*
 * receive answer
 */
char *recvanswer(struct server *server) {
	char buf[10000], tag[10];
	char *res, *r, *ok;

	res = strdup("");
	do {
		r = fdgets(buf, 9999, server, 1);
		if (r == NULL)
			break;
		printstring(buf);
		res = (char *) realloc(res, strlen(res) + strlen(buf) + 2);
		strcat(res, buf);
	} while (buf[0] == '*');

	sprintf(tag, "A%03d ", cnum);

	if (strstr(buf, tag) != buf) {
		printf("received: \"%s\"\n", buf);
		printf("received tag differs from expected %s\n", tag);
	}

	ok = (char *) malloc(strlen(tag) + 4);
	sprintf(ok, "%sOK", tag);
	if (strstr(buf, ok) != buf) {
		free(ok);
		free(res);
		return NULL;
	}
	free(ok);

	return res;
}

/*
 * handle received answer
 */
char *handleanswer(char *answer, int logout) {
	if (answer == NULL) {
		printf("non-OK answer\n");
		if (logout)
			exit(EXIT_FAILURE);
		else
			return NULL;
	}
	return answer;
}

/*
 * send command and receive answer
 */
void logout(int ret);
char *sendrecv(struct server *server, char *comm) {
	char *res;

	cnum++;

	sendcommand(server, comm);
	res = recvanswer(server);
	return handleanswer(res, ! strcmp(comm, "LOGOUT"));
}

/*
 * receive an email
 */
void recvemail(struct server *server, FILE* dest) {
	char buf[BUFLEN];
	char *res;
	int size, nr;

	res = fdgets(buf, BUFLEN, server, 0);
	printstring(res);
	while (*res != '{')
		res++;
	if (sscanf(res, "{%d}", &size) != 1) {
		printf("cannot read mail size\n");
		logout(EXIT_FAILURE);
	}

	res = (char *) malloc(1024);

	while (size > 0) {
		nr = FD_read(server, res, size < 1024 ? size : 1024);
		fwrite(res, 1, nr, dest);
		size -= nr;
	}
	free(res);

	fdgets(buf, 999, server, 0);
	printstring(buf);
}

/*
 * download a mail content
 * different answer than the other commands:
 * * n FETCH (BODY[] {size}
 * email
 * )
 * tag OK
*/
char *fetch(struct server *server, char *comm, FILE *dest) {
	char *res;

	cnum++;

	sendcommand(server, comm);
	recvemail(server, dest);
	res = recvanswer(server);
	if (res == NULL) {
		printf("Received non-OK answer, exiting\n");
		logout(EXIT_FAILURE);
	}

	return res;
}

/*
 * find number of mails from an answer
 */
int cardinality(char *answer, int *n) {
	char *cur, c;
	int i;

	if (answer == NULL)
		return -1;
	for (cur = answer;
	     cur != NULL && (cur == answer || ++cur != NULL);
	     cur = index(cur, '\n'))
		if (2 == sscanf(cur, "* %d EXISTS%c", &i, &c)) {
			dprintf("cardinality: %d\n", i);
			*n = i;
			return 1;
		}

	return 0;
}

/*
 * open an inbox in select or examine mode
 */
int openbox(struct server *server, char *inbox, int rw, int *n) {
	char buf[BUFLEN];
	char *res;

	sprintf(buf, "%s %s",
	        rw ? "SELECT" : "EXAMINE", inbox[0] == '\0' ? "INBOX" : inbox);
	SIMULATE_ERROR("select-inbox", buf);
	res = sendrecv(server, buf);
	cardinality(res, n);
	if (res == NULL)
		return -1;
	free(res);
	return 0;
}

/*
 * logout
 */
struct server server;
int sentlogout = 0;
void logout(int ret) {
	char *res;
	printf("logout\n");
	if (! sentlogout) {
		sentlogout = 1;
		res = sendrecv(&server, "LOGOUT");
		free(res);
	}
	imapclose(&server);
	exit(ret);
}

/*
 * read a character from terminal
 */
char readchar() {
	struct termios original, charbychar;
	char c;

	tcgetattr(STDIN_FILENO, &original);

	charbychar = original;
	charbychar.c_lflag &= ~(ICANON | ISIG | IEXTEN);
	charbychar.c_cc[VMIN] = 1;
	charbychar.c_cc[VTIME] = 0;

	tcsetattr(STDIN_FILENO, TCSANOW, &charbychar);

	fread(&c, 1, 1, stdin);

	tcsetattr(STDIN_FILENO, TCSANOW, &original);

	return c;
}

/*
 * signal hander
 */
int breakloop = 0;
void interrupt(int sig) {
	printf("received signal %d\n", sig);
	logout(EXIT_FAILURE);
}

/*
 * string to int with + and - for index
 */
int stringindextoint(char *s, int *idx, int lidx) {
	int i;

	i = atoi(s);
	if (i == 0)
		return 0;
	if (strchr("+-", s[0]) != NULL && (idx == NULL || lidx == 0))
		return 0;
	return s[0] == '+' ? idx[i - 1] : s[0] == '-' ? idx[lidx + i] : i;
}

/*
 * string to int array
 */
int *stringtointarray(char *s, int *size) {
	int alloc = 100, *idx;
	char *cur;

	idx = malloc(alloc * sizeof(int));

	*size = 0;
	cur = strtok(s, " ");
	while (cur != NULL) {
		if (atoi(cur) != 0) {
			if (*size >= alloc) {
				alloc += 100;
				idx = realloc(idx, alloc * sizeof(int));
			}
			idx[*size] = atoi(cur);
			(*size)++;
		}
		cur = strtok(NULL, " ");
	}

	return idx;
}

/*
 * int array to string
 */
char *intarraytostring(int *a, int n, char *sep) {
	char *r, buf[20];
	int len = 0, alloc = 0, inc = 1024;
	int i;

	r = strdup("");

	for (i = 0; i < n; i++) {
		sprintf(buf, "%d", a[i]);
		while (len + 1 + (int) strlen(buf) > alloc)
			r = realloc(r, alloc += inc);
		len += 1 + strlen(buf);
		strcat(r, i != 0 ? sep : "");
		strcat(r, buf);
	}

	return r;
}

/*
 * command
 */
struct imapcommand {
	char *account;
	char *inbox;
	int rw;
	int n;			// number of mail in inbox
	int *idx;		// array of matching mail UIDs
	int lidx;		// number of matching mail in last search

				// search criteria
	int first;		// first mail number, negative is from the last
	int last;		// last mail number, negative is from the last
	int begin;		// first mail number, from 1 to m
	int end;		// last email number, from 1 to m
	char *from;
	char *to;
	char *cc;
	char *subject;
	char *text;
	char *since;
	char *before;
	char *on;
	char *range;

				// actions
	int executeonly;	// only perform search
	int listonly;		// only list message numbers
	int structure;		// show message structure
	int body;		// show or save body
	char *prefix;		// save body to prefix.n
	int restore;		// restore flags, if not deleted
	int delete;		// delete
	int synchronous;	// synchronous delete
	char *viewer;
	char *pager;
	int pagerpipe[2];
	int pagersave;
	int *uid;		// only these uids
	int luid;
				// others
	char *search;
	char *command;
	char *automate;
	char *readfile;
	FILE *read;
	int quit;
	char *help;
	int verbose;
};

/*
 * command to string
 */
void commandtostring(char *buf, struct imapcommand *command) {
	buf[0] = '\0';

	if (command->command != NULL)
		strcpy(buf, command->command);
	else
		sprintf(buf, "UID SEARCH %d:%d%s",
			command->begin, command->end, command->search);
}

/*
 * flags to string
 */
void flagstostring(char *buf, struct imapcommand *command) {
	char lbuf[BUFLEN];

	buf[0] = '\0';
	sprintf(lbuf, "%s", command->executeonly ? "execute+" : "execute-");
	strcat(buf, lbuf);
	sprintf(lbuf, " %s", command->listonly ? "listonly+" : "listonly-");
	strcat(buf, lbuf);
	sprintf(lbuf, " %s", command->structure ? "structure+" : "structure-");
	strcat(buf, lbuf);
	sprintf(lbuf, " body%s%s", command->body ? "=" : "",
	                           command->prefix ? command->prefix : "-");
	strcat(buf, lbuf);
	sprintf(lbuf, " %s", command->restore ? "restore+" : "restore-");
	strcat(buf, lbuf);
	sprintf(lbuf, " %s", command->synchronous ?
	                     "synchronous+" : "synchronous-");
	strcat(buf, lbuf);
	sprintf(lbuf, " viewer%s%s", command->viewer ? "=" : "",
	                             command->viewer ? command->viewer : "-");
	strcat(buf, lbuf);
	sprintf(lbuf, " pager%s%s", command->pager ? "=" : "",
	                            command->pager ? command->pager : "-");
	strcat(buf, lbuf);
	sprintf(lbuf, " %s", command->verbose ? "verbose+" : "verbose-");
	strcat(buf, lbuf);
	sprintf(lbuf, " %s", command->rw ? "rw" : "ro");
	strcat(buf, lbuf);
}

/*
 * save current criteria in a file
 */
int automate(struct imapcommand *command) {
	FILE *out;

	printf("saving to %s\n", command->automate);

	out = fopen(command->automate, "w");
	if (out == NULL) {
		perror(command->automate);
		return -1;
	}

	fprintf(out, "#!/bin/sh\n# generated by imapshell\n\nimapshell");
	if (command->from != NULL)
		fprintf(out, " -f %s", command->from);
	if (command->to != NULL)
		fprintf(out, " -o %s", command->to);
	if (command->cc != NULL)
		fprintf(out, " -y %s", command->cc);
	if (command->subject != NULL)
		fprintf(out, " -s %s", command->subject);
	if (command->text != NULL)
		fprintf(out, " -t %s", command->text);
	if (command->since != NULL)
		fprintf(out, " -a %s", command->since);
	if (command->before != NULL)
		fprintf(out, " -u %s", command->before);
	if (command->on != NULL)
		fprintf(out, " -p %s", command->on);
	if (command->range != NULL)
		fprintf(out, " -r %s", command->range);
	if (command->command != NULL)
		fprintf(out, " -c %s", command->command);
	if (command->listonly)
		fprintf(out, " -l");
	if (command->executeonly)
		fprintf(out, " -e");
	if (command->structure)
		fprintf(out, " -x");
	if (command->body && command->prefix != NULL)
		fprintf(out, " -b %s", command->prefix);
	if (! command->restore)
		fprintf(out, " -m");
	if (command->delete)
		fprintf(out, " -d");
	if (command->synchronous)
		fprintf(out, " -D");
	if (server.program != NULL)
		fprintf(out, " -S %s", server.program);
	if (command->verbose)
		fprintf(out, " -V");

	fprintf(out, " -- %s", command->account);
	fprintf(out, " %d %d\n", command->first, command->last);

	fclose(out);
	return 0;
}

/*
 * restrict command by uids
 */
int stringtouid(struct imapcommand *command, char *s) {
	int i;

	i = stringindextoint(s, command->idx, command->lidx);
	if (i == 0)
		return -1;

	free(command->uid);
	command->uid = malloc(sizeof(int));
	command->uid[0] = i;
	command->luid = 1;
	return 0;
}

/*
 * print help
 */
void printhelp(char *command) {
	char *init, *bol, *eol, *c;
	int print, found;
	extern char *helptext;

	bol = strdup(helptext);
	init = bol;
	eol = strchrnul(bol, '\n');
	print = 0;
	found = 0;
	for (; bol[0] != '\0'; bol = eol + 1) {
		eol = strchrnul(bol, '\n');
		*eol = '\0';
		c = strdup(bol);
		*strchrnul(c, ' ') = '\0';
		*strchrnul(c, '\t') = '\0';
		*strchrnul(c, '+') = '\0';
		*strchrnul(c, '-') = '\0';
		if (command == NULL && bol[0] == '%' && bol[1] == '%')
			printf("%s", bol + 3);
		else if (command == NULL && bol[0] == '%' && bol[1] == ' ')
			printf("%s\n", bol + 2);
		else if (command == NULL && bol[0] == '%')
			printf("%s\n", bol + 1);
		else if (command == NULL &&
		    bol[0] != ' ' && bol[0] != '\t' &&
		    bol[0] != '%' && bol[0] != '\0')
			printf(" %s%s%s", it, c, rm);
		else if (command == NULL) {
		}
		else if (! strcmp(c, command)) {
			print = 1;
			found = 1;
		}
		else if (bol[0] != ' ' && bol[0] != '\t')
			print = 0;
		if (print)
			puts(bol);
		free(c);
	}

	if (command == NULL) {
		printf("\n");
		printf("for information on a specific command: ");
		printf("\"help command\"\n");
	}
	else if (! found)
		printf("no such command: %s\n", command);

	free(init);
}

/*
 * negative mail bounds
 */
void bounds(struct imapcommand *command) {
	command->begin = command->first > 0 ?
	                 command->first :
		         command->n + command->first + 1 < 1 ?
			 1 :
		         command->n + command->first + 1;
	command->end   = command->last > 0 ?
	                 command->last :
		         command->n + command->last < 1 ?
			 1 :
		         command->n + command->last;
}

/*
 * start paging
 */
int pagerstart(struct imapcommand *command) {
	int res;

	if (! command->pager)
		return 0;
	if ((command->body && command->prefix) || command->delete) 
		return 0;

	res = pipe(command->pagerpipe);
	if (res == -1) {
		perror("pipe");
		return -1;
	}

	if (! fork()) {
		close(command->pagerpipe[1]);
		res = dup2(command->pagerpipe[0], STDIN_FILENO);
		if (res == -1) {
			perror("dup2");
			return -1;
		}
		res = system(command->pager);
		exit(res);
	}

	command->pagersave = dup(STDOUT_FILENO);
	dup2(command->pagerpipe[1], STDOUT_FILENO);

	return 0;
}

/*
 * stop paging
 */
void pagerstop(struct imapcommand *command) {
	int res;

	if (! command->pager)
		return;
	if ((command->body && command->prefix) || command->delete) 
		return;

	fflush(stdout);
	close(command->pagerpipe[1]);
	close(STDOUT_FILENO);
	wait(&res);

	dup2(command->pagersave, STDOUT_FILENO);
	close(command->pagersave);
	command->pagersave = -1;
}

/*
 * run a command
 */
int imaprun(struct imapcommand *command) {
	char *uid;
	char buf[BUFLEN], fname[BUFLEN] = "";
	char *header;
	int begin, end;
	char *res, *cur, *next;
	int pendingenvelope = 0, pendingdelete = 0, getpending;
	int executeonly = 0;
	char *seen;
	int i, j;
	FILE *file;
	char c;

			/* search */

	if (command->uid) {
		uid = "UID ";
		begin = 0;
		end = command->luid - 1;
		cur = intarraytostring(command->uid, command->luid, ",");
		sprintf(buf, "%sFETCH %s ENVELOPE", uid, cur);
		free(cur);
	}
	else if (command->search == NULL &&
	         command->command == NULL &&
	         ! command->delete) {
		uid = "";
		free(command->idx);
		command->idx = NULL;
		begin = command->begin;
		end = command->end;
		printf("emails from %d to %d\n", begin, end);
		sprintf(buf, "%sFETCH %d:%d ENVELOPE", uid, begin, end);
	}
	else {
		commandtostring(buf, command);
		uid = strstr(buf, "UID ") == buf ? "UID " : "";
		SIMULATE_ERROR("search", buf);
		res = sendrecv(&server, buf);
		cardinality(res, &command->n);
		if (res == NULL)
			return -1;
		if (command->executeonly && ! command->verbose)
			fputs(res, stdout);
		if (strstr(res, "* TOOLARGE\n") == res) {
			printf("too many mails to show - ");
			printf("restrict by range or search criteria\n");
		}
		if (strstr(res, "* SEARCH ") != res)
			executeonly = 1;
		else {
			free(command->idx);
			command->idx = stringtointarray(res, &command->lidx);
		}
		free(res);
		if (executeonly || command->executeonly)
			return 0;

		printf("found %s%d%s mail%s", it,
		       command->lidx, rm, command->lidx > 1 ? "s" : "");
		if (command->first != 1 || command->last != command->n) {
			printf(" within %s", it);
			printf("%d-%d%s", command->first, command->last, rm);
			printf(" (%d-%d)", command->begin, command->end);
		}
		if (command->lidx == 0) {
			printf("\n");
			return -1;
		}
		printf(": ");
		for (i = 0; i < command->lidx; i++)
			printf("%d ", command->idx[i]);
		printf("\n");

		begin = 0;
		end = command->lidx - 1;
		dprintf("mail from %d", command->idx[begin]);
		dprintf(" to %d\n", command->idx[end]);
		dprintf("between %d and %d\n", command->first, command->last);
		breakloop = 0;

		cur = intarraytostring(command->idx, end + 1, ",");
		// alternative: (ENVELOPE FLAGS)
		snprintf(buf, BUFLEN, "%sFETCH %s ENVELOPE", uid, cur);
		free(cur);
	}

			/* load all envelopes at once if possible */

	if (! command->listonly && ! command->structure && ! command->body &&
	    ! (command->delete && command->idx != NULL) &&
	    command->lidx < 200) {
		SIMULATE_ERROR("fetch-all-envelopes", buf);
		res = sendrecv(&server, buf);
		cardinality(res, &command->n);
		if (res == NULL)
			return 0;
		if (command->viewer)
			for (cur = res; *cur == '*'; cur = next + 2) {
				next = strchr(cur, '\r');
				if (next == NULL)
					break;
				*next = '\0';
				external = command->viewer;
				printstringtocommand(cur);
			}
		else if (! command->verbose)
			printstringonscreen(res);
		free(res);
		return 0;
	}

			/* loop over emails */

	for (i = begin; i <= end && ! breakloop; i++) {

					/* email index */
		j = command->uid ? command->uid[i] :
		    command->idx ? command->idx[i] :
		                   i;
		header = command->listonly ? "%s %d\n" : "===== %s %d =====\n";
		printf(header, uid ? "UID" : "ID", j);
		if (command->listonly)
			continue;

					/* fetch envelope */

		if (command->synchronous || ! command->delete ||
		   command->idx == NULL || i == begin) {
			sprintf(buf, "%sFETCH %d ENVELOPE", uid, j);
			SIMULATE_ERROR("fetch-envelope", buf);
			res = sendrecv(&server, buf);
			cardinality(res, &command->n);
		}
		if (! command->synchronous && command->delete &&
		   command->idx != NULL) {
			if (i < end) {
				printstring("request next\n");
				sprintf(buf, "%sFETCH %d ENVELOPE",
				        uid, command->idx[i + 1]);
				SIMULATE_ERROR("fetch-envelope", buf);
				cnum++;
				sendcommand(&server, buf);
				pendingenvelope++;
			}
			if (i != begin) {
				printstring("receive previously requested\n");
				cnum -= pendingenvelope + pendingdelete - 1;
				res = handleanswer(recvanswer(&server), 0);
				cardinality(res, &command->n);
				cnum += pendingenvelope + pendingdelete - 1;
				pendingenvelope--;
			}
		}
		if (! command->verbose) {
			cur = strchr(res, '\n');
			if (cur && *cur != '\0')
				*(cur + 1) = '\0';
			if (! command->viewer)
				printstringonscreen(res);
			else {
				external = command->viewer;
				printstringtocommand(res);
			}
		}
		free(res);

					/* get flags */
		if (command->structure || command->body) {
			sprintf(buf, "%sFETCH %d FLAGS", uid, j);
			SIMULATE_ERROR("fetch-flags", buf);
			res = sendrecv(&server, buf);
			cardinality(res, &command->n);
			seen = strstr(res, "\\Seen");
			if (! command->verbose)
				fputs(res, stdout);
			free(res);
		}

					/* get body structure */
		if (command->structure) {
			sprintf(buf, "%sFETCH %d BODYSTRUCTURE", uid, j);
			SIMULATE_ERROR("fetch-body", buf);
			res = sendrecv(&server, buf);
			cardinality(res, &command->n);
			if (! command->verbose)
				fputs(res, stdout);
			free(res);
		}

					/* get body */
		if (command->body) {
			sprintf(buf, "%sFETCH %d BODY.PEEK[]", uid, j);
			if (command->prefix == NULL)
				res = fetch(&server, buf, stdout);
			else {
				snprintf(fname, BUFLEN, "%s.%d",
				                command->prefix, j);
				printf("content -> %s\n", fname);
				file = fopen(fname, "w");
				if (file == NULL) {
					printf("cannot open file %s\n", fname);
					continue;
				}
				res = fetch(&server, buf, file);
				fclose(file);
			}
			cardinality(res, &command->n);
			free(res);
		}

					/* delete email */
		if (command->delete && command->idx != NULL) {
			printf("delete email [yes/No/exit]? ");
			c = readchar();
			printf("\n\n");
			getpending = pendingdelete;
			switch(c) {
			case 'y':
				sprintf(buf,
					"%sSTORE %d:%d +FLAGS (\\Deleted)",
					uid, j, j);
				SIMULATE_ERROR("delete", buf);
				cnum++;
				sendcommand(&server, buf);
				pendingdelete++;
				getpending |= command->synchronous;
				break;
			case 'e':
				breakloop = 1;
			}
			if (getpending) {
				cnum -= pendingenvelope + pendingdelete - 1;
				res = handleanswer(recvanswer(&server), 0);
				cardinality(res, &command->n);
				if (res == NULL)
					printf("delete error\n");
				cnum += pendingenvelope + pendingdelete - 1;
				pendingdelete--;
			}
		}

					/* get flags again, just to be sure */
		if (command->structure || command->body) {
			sprintf(buf, "%sFETCH %d FLAGS", uid, j);
			res = sendrecv(&server, buf);
			cardinality(res, &command->n);
			free(res);
		}

					/* set flags to previous ones */
		if (command->restore && command->rw &&
		   (command->body || command->structure) &&
		   ! seen && ! command->delete) {
			/* not really necessary because of FETCH BODY.PEEK */
			sprintf(buf, "%sSTORE %d:%d -FLAGS (\\SEEN)",
				uid, j, j);
			SIMULATE_ERROR("restore-flags", buf);
			res = sendrecv(&server, buf);
			cardinality(res, &command->n);
			free(res);
		}
	}

	cnum -= pendingenvelope + pendingdelete - 1;
	for (i = 0; i < pendingenvelope + pendingdelete; i++) {
		printf("receive pending\n");
		res = handleanswer(recvanswer(&server), 0);
		cardinality(res, &command->n);
		cnum++;
	}
	cnum--;

	if (command->prefix && fname[0] != '\0') {
		printf("*** emails saved\n");
		printf("*** uncompress with munpack (unpack package)\n");
	}

	return 0;
}

/*
 * reset command before building prompt and parsing
 */
void resetcommand(struct imapcommand *command) {
	command->body = 0;
	command->delete = 0;
	free(command->command);
	command->command = NULL;
	free(command->uid);
	command->uid = NULL;
	command->luid = 0;
}

/*
 * parse command line
 * GET = list, show, save or delete
 */
enum command {OPTION, GET, REOPEN, AUTO, READ, HELP, QUIT, SYNTAX, VALUE};
enum command parse(struct imapcommand *command, char *line) {
	char *single, *s;
	int i;
	char *quit[] = {"quit", "q", "exit", "x", "finish", "done", NULL};
	char *list[] = {"list", "-", "l", "ls", "dir", "search", NULL};
	char *delete[] = {"delete", "del", "remove", "rm", NULL};
	int rw;
	int ret;

	single = strdup(line);
	*strchrnul(single, ' ') = '\0';
	*strchrnul(single, '\t') = '\0';

	s = malloc(BUFLEN);

	ret = OPTION;
	for (i = 0; quit[i] != NULL; i++)
		if (! strcmp(single, quit[i]))
			ret = QUIT;
	for (i = 0; list[i] != NULL; i++)
		if (! strcmp(single, list[i])) {
			ret = GET;
			if (2 == sscanf(line, "%s %s\n", s, s))
				if (stringtouid(command, s))
					ret = VALUE;
		}
	for (i = 0; delete[i] != NULL; i++)
		if (! strcmp(single, delete[i])) {
			command->delete = 1;
			ret = GET;
			if (2 == sscanf(line, "%s %s\n", s, s))
				if (stringtouid(command, s))
					ret = VALUE;
		}

	if (ret != OPTION) {
	}
	else if (! strcmp(single, "reopen"))
		ret = REOPEN;
	else if (1 == sscanf(line, "begin %d", &i))
		command->first = i;
	else if (1 == sscanf(line, "end %d", &i))
		command->last = i;
	else if (1 == sscanf(line, "from %s", s)) {
		free(command->from);
		command->from = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (1 == sscanf(line, "to %s", s)) {
		free(command->to);
		command->to = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (1 == sscanf(line, "cc %s", s)) {
		free(command->cc);
		command->cc = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (1 == sscanf(line, "subject %[^\r\n]", s)) {
		free(command->subject);
		command->subject = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (1 == sscanf(line, "after %s", s)) {
		free(command->since);
		command->since = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (1 == sscanf(line, "before %s", s)) {
		free(command->before);
		command->on = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (1 == sscanf(line, "on %s", s)) {
		free(command->on);
		command->on = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (1 == sscanf(line, "range %s", s)) {
		free(command->range);
		command->range = ! strcmp(s, "-") ? NULL : strdup(s);
	}
	else if (! strcmp(single, "execute+"))
		command->executeonly = 1;
	else if (! strcmp(single, "execute-"))
		command->executeonly = 0;
	else if (! strcmp(single, "execute")) {
		printhelp("execute");
		printf("currently: %s\n", command->executeonly ? "yes" : "no");
	}
	else if (! strcmp(single, "listonly")) {
		printhelp("listonly");
		printf("currently: %s\n", command->listonly ? "yes" : "no");
	}
	else if (! strcmp(single, "listonly+"))
		command->listonly = 1;
	else if (! strcmp(single, "listonly-"))
		command->listonly = 0;
	else if (! strcmp(single, "structure+"))
		command->structure = 1;
	else if (! strcmp(single, "structure-"))
		command->structure = 0;
	else if (! strcmp(single, "structure")) {
		printhelp("structure");
		printf("currently: %s\n", command->structure ? "yes" : "no");
	}
	else if (1 == sscanf(line, "show %s", s)) {
		command->body = 1;
		free(command->prefix);
		command->prefix = NULL;
		ret = GET;
		if (stringtouid(command, s))
			ret = VALUE;
	}
	else if (! strcmp(single, "show")) {
		command->body = 1;
		free(command->prefix);
		command->prefix = NULL;
		ret = GET;
	}
	else if (1 == sscanf(line, "save %s", s)) {
		command->body = 1;
		free(command->prefix);
		command->prefix = strdup(s);
		ret = GET;
		if (2 == sscanf(line, "save %s %s\n", s, s))
			if (stringtouid(command, s))
				ret = VALUE;
	}
	else if (! strcmp(single, "save")) {
		if (command->prefix == NULL)
			ret = VALUE;
		else {
			command->body = 1;
			ret = GET;
		}
	}
	else if (! strcmp(single, "restore+"))
		command->restore = 1;
	else if (! strcmp(single, "restore-"))
		command->restore = 0;
	else if (! strcmp(single, "restore")) {
		printhelp("restore");
		printf("currently: %s\n", command->restore ? "yes" : "no");
	}
	else if (1 == sscanf(line, "viewer %s", s)) {
		if (! strcmp(s, "-")) {
			free(command->viewer);
			command->viewer = NULL;
		}
		else if (! strcmp(s, "+") || ! strcmp(s, "."))
			command->viewer = strdup(DEFAULTVIEWER);
		else
			command->viewer = strdup(s);
	}
	else if (! strcmp(single, "viewer+")) {
		free(command->viewer);
		command->viewer = strdup(DEFAULTVIEWER);
	}
	else if (! strcmp(single, "viewer-")) {
		free(command->viewer);
		command->viewer = NULL;
	}
	else if (! strcmp(single, "viewer")) {
		printhelp("viewer");
		printf("currently: %s%s\n",
		        command->viewer ? "viewer " : "no viewer",
		        command->viewer ? command->viewer : "");
	}
	else if (1 == sscanf(line, "pager %[^\r\n]", s))
		if (! strcmp(s, "-")) {
			free(command->pager);
			command->pager = NULL;
		}
		else if (! strcmp(s, "+") || ! strcmp(s, "."))
			command->pager = strdup(DEFAULTPAGER);
		else
			command->pager = strdup(s);
	else if (! strcmp(single, "pager+") || ! strcmp(single, "pager.")) {
		free(command->pager);
		command->pager = strdup(DEFAULTPAGER);
	}
	else if (! strcmp(single, "pager-"))
		command->pager = NULL;
	else if (! strcmp(single, "pager")) {
		printhelp("pager");
		printf("currently: %s%s\n",
		        command->pager ? "pager " : "no pager",
		        command->pager ? command->pager : "");
	}
	else if (! strcmp(single, "synchronous+"))
		command->synchronous = 1;
	else if (! strcmp(single, "synchronous-"))
		command->synchronous = 0;
	else if (! strcmp(single, "synchronous")) {
		printhelp("synchronous");
		printf("currently: %s\n", command->synchronous ? "yes" : "no");
	}
	else if (! strcmp(single, "rw") || ! strcmp(single, "ro")) {
		rw = command->rw;
		command->rw = ! strcmp(single, "rw");
		ret = rw == command->rw ? OPTION : REOPEN;
	}
	else if (1 == sscanf(line, "command %[^\r\n]", s)) {
		command->command = strdup(s);
		ret = GET;
	}
	else if (! strcmp(single, "verbose+"))
		command->verbose = 1;
	else if (! strcmp(single, "verbose-"))
		command->verbose = 0;
	else if (! strcmp(single, "verbose")) {
		printhelp("verbose");
		printf("currently: %s\n", command->verbose ? "yes" : "no");
	}
	else if (1 == sscanf(line, "read %s", s)) {
		free(command->readfile);
		command->readfile = strdup(s);
		ret = READ;
	}
	else if (! strcmp(single, "read"))
		printhelp("read");
	else if (1 == sscanf(line, "auto %s", s) ||
	         1 == sscanf(line, "automate %s", s)) {
		free(command->automate);
		command->automate = strdup(s);
		ret = AUTO;
	}
	else if (! strcmp(single, "auto"))
		printhelp("auto");
	else if (! strcmp(single, "nop")) {
		command->command = strdup("NOOP");
		ret = GET;
	}
	else if (1 == sscanf(line, "help %s", s)) {
		free(command->help);
		command->help = strdup(s);
		ret = HELP;
	}
	else if (! strcmp(single, "help")) {
		free(command->help);
		command->help = NULL;
		ret = HELP;
	}
	else
		ret = VALUE;

	free(single);
	free(s);
	return ret;
}

/*
 * build search string 
 */
int searchstring(struct imapcommand *command) {
	command->search[0] = '\0';
	if (command->from) {
		strcat(command->search, " FROM ");
		strcat(command->search, command->from);
	}
	if (command->to) {
		strcat(command->search, " TO ");
		strcat(command->search, command->to);
	}
	if (command->uid)
		sprintf(command->search + strlen(command->search),
		        " UID %d", command->uid[0]);
	if (command->cc) {
		strcat(command->search, " CC ");
		strcat(command->search, command->cc);
	}
	if (command->subject) {
		strcat(command->search, " SUBJECT \"");
		strcat(command->search, command->subject);
		strcat(command->search, "\"");
	}
	if (command->since) {
		strcat(command->search, " SINCE ");
		strcat(command->search, command->since);
	}
	if (command->before) {
		strcat(command->search, " BEFORE ");
		strcat(command->search, command->before);
	}
	if (command->on) {
		strcat(command->search, " ON ");
		strcat(command->search, command->on);
	}
	if (command->text) {
		strcat(command->search, " TEXT ");
		strcat(command->search, command->text);
	}

	return 0;
}

/*
 * prompt
 */
char prompt[BUFLEN] = "";
char *prefix = "IMAP> ";

/*
 * the completions for a line
 */
char **completion(const char *text, int start, int end) {
	char *res, **dup;
	char *commands[] = {"list", "show", "save", "delete", "quit",
	                    "begin", "end", "from", "to", "cc", "subject",
	                    "text", "after", "before", "on", "range",
	                    "execute", "listonly", "structure", "synchronous",
			    "restore", "viewer", "pager",
			    "rw", "ro", "reopen",
	                    "nop", "command", "read", "automate",
			    "help", "verbose",
	                    NULL};
	int i;

	(void) start;
	(void) end;
	dprintf("completion(\"%s\",%d,%d)", text, start, end);

	if (start != 0)
		return NULL;

	if (text[0] == '\0') {
		printf("\n");
		printf("from to subject text after range go exit\n");
		printf("%s%s", prompt, text);
		return NULL;
	}

	for (i = 0; commands[i] != NULL; i++)
		if (strstr(commands[i], text) == commands[i]) {
			res = commands[i];
			break;
		}

	if (commands[i] == NULL) 
		return NULL;

	dprintf("completion=\"%s\"\n", res);
	dup = malloc(2 * sizeof(char *));
	dup[0] = strdup(res);
	dup[1] = NULL;

	return dup;
}

/*
 * prevent filename matching
 */
char *nofilename(const char *text, int state) {
	(void) text;
	(void) state;
	dprintf("completion_null(\"%s\",%d)", text, state);
	return NULL;
}

/*
 * main loop
 */
int loop(struct imapcommand *command) {
	char *line;
	enum command res;
	int ret = EXIT_SUCCESS;
	char buf[BUFLEN], history[BUFLEN];

	using_history();
	sprintf(history, "%s/.config/imapshell/history.txt", getenv("HOME"));
	read_history(history);
	if (command->command) {
		sprintf(buf, "command %s", command->command);
		add_history(buf);
	}
	add_history("viewer imapenvelope");
	add_history("search");
	add_history("exit");

	rl_completion_entry_function = nofilename;
	rl_attempted_completion_function = completion;

	if (command->readfile == NULL) {
		bounds(command);
		if (command->end - command->begin > 100 && ! command->quit) {
			printf("skipped initial listing - ");
			printf("too many mails for an interactive session\n");
		}
		else {
			searchstring(command);
			pagerstart(command);
			ret = imaprun(command);
			pagerstop(command);
			if (command->quit)
				return ret;
		}
	}

	for (res = OPTION; res != QUIT; ) {
		prompt[0] = '\0';
		strcat(prompt, "current parameters:\n");
		resetcommand(command);
		bounds(command);
		searchstring(command);
		commandtostring(buf, command);
		strcat(prompt, buf);
		strcat(prompt, "\n");
		flagstostring(buf, command);
		strcat(prompt, buf);
		strcat(prompt, "\n");
		strcat(prompt, prefix);
		if (command->readfile == NULL) {
			line = readline(prompt);
			if (line == NULL)
				return ret;
		}
		else if (command->readfile[0] == '\0') {
			free(command->readfile);
			command->readfile = NULL;
			continue;
		}
		else {
			if (command->read == NULL) {
				command->read = fopen(command->readfile, "r");
				if (command->read == NULL) {
					perror(command->readfile);
					logout(EXIT_FAILURE);
				}
			}
			line = fgets(buf, BUFLEN, command->read);
			if (line == NULL) {
				fclose(command->read);
				command->read = NULL;
				free(command->readfile);
				command->readfile = NULL;
				if (command->quit)
					return ret;
				else
					continue;
			}
			line = strdup(buf);
			*strchrnul(line, '\n') = '\0';
			*strchrnul(line, '#') = '\0';
			if (line[0] == '\0')
				continue;
			printf("%s", prompt);
			puts(line);
		}
		add_history(line);

		res = parse(command, line);
		bounds(command);
		searchstring(command);
		switch (res) {
		case OPTION:
		case QUIT:
			break;
		case SYNTAX:
			printf("error parsing line: %s\n", line);
			break;
		case VALUE:
			printf("invalid value in line: %s\n", line);
			break;
		case GET:
			pagerstart(command);
			ret = imaprun(command);
			pagerstop(command);
			break;
		case REOPEN:
			ret = openbox(&server, command->inbox, command->rw,
			              &command->n);
			break;
		case AUTO:
			ret = automate(command);
			break;
		case READ:
			break;
		case HELP:
			printhelp(command->help);
			break;
		}
		free(line);
	}
	
	write_history(history);
	history_truncate_file(history, 100);
	return ret;
}

/*
 * usage
 */
void usage(int ret, struct account *accts) {
	int i;
	printf("imapshell [options] -- account [first [last]]\n");
	printf("\t-f email\tonly from this sender\n");
	printf("\t-o email\tonly to this recipient\n");
	printf("\t-y email\tonly this cc recipient\n");
	printf("\t-s word\t\tonly with subject containing this whole word\n");
	printf("\t-t text\t\tonly containing this string, anywhere\n");
	printf("\t-a date\t\tonly after this date, like 12-Dec-2022\n");
	printf("\t-u date\t\tonly before this date\n");
	printf("\t-p date\t\tonly on this date\n");
	printf("\t-r range\tonly in this range, like 9120:*\n");
	printf("\t-c string\texecute an IMAP command\n");
	printf("\t-v viewer\tformat envelopes with this program\n");
	printf("\t-M pager\tshow mail listings through this program\n");
	printf("\t-w\t\tread-write mode (default is read-only)\n");
	printf("\t-e\t\tonly execute search or user-given IMAP command\n");
	printf("\t-l\t\tonly list emails\n");
	printf("\t-x\t\tenvelope, flags and structure of each email\n");
	printf("\t-b body\tsave each email n to body.n\n");
	printf("\t-d\t\tdelete emails found (with -w), after confirm\n");
	printf("\t-D\t\tsynchronous deletion\n");
	printf("\t-m\t\tdo not restore flags of email found\n");
	printf("\t-q\t\tno interactive session - end after first command\n");
	printf("\t-i\t\tlist of inboxes in the server\n");
	printf("\t-z\t\tprint account file name list of accounts\n");
	printf("\t-V\t\tverbose\n");
	printf("\t-E where\tsimulated errors, where=help for a list\n");
	printf("\t-P\t\tnot-encrypted connection\n");
	printf("\t-S script\tserver simulated by script\n");
	printf("\t-h\t\tinline help\n");
	printf("\taccount\t\taccount\n");
	printf("\tfirst\t\tfirst email, negative means from last\n");
	printf("\tlast\t\tlast email, negative means from last\n");
	if (accts == NULL)
		exit(ret);
	printf("accounts:\n");
	for (i = 0; accts[i].srv[0] != '\0'; i++)
		printf("%d: user %s, mail in %s/%s\n",
		       i, accts[i].usr, accts[i].srv, accts[i].dir);
	exit(ret);
}

/*
 * main
 */
int main(int argn, char *argv[]) {
	char *opts;
	int opt;
	struct imapcommand command;
	char buf[BUFLEN];
	int inboxes;
	struct account *account;
	int printaccounts;
	char *res;
	int ret;

			/* args */

	inboxes = 0;
	command.from = NULL;
	command.to = NULL;
	command.cc = NULL;
	command.subject = NULL;
	command.text = NULL;
	command.since = NULL;
	command.before = NULL;
	command.on = NULL;
	command.range = NULL;
	command.uid = NULL;
	command.luid = 0;
	command.search = malloc(200);
	command.command = NULL;
	command.viewer = NULL;
	command.pager = NULL;
	command.executeonly = 0;
	command.rw = 0;
	command.executeonly = 0;
	command.listonly = 0;
	command.structure = 0;
	command.body = 0;
	command.prefix = NULL;
	command.restore = 1;
	command.delete = 0;
	command.synchronous = 0;
	command.idx = NULL;
	command.lidx = 0;
	command.readfile = NULL;
	command.read = NULL;
	command.quit = 0;
	command.help = NULL;
	command.verbose = 0;
	simulate_error_where = "";
	server.fd = -1;
	server.program = NULL;
	printaccounts = 0;
	printstring = printstringnowhere;
	command.n = -1;
	opts = "f:o:y:s:u:p:t:a:r:c:v:M:ewlxb:dDizk:R:qVE:PS:h";
	while (-1 != (opt = getopt(argn, argv, opts)))
		switch(opt) {
			case 'f':
				command.from = strdup(optarg);
				break;
			case 'o':
				command.to = strdup(optarg);
				break;
			case 'y':
				command.cc = strdup(optarg);
				break;
			case 's':
				command.subject = strdup(optarg);
				break;
			case 't':
				command.text = strdup(optarg);
				break;
			case 'a':
				command.since = strdup(optarg);
				break;
			case 'u':
				command.before = strdup(optarg);
				break;
			case 'p':
				command.on = strdup(optarg);
				break;
			case 'r':
				command.range = strdup(optarg);
				break;
			case 'c':
				command.command = strdup(optarg);
				break;
			case 'v':
				command.viewer =
					strdup(! strcmp(optarg, ".") ?
						DEFAULTVIEWER : optarg);
				break;
			case 'M':
				command.pager =
					strdup(! strcmp(optarg, ".") ?
						DEFAULTPAGER : optarg);
				break;
			case 'w':
				command.rw = 1;
				break;
			case 'e':
				command.executeonly = 1;
				break;
			case 'l':
				command.listonly = 1;
				break;
			case 'x':
				command.structure = 1;
				break;
			case 'b':
				command.body = 1;
				command.prefix = 
					! strcmp(optarg, ".") ?
						NULL : strdup(optarg);
				break;
			case 'd':
				command.delete = 1;
				break;
			case 'D':
				command.delete = 1;
				command.synchronous = 1;
				break;
			case 'm':
				command.restore = 0;
				break;
			case 'i':
				inboxes = 1;
				break;
			case 'z':
				printaccounts = 1;
				break;
			case 'k':
				if (! strcmp(optarg, "")) {
					command.readfile = strdup("");
					break;
				}
				parse(&command, optarg);
				break;
			case 'R':
				command.readfile = strdup(optarg);
				break;
			case 'q':
				command.quit = 1;
				break;
			case 'V':
				command.verbose = 1;
				printstring = printstringonscreen;
				break;
			case 'P':
				server.fd = -2;
				break;
			case 'S':
				server.program =
					! strcmp(optarg, ".") ?
						DEFAULTSIMULATE : optarg;
				break;
			case 'E':
				if (! strcmp(optarg, "help")) {
					simulate_error_usage();
					return EXIT_SUCCESS;
				}
				simulate_error_where = optarg;
				break;
			case 'h':
				usage(EXIT_SUCCESS, NULL);
				break;
			default:
				usage(EXIT_FAILURE, NULL);
		}
	if (argn - 1 < optind && ! printaccounts) {
		printf("too few arguments\n");
		usage(EXIT_FAILURE, NULL);
	}

			/* color terminal */

	if (getenv("TERM") != NULL && ! strcmp(getenv("TERM"), "linux")) {
		// tgetstr, smul, rmul
		it = "\033[3m";
		rm = "\033[23m";
	}

			/* account */

	account = findaccount(printaccounts ? NULL : argv[optind]);
	if (account == NULL)
		usage(EXIT_FAILURE, NULL);
	if (printaccounts)
		return EXIT_SUCCESS;

			/* range */

	command.first = argn - 1 <= optind + 0 ? -200 : atoi(argv[optind + 1]);
	command.last =  argn - 1 <= optind + 1 ? 0 : atoi(argv[optind + 2]);

			/* password */

	printf("account %s in %s/%s\n",
	       account->usr, account->srv, account->dir);

	if (! strcmp(account->psw, "NULL")) {
		sprintf(buf, "password for %s on %s: ",
		        account->usr, account->srv);
		strcpy(account->psw, getpass(buf));
	}

			/* open socket */

	ret = imapconnect(&server, account->srv);
	if (ret == -1)
		exit(EXIT_FAILURE);
	fdgets(buf, BUFLEN - 1, &server, 0);
	printstring(buf);

			/* test connection */

	strcpy(buf, "NOOP");
	SIMULATE_ERROR("noop", buf);
	res = sendrecv(&server, buf);
	cardinality(res, &command.n);
	free(res);

			/* login */

	sprintf(buf, "LOGIN %s %s", account->usr, account->psw);
	SIMULATE_ERROR("login", buf);
	res = sendrecv(&server, buf);
	cardinality(res, &command.n);
	free(res);

			/* list inboxes */

	if (inboxes) {
		res = sendrecv(&server, "LIST \"\" *");
		SIMULATE_ERROR("list-inboxes", buf);
		if (! command.verbose)
			fputs(res, stdout);
		free(res);
		logout(EXIT_SUCCESS);
	}

			/* signal handler: exit from loop */

	signal(SIGINT, interrupt);
	signal(SIGTERM, interrupt);

			/* select inbox; OPENAS = SELECT or EXAMINE */

	command.inbox = account->dir;
	if (-1 == openbox(&server, command.inbox, command.rw, &command.n)) {
		printf("error selecting inbox\n");
		logout(EXIT_FAILURE);
	}

			/* main loop */

	loop(&command);

			/* exit */

	if (command.n == -1)
		logout(EXIT_FAILURE);
	logout(EXIT_SUCCESS);
}
