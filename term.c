/*
 * term.c
 *	Terminal program, so you can type into a serial port
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/fcntl.h>

/* Default TTY to access */
#define DEFAULT_TTY "/dev/ttyUSB0"

#define BUFSIZE (30)		/* # chars read in at a time, max */

static struct termios ntty, otty, ext;
static int baud = B9600;
static void proto_xfer();
static FILE *logfp = NULL;

static int ttyfd, child;	/* Needed for quit function */

static int parodd, pareven,	/* Parity? */
    seven_bits;			/* 7 bit format (else 8) */

static int raw_kbd = 0;		/* Don't map \r to \n on input typing? */

/*
 * Protocols to receive under
 */
#define PROTO_RX 1
#define PROTO_RY 2
#define PROTO_RZ 3
#define PROTO_TXT 4
static int proto = PROTO_RZ;
#define PROTO_CHAR '\32'	/* Control-Z starts protocol transfer */

/* For suspending reader/writer during file transfer */
static void
usr2(int dummy)
{
    signal(SIGUSR2, usr2);
}

static void
usr1(int dummy)
{
    signal(SIGUSR1, usr1);
    pause();
}

/* Entered when parent tells child to die */
void
do_term()
{
    if (logfp) {
	fclose(logfp);
    }
    exit(0);
}

/* Tell what the command line options are */
static void
usage(void)
{
    fprintf(stderr,
"Usage is: term [-eo7] [-s <speed>] [-p <protocol>] [-l <log>] [<tty>]\n");
    exit(1);
}

/* Complain about bad parity selection (both even & odd) */
static void
bad_parity(void)
{
    fprintf(stderr,
	"Can't select both even and odd parity.\n");
    usage();
}

/*
 * done()
 *	Clean up and exit
 */
static void
done(void)
{
    kill(child, SIGTERM);
    tcsetattr(ttyfd, TCSAFLUSH, &otty);
    write(ttyfd, "Exiting\n", 8);
    exit(0);
}

/*
 * setup_serial()
 *	Set serial port to our desired parameters
 *
 * We have this in a function so we can do it not only when we
 * start up, but also after protocol transfers.  It turns out
 * that the serial port might not be left in the same state it
 * started in.
 */
static void
setup_serial(rs232fd)
	int rs232fd;
{
    int fl;

    /*
     * Set up serial port for local access.  Once we've
     * set CLOCAL, turn off O_NDELAY.
     */
    tcgetattr(rs232fd, &ext);
    ext.c_lflag &= ~(ECHO|ICANON|ISIG);
    ext.c_cflag |= CLOCAL;	/* Ignore modem status */
    ext.c_cflag &= ~(CSIZE);	/* Set # bits */
    if (seven_bits) {
	ext.c_cflag |= (CS7);
    } else {
	ext.c_cflag |= (CS8);
    }
    ext.c_cflag &= ~(CSTOPB);
    if (!parodd && !pareven) {	/* Set parity */
	ext.c_cflag &= ~(PARENB);
    } else if (parodd) {
	ext.c_cflag |= (PARODD);
    }
    ext.c_oflag &= ~OPOST;
    ext.c_iflag = 0;
    ext.c_cc[VMIN] = BUFSIZE;
    ext.c_cc[VTIME] = 1;
    cfsetispeed(&ext, baud);
    cfsetospeed(&ext, baud);
    tcsetattr(rs232fd, TCSAFLUSH, &ext);

    /* Permit sleeps on I/O now that we're ignoring modem control */
    fl = fcntl(rs232fd, F_GETFL, 0);
    fl &= ~(O_NDELAY);
    (void)fcntl(rs232fd, F_SETFL, fl);
}

int
main(int argc, char **argv)
{
    int rs232, x;
    char c, *tty = DEFAULT_TTY;
    extern char *optarg;

    while ((x = getopt(argc, argv, "s:p:l:eo7r")) != -1) {
	switch (x) {

	/*
	 * Selection of baud rate
	 */
	case 's':
	    if (!strcmp(optarg, "300")) {
		baud = B300; 
	    } else if (!strcmp(optarg, "1200")) {
		baud = B1200; 
	    } else if (!strcmp(optarg, "2400")) {
		baud = B2400; 
	    } else if (!strcmp(optarg, "9600")) {
		baud = B9600; 
	    } else if (!strcmp(optarg, "19200")) {
		baud = B19200; 
	    } else if (!strcmp(optarg, "38400")) {
		baud = B38400; 
	    } else if (!strcmp(optarg, "115200")) {
		baud = B115200; 
	    } else {
		fprintf(stderr, "Illegal speed: %s\n", optarg);
		usage();
	    }
	    break;

	/*
	 * Selection of transfer protocol
	 */
	case 'p':
	    if (!strcmp(optarg, "x")) {
		proto = PROTO_RX;
	    } else if (!strcmp(optarg, "y")) {
		proto = PROTO_RY;
	    } else if (!strcmp(optarg, "z")) {
		proto = PROTO_RZ;
	    } else if (!strcmp(optarg, "txt")) {
		proto = PROTO_TXT;
	    } else {
		printf("Illegal protocol: %s\n", optarg);
		usage();
	    }
	    break;

	/* Choose a log file */
	case 'l':
	    if ((logfp = fopen(optarg, "w")) == NULL) {
		perror(optarg);
		exit(1);
	    }
	    break;

	/* Set odd parity */
	case 'o':
	    if (pareven) {
		bad_parity();
	    }
	    parodd = 1;
	    break;

	/* Set even parity */
	case 'e':
	    if (parodd) {
		bad_parity();
	    }
	    pareven = 1;
	    break;

	/* Set seven bit format (else 8) */
	case '7':
	    seven_bits = 1;
	    break;

	/* Raw keyboard */
	case 'r':
	    raw_kbd = 1;
	    break;

	default:
	    printf("Illegal option: %c\n", x);
	    usage();
	}
    }

    /* Trailing argument; terminal device */
    if (optind < argc) {
        tty = argv[optind++];
    }
    if (optind != argc) {
        printf("Trailing argument(s)\n");
        usage();
    }

    printf("Terminal starting up...\n");
    printf("Use ^Z-q (control-Z, followed by q) to quit.\n");

    /*
     * The next bit of code makes assumptions about
     *  how UNIX manages its file descriptors.  The goal
     *  is to make the terminal device be our standard
     *  input & output.
     */
    ttyfd = dup(1);
    close(0); 
    close(1);
    if ((rs232 = open(tty, O_RDWR|O_EXCL|O_NDELAY)) < 0) {
	perror(tty);
	exit(1);
    }
    dup(rs232);

    /*
     * Set up for raw TTY I/O
     */
    tcgetattr(ttyfd, &otty);
    ntty = otty;
    ntty.c_lflag &= ~(ECHO|ICANON|ISIG);
    ntty.c_oflag &= ~OPOST;
    ntty.c_iflag = 0;
    ntty.c_cc[VMIN] = 1;
    ntty.c_cc[VTIME] = 0;
    tcsetattr(ttyfd, TCSAFLUSH, &ntty);

    setup_serial(rs232);

    if ((child = fork()) == 0) {
	static char boot_msg[] = "Term ready.\r\n";

	/*
	 * This is the code which posts reads to the serial port
	 * and then writes them on to the user (and, if -l was
	 * used, to the log file too)
	 */
	signal(SIGUSR1, usr1);
	signal(SIGUSR2, usr2);
	signal(SIGTERM, do_term);
	write(ttyfd, boot_msg, sizeof(boot_msg)-1);
	for (;;) {
	    char *p, *q, buf[BUFSIZE];

	    if ((x = read(rs232, buf, sizeof(buf))) < 0) {
		if (errno == EINTR)
		    continue;
		perror("child");
		exit(1);
	    }
	    p = buf; 
	    q = buf+x;
	    while (p < q) {
		*p++ &= 0x7F;
	    }
	    write(ttyfd, buf, x);
	    if (logfp) {
		fwrite(buf, sizeof(char), x, logfp);
	    }
	}
    }

    if (child < 0) {
	perror("child fork");
	exit(1);
    }

    /*
     * This is the code which posts reads to the user's keyboard and
     * writes the data out to the serial port.
     */
    while ((x = read(ttyfd, &c, 1)) == 1) {
	c &= 0x7F;

	/* PROTO_CHAR (usually Control-Z) starts file transfers */
	if (c == PROTO_CHAR) {
	    kill(child, SIGUSR1);
	    proto_xfer(ttyfd, rs232);
	    kill(child, SIGUSR2);
	    continue;
	}
	if (!raw_kbd && (c == '\n')) {
	    c = '\r';
	}
	write(rs232, &c, 1);
    }
    if (x < 0) {
	perror("parent");
    }
    done();
    /*NOTREACHED*/
    return(0);
}

/*
 * Ask for and receive a line in raw mode
 */
static void
prompt_read(int ttyfd, char *prompt, char *buf, int len)
{
    char c;

    /* Leave room for null terminator */
    len -= 1;

    /* Prompt */
    write(ttyfd, prompt, strlen(prompt));

    /* Read chars until newline */
    do {
	while (read(ttyfd, &c, sizeof(c)) == 0) ;
	c &= 0x7F;
	write(ttyfd, &c, sizeof(c));
	if (len) {
	    *buf++ = c;
	    --len;
	}
    } while ((c != '\n') && (c != '\r'));

    /* Overwrite newline with terminator */
    buf -= 1;
    *buf = '\0';
}

/*
 * Execute a protocol receive
 */
static void
rx_xfer(int ttyfd)
{
    char buf[80];
    char fname[60];

    switch (proto) {

    case PROTO_RX:
	/*
	 * Xmodem doesn't send names, so we have to ask.  Bleh.
	 */
	prompt_read(ttyfd, "Receive file: ", fname, sizeof(fname));
	sprintf(buf, "lrx %s", fname);
	break;

    case PROTO_RY:
	strcpy(buf, "lry");
	break;

    case PROTO_RZ:
	strcpy(buf, "lrz");
	break;

    default:
	fprintf(stderr, "Receive not supported with this protocol.\r\n");
	return;
    }
    system(buf);
}

/*
 * Execute a protocol transmit
 */
static void
tx_xfer(int ttyfd)
{
    char buf[80];
    char fname[60];

    prompt_read(ttyfd, "Send file: ", fname, sizeof(fname));

    switch (proto) {

    case PROTO_RX:
	strcpy(buf, "lsx");
	break;

    case PROTO_RY:
	strcpy(buf, "lsy");
	break;

    case PROTO_RZ:
	strcpy(buf, "lsz");
	break;

    case PROTO_TXT:
	strcpy(buf, "cat");
	break;

    default:
	fprintf(stderr, "Transmit not supported with this protocol.\r\n");
	return;
    }

    sprintf(buf, "%s %s", buf, fname);
    system(buf);
}

/*
 * Figure out what they want to do, drive a protcol transfer
 */
static void
proto_xfer(int ttyfd, int rs232)
{
    char c;
    register char c2;
    static char helpmsg[] = "Options are: <r>eceive, <s>end, <q>uit\r\n";

    /* Get next char to see what they want to do */
    while (read(ttyfd, &c, sizeof(c)) == 0)
	    ;
    c &= 0x7F;

    /* Send char through literally */
    if (c == PROTO_CHAR) {
	write(rs232, &c, sizeof(c));
	return;
    }

    /* 'q' or 'Q' means quit */
    if ((c == 'q') || (c == 'Q')) {
	done();
    }

    /* Receive? */
    c2 = c;
    if ((c2 == 'r') || (c2 == 'R')) {
	rx_xfer(ttyfd);
	setup_serial(rs232);

    /* Send? */
    } else if ((c2 == 's') || (c2 == 'S') || (c2 == 't') ||
	    (c2 == 'T')) {
	tx_xfer(ttyfd);
	setup_serial(rs232);

    /* Dunno */
    } else {
	write(ttyfd, helpmsg, sizeof(helpmsg)-1);
    }
}
