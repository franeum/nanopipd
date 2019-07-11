/* Copyright (c) 1997-1999 Miller Puckette.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* Pd side of the Pd/Pd-gui interface.  Also, some system interface routines
that didn't really belong anywhere. */

#include "m_pd.h"
#include "s_stuff.h"
#include "m_imp.h"
#include "g_canvas.h"   /* for GUI queueing stuff */
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif
#ifdef HAVE_BSTRING_H
#include <bstring.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <winsock.h>
#include <windows.h>
typedef int socklen_t;
#define EADDRINUSE WSAEADDRINUSE
#endif

#include <stdarg.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/stat.h>
#include <glob.h>
#else
#include <stdlib.h>
#endif

#define stringify(s) str(s)
#define str(s) #s

#define DEBUG_MESSUP 1      /* messages up from pd to pd-gui */
#define DEBUG_MESSDOWN 2    /* messages down from pd-gui to pd */

#ifndef PDBINDIR
#define PDBINDIR "bin/"
#endif

#ifndef PDGUIDIR
#define PDGUIDIR "tcl/"
#endif

#ifndef WISH
# if defined _WIN32
#  define WISH "wish85.exe"
# elif defined __APPLE__
   // leave undefined to use dummy search path, otherwise
   // this should be a full path to wish on mac
#else
#  define WISH "wish"
# endif
#endif

#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)
#define LOCALHOST "127.0.0.1"
#else
#define LOCALHOST "localhost"
#endif

#if PDTHREADS
#include "pthread.h"
#endif

typedef struct _fdpoll
{
    int fdp_fd;
    t_fdpollfn fdp_fn;
    void *fdp_ptr;
} t_fdpoll;

#define INBUFSIZE 4096

struct _socketreceiver
{
    char *sr_inbuf;
    int sr_inhead;
    int sr_intail;
    void *sr_owner;
    int sr_udp;
    t_socketnotifier sr_notifier;
    t_socketreceivefn sr_socketreceivefn;
};

typedef struct _guiqueue
{
    void *gq_client;
    t_glist *gq_glist;
    t_guicallbackfn gq_fn;
    struct _guiqueue *gq_next;
} t_guiqueue;

struct _instanceinter
{
    int i_havegui;
    int i_nfdpoll;
    t_fdpoll *i_fdpoll;
    int i_maxfd;
    int i_guisock;
    t_socketreceiver *i_socketreceiver;
    t_guiqueue *i_guiqueuehead;
    t_binbuf *i_inbinbuf;
    char *i_guibuf;
    int i_guihead;
    int i_guitail;
    int i_guisize;
    int i_waitingforping;
    int i_bytessincelastping;

#ifdef _WIN32
    LARGE_INTEGER i_inittime;
    double i_freq;
#endif
#if PDTHREADS
    pthread_mutex_t i_mutex;
#endif
};

extern int sys_guisetportnumber;
extern int sys_addhist(int phase);
void sys_set_searchpath(void);
void sys_set_temppath(void);
void sys_set_extrapath(void);
void sys_set_startup(void);
void sys_stopgui( void);

/* ----------- functions for timing, signals, priorities, etc  --------- */

#ifdef _WIN32

static void sys_initntclock(void)
{
    LARGE_INTEGER f1;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!QueryPerformanceFrequency(&f1))
    {
          fprintf(stderr, "pd: QueryPerformanceFrequency failed\n");
          f1.QuadPart = 1;
    }
    pd_this->pd_inter->i_freq = f1.QuadPart;
    pd_this->pd_inter->i_inittime = now;
}

#if 0
    /* this is a version you can call if you did the QueryPerformanceCounter
    call yourself.  Necessary for time tagging incoming MIDI at interrupt
    level, for instance; but we're not doing that just now. */

double nt_tixtotime(LARGE_INTEGER *dumbass)
{
    if (pd_this->pd_inter->i_freq == 0) sys_initntclock();
    return (((double)(dumbass->QuadPart -
        pd_this->pd_inter->i_inittime.QuadPart)) / pd_this->pd_inter->i_freq);
}
#endif
#endif /* _WIN32 */

    /* get "real time" in seconds; take the
    first time we get called as a reference time of zero. */
double sys_getrealtime(void)
{
#ifndef _WIN32
    static struct timeval then;
    struct timeval now;
    gettimeofday(&now, 0);
    if (then.tv_sec == 0 && then.tv_usec == 0) then = now;
    return ((now.tv_sec - then.tv_sec) +
        (1./1000000.) * (now.tv_usec - then.tv_usec));
#else
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (pd_this->pd_inter->i_freq == 0) sys_initntclock();
    return (((double)(now.QuadPart -
        pd_this->pd_inter->i_inittime.QuadPart)) / pd_this->pd_inter->i_freq);
#endif
}

extern int sys_nosleep;

static int sys_domicrosleep(int microsec, int pollem)
{
    struct timeval timout;
    int i, didsomething = 0;
    t_fdpoll *fp;
    timout.tv_sec = 0;
    timout.tv_usec = (sys_nosleep ? 0 : microsec);
    if (pollem)
    {
        fd_set readset, writeset, exceptset;
        FD_ZERO(&writeset);
        FD_ZERO(&readset);
        FD_ZERO(&exceptset);
        for (fp = pd_this->pd_inter->i_fdpoll,
            i = pd_this->pd_inter->i_nfdpoll; i--; fp++)
                FD_SET(fp->fdp_fd, &readset);
#ifdef _WIN32
        if (pd_this->pd_inter->i_maxfd == 0)
                Sleep(microsec/1000);
        else
#endif
        if(select(pd_this->pd_inter->i_maxfd+1,
                  &readset, &writeset, &exceptset, &timout) < 0)
          perror("microsleep select");
        for (i = 0; i < pd_this->pd_inter->i_nfdpoll; i++)
            if (FD_ISSET(pd_this->pd_inter->i_fdpoll[i].fdp_fd, &readset))
        {
#ifdef THREAD_LOCKING
            sys_lock();
#endif
            (*pd_this->pd_inter->i_fdpoll[i].fdp_fn)
                (pd_this->pd_inter->i_fdpoll[i].fdp_ptr,
                    pd_this->pd_inter->i_fdpoll[i].fdp_fd);
#ifdef THREAD_LOCKING
            sys_unlock();
#endif
            didsomething = 1;
        }
        return (didsomething);
    }
    else
    {
#ifdef _WIN32
        if (pd_this->pd_inter->i_maxfd == 0)
              Sleep(microsec/1000);
        else
#endif
        select(0, 0, 0, 0, &timout);
        return (0);
    }
}

void sys_microsleep(int microsec)
{
    sys_domicrosleep(microsec, 1);
}

#if !defined(_WIN32) && !defined(__CYGWIN__)
static void sys_signal(int signo, sig_t sigfun)
{
    struct sigaction action;
    action.sa_flags = 0;
    action.sa_handler = sigfun;
    memset(&action.sa_mask, 0, sizeof(action.sa_mask));
#if 0  /* GG says: don't use that */
    action.sa_restorer = 0;
#endif
    if (sigaction(signo, &action, 0) < 0)
        perror("sigaction");
}

static void sys_exithandler(int n)
{
    static int trouble = 0;
    if (!trouble)
    {
        trouble = 1;
        fprintf(stderr, "Pd: signal %d\n", n);
        sys_bail(1);
    }
    else _exit(1);
}

static void sys_alarmhandler(int n)
{
    fprintf(stderr, "Pd: system call timed out\n");
}

static void sys_huphandler(int n)
{
    struct timeval timout;
    timout.tv_sec = 0;
    timout.tv_usec = 30000;
    select(1, 0, 0, 0, &timout);
}

void sys_setalarm(int microsec)
{
    struct itimerval gonzo;
    int sec = (int)(microsec/1000000);
    microsec %= 1000000;
#if 0
    fprintf(stderr, "timer %d:%d\n", sec, microsec);
#endif
    gonzo.it_interval.tv_sec = 0;
    gonzo.it_interval.tv_usec = 0;
    gonzo.it_value.tv_sec = sec;
    gonzo.it_value.tv_usec = microsec;
    if (microsec)
        sys_signal(SIGALRM, sys_alarmhandler);
    else sys_signal(SIGALRM, SIG_IGN);
    setitimer(ITIMER_REAL, &gonzo, 0);
}

#endif /* NOT _WIN32 && NOT __CYGWIN__ */

    /* on startup, set various signal handlers */
void sys_setsignalhandlers( void)
{
#if !defined(_WIN32) && !defined(__CYGWIN__)
    signal(SIGHUP, sys_huphandler);
    signal(SIGINT, sys_exithandler);
    signal(SIGQUIT, sys_exithandler);
    signal(SIGILL, sys_exithandler);
# ifdef SIGIOT
    signal(SIGIOT, sys_exithandler);
# endif
    signal(SIGFPE, SIG_IGN);
    /* signal(SIGILL, sys_exithandler);
    signal(SIGBUS, sys_exithandler);
    signal(SIGSEGV, sys_exithandler); */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, SIG_IGN);
#if 0  /* GG says: don't use that */
    signal(SIGSTKFLT, sys_exithandler);
#endif
#endif /* NOT _WIN32 && NOT __CYGWIN__ */
}

#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)

#if defined(_POSIX_PRIORITY_SCHEDULING) || defined(_POSIX_MEMLOCK)
#include <sched.h>
#endif

#define MODE_NRT 0
#define MODE_RT 1
#define MODE_WATCHDOG 2

void sys_set_priority(int mode)
{
#ifdef _POSIX_PRIORITY_SCHEDULING
    struct sched_param par;
    int p1, p2, p3;
    p1 = sched_get_priority_min(SCHED_FIFO);
    p2 = sched_get_priority_max(SCHED_FIFO);
#ifdef USEAPI_JACK
    p3 = (mode == MODE_WATCHDOG ? p1 + 7 : (mode == MODE_RT ? p1 + 5 : 0));
#else
    p3 = (mode == MODE_WATCHDOG ? p2 - 5 : (mode == MODE_RT ? p2 - 7 : 0));
#endif
    par.sched_priority = p3;
    if (sched_setscheduler(0,
        (mode == MODE_NRT ? SCHED_OTHER : SCHED_FIFO), &par) < 0)
    {
        if (mode == MODE_WATCHDOG)
            fprintf(stderr, "priority %d scheduling failed.\n", p3);
        else post("priority %d scheduling failed; running at normal priority",
                p3);
    }
    else if (sys_verbose)
    {
        if (mode == MODE_RT)
            post("priority %d scheduling enabled.\n", p3);
        else post("running at normal (non-real-time) priority.\n");
    }
#endif

#if !defined(USEAPI_JACK)
    if (mode != MODE_NRT)
    {
            /* tb: force memlock to physical memory { */
        struct rlimit mlock_limit;
        mlock_limit.rlim_cur=0;
        mlock_limit.rlim_max=0;
        setrlimit(RLIMIT_MEMLOCK,&mlock_limit);
            /* } tb */
        if (mlockall(MCL_FUTURE) != -1 && sys_verbose)
            fprintf(stderr, "memory locking enabled.\n");
    }
    else munlockall();
#endif
}

#endif /* __linux__ */

/* ------------------ receiving incoming messages over sockets ------------- */

void sys_sockerror(char *s)
{
#ifdef _WIN32
    int err = WSAGetLastError();
    if (err == 10054) return;
    else if (err == 10044)
    {
        fprintf(stderr,
            "Warning: you might not have TCP/IP \"networking\" turned on\n");
        fprintf(stderr, "which is needed for Pd to talk to its GUI layer.\n");
    }
#else
    int err = errno;
#endif
    error("%s: %s (%d)\n", s, strerror(err), err);
}

void sys_addpollfn(int fd, t_fdpollfn fn, void *ptr)
{
    int nfd, size;
    t_fdpoll *fp;
    sys_init_fdpoll();
    nfd = pd_this->pd_inter->i_nfdpoll;
    size = nfd * sizeof(t_fdpoll);
    pd_this->pd_inter->i_fdpoll = (t_fdpoll *)t_resizebytes(
        pd_this->pd_inter->i_fdpoll, size, size + sizeof(t_fdpoll));
    fp = pd_this->pd_inter->i_fdpoll + nfd;
    fp->fdp_fd = fd;
    fp->fdp_fn = fn;
    fp->fdp_ptr = ptr;
    pd_this->pd_inter->i_nfdpoll = nfd + 1;
    if (fd >= pd_this->pd_inter->i_maxfd)
        pd_this->pd_inter->i_maxfd = fd + 1;
}

void sys_rmpollfn(int fd)
{
    int nfd = pd_this->pd_inter->i_nfdpoll;
    int i, size = nfd * sizeof(t_fdpoll);
    t_fdpoll *fp;
    for (i = nfd, fp = pd_this->pd_inter->i_fdpoll; i--; fp++)
    {
        if (fp->fdp_fd == fd)
        {
            while (i--)
            {
                fp[0] = fp[1];
                fp++;
            }
            pd_this->pd_inter->i_fdpoll = (t_fdpoll *)t_resizebytes(
                pd_this->pd_inter->i_fdpoll, size, size - sizeof(t_fdpoll));
            pd_this->pd_inter->i_nfdpoll = nfd - 1;
            return;
        }
    }
    post("warning: %d removed from poll list but not found", fd);
}

t_socketreceiver *socketreceiver_new(void *owner, t_socketnotifier notifier,
    t_socketreceivefn socketreceivefn, int udp)
{
    t_socketreceiver *x = (t_socketreceiver *)getbytes(sizeof(*x));
    x->sr_inhead = x->sr_intail = 0;
    x->sr_owner = owner;
    x->sr_notifier = notifier;
    x->sr_socketreceivefn = socketreceivefn;
    x->sr_udp = udp;
    if (!(x->sr_inbuf = malloc(INBUFSIZE))) bug("t_socketreceiver");;
    return (x);
}

void socketreceiver_free(t_socketreceiver *x)
{
    free(x->sr_inbuf);
    freebytes(x, sizeof(*x));
}

    /* this is in a separately called subroutine so that the buffer isn't
    sitting on the stack while the messages are getting passed. */
static int socketreceiver_doread(t_socketreceiver *x)
{
    char messbuf[INBUFSIZE], *bp = messbuf;
    int indx, first = 1;
    int inhead = x->sr_inhead;
    int intail = x->sr_intail;
    char *inbuf = x->sr_inbuf;
    for (indx = intail; first || (indx != inhead);
        first = 0, (indx = (indx+1)&(INBUFSIZE-1)))
    {
            /* if we hit a semi that isn't preceded by a \, it's a message
            boundary.  LATER we should deal with the possibility that the
            preceding \ might itself be escaped! */
        char c = *bp++ = inbuf[indx];
        if (c == ';' && (!indx || inbuf[indx-1] != '\\'))
        {
            intail = (indx+1)&(INBUFSIZE-1);
            binbuf_text(pd_this->pd_inter->i_inbinbuf, messbuf, bp - messbuf);
            if (sys_debuglevel & DEBUG_MESSDOWN)
            {
                write(2,  messbuf, bp - messbuf);
                write(2, "\n", 1);
            }
            x->sr_inhead = inhead;
            x->sr_intail = intail;
            return (1);
        }
    }
    return (0);
}

static void socketreceiver_getudp(t_socketreceiver *x, int fd)
{
    char buf[INBUFSIZE+1];
    int ret = (int)recv(fd, buf, INBUFSIZE, 0);
    if (ret < 0)
    {
        sys_sockerror("recv");
        sys_rmpollfn(fd);
        sys_closesocket(fd);
    }
    else if (ret > 0)
    {
        buf[ret] = 0;
#if 0
        post("%s", buf);
#endif
        if (buf[ret-1] != '\n')
        {
#if 0
            buf[ret] = 0;
            error("dropped bad buffer %s\n", buf);
#endif
        }
        else
        {
            char *semi = strchr(buf, ';');
            if (semi)
                *semi = 0;
            binbuf_text(pd_this->pd_inter->i_inbinbuf, buf, strlen(buf));
            outlet_setstacklim();
            if (x->sr_socketreceivefn)
                (*x->sr_socketreceivefn)(x->sr_owner,
                    pd_this->pd_inter->i_inbinbuf);
            else bug("socketreceiver_getudp");
        }
    }
}

void sys_exit(void);

void socketreceiver_read(t_socketreceiver *x, int fd)
{
    if (x->sr_udp)   /* UDP ("datagram") socket protocol */
        socketreceiver_getudp(x, fd);
    else  /* TCP ("streaming") socket protocol */
    {
        char *semi;
        int readto =
            (x->sr_inhead >= x->sr_intail ? INBUFSIZE : x->sr_intail-1);
        int ret;

            /* the input buffer might be full.  If so, drop the whole thing */
        if (readto == x->sr_inhead)
        {
            fprintf(stderr, "pd: dropped message from gui\n");
            x->sr_inhead = x->sr_intail = 0;
            readto = INBUFSIZE;
        }
        else
        {
            ret = (int)recv(fd, x->sr_inbuf + x->sr_inhead,
                readto - x->sr_inhead, 0);
            if (ret <= 0)
            {
                if (ret < 0)
                    sys_sockerror("recv");
                if (x == pd_this->pd_inter->i_socketreceiver)
                {
                    if (pd_this == &pd_maininstance)
                        sys_bail(1);
                    else
                    {
                        sys_rmpollfn(fd);
                        sys_closesocket(fd);
                        sys_stopgui();
                    }
                }
                else
                {
                    if (x->sr_notifier)
                        (*x->sr_notifier)(x->sr_owner, fd);
                    sys_rmpollfn(fd);
                    sys_closesocket(fd);
                }
            }
            else
            {
                x->sr_inhead += ret;
                if (x->sr_inhead >= INBUFSIZE) x->sr_inhead = 0;
                while (socketreceiver_doread(x))
                {
                    outlet_setstacklim();
                    if (x->sr_socketreceivefn)
                        (*x->sr_socketreceivefn)(x->sr_owner,
                            pd_this->pd_inter->i_inbinbuf);
                    else binbuf_eval(pd_this->pd_inter->i_inbinbuf, 0, 0, 0);
                    if (x->sr_inhead == x->sr_intail)
                        break;
                }
            }
        }
    }
}

void sys_closesocket(int fd)
{
#ifdef HAVE_UNISTD_H
    if(fd<0)
        return;
    close(fd);
#endif
#ifdef _WIN32
    closesocket(fd);
#endif
}

/* ---------------------- sending messages to the GUI ------------------ */
#define GUI_ALLOCCHUNK 8192
#define GUI_UPDATESLICE 512 /* how much we try to do in one idle period */
#define GUI_BYTESPERPING 1024 /* how much we send up per ping */

static void sys_trytogetmoreguibuf(int newsize)
{
    char *newbuf = realloc(pd_this->pd_inter->i_guibuf, newsize);
#if 0
    static int sizewas;
    if (newsize > 70000 && sizewas < 70000)
    {
        int i;
        for (i = pd_this->pd_inter->i_guitail;
            i < pd_this->pd_inter->i_guihead; i++)
                fputc(pd_this->pd_inter->i_guibuf[i], stderr);
    }
    sizewas = newsize;
#endif
#if 0
    fprintf(stderr, "new size %d (head %d, tail %d)\n",
        newsize, pd_this->pd_inter->i_guihead, pd_this->pd_inter->i_guitail);
#endif

        /* if realloc fails, make a last-ditch attempt to stay alive by
        synchronously writing out the existing contents.  LATER test
        this by intentionally setting newbuf to zero */
    if (!newbuf)
    {
        int bytestowrite = pd_this->pd_inter->i_guitail -
            pd_this->pd_inter->i_guihead;
        int written = 0;
        while (1)
        {
            int res = (int)send(pd_this->pd_inter->i_guisock,
                pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guitail +
                    written, bytestowrite, 0);
            if (res < 0)
            {
                perror("pd output pipe");
                sys_bail(1);
            }
            else
            {
                written += res;
                if (written >= bytestowrite)
                    break;
            }
        }
        pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    }
    else
    {
        pd_this->pd_inter->i_guisize = newsize;
        pd_this->pd_inter->i_guibuf = newbuf;
    }
}

int sys_havegui( void)
{
    return (pd_this->pd_inter->i_havegui);
}

void sys_vgui(char *fmt, ...)
{
    int msglen, bytesleft, headwas, nwrote;
    va_list ap;

    if (!sys_havegui())
        return;
    if (!pd_this->pd_inter->i_guibuf)
    {
        if (!(pd_this->pd_inter->i_guibuf = malloc(GUI_ALLOCCHUNK)))
        {
            fprintf(stderr, "Pd: couldn't allocate GUI buffer\n");
            sys_bail(1);
        }
        pd_this->pd_inter->i_guisize = GUI_ALLOCCHUNK;
        pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    }
    if (pd_this->pd_inter->i_guihead > pd_this->pd_inter->i_guisize -
        (GUI_ALLOCCHUNK/2))
            sys_trytogetmoreguibuf(pd_this->pd_inter->i_guisize +
                GUI_ALLOCCHUNK);
    va_start(ap, fmt);
    msglen = vsnprintf(pd_this->pd_inter->i_guibuf +
        pd_this->pd_inter->i_guihead,
            pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead,
                fmt, ap);
    va_end(ap);
    if(msglen < 0)
    {
        fprintf(stderr,
            "Pd: buffer space wasn't sufficient for long GUI string\n");
        return;
    }
    if (msglen >= pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead)
    {
        int msglen2, newsize = pd_this->pd_inter->i_guisize + 1 +
            (msglen > GUI_ALLOCCHUNK ? msglen : GUI_ALLOCCHUNK);
        sys_trytogetmoreguibuf(newsize);

        va_start(ap, fmt);
        msglen2 = vsnprintf(pd_this->pd_inter->i_guibuf +
            pd_this->pd_inter->i_guihead,
                pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead,
                    fmt, ap);
        va_end(ap);
        if (msglen2 != msglen)
            bug("sys_vgui");
        if (msglen >= pd_this->pd_inter->i_guisize -
            pd_this->pd_inter->i_guihead) msglen =
                pd_this->pd_inter->i_guisize - pd_this->pd_inter->i_guihead;
    }
    if (sys_debuglevel & DEBUG_MESSUP)
        fprintf(stderr, "%s",
            pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guihead);
    pd_this->pd_inter->i_guihead += msglen;
    pd_this->pd_inter->i_bytessincelastping += msglen;
}

void sys_gui(char *s)
{
    sys_vgui("%s", s);
}

static int sys_flushtogui(void)
{
    int writesize = pd_this->pd_inter->i_guihead - pd_this->pd_inter->i_guitail,
        nwrote = 0;
    if (writesize > 0)
        nwrote = (int)send(pd_this->pd_inter->i_guisock,
            pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guitail,
                writesize, 0);

#if 0
    if (writesize)
        fprintf(stderr, "wrote %d of %d\n", nwrote, writesize);
#endif

    if (nwrote < 0)
    {
        perror("pd-to-gui socket");
        sys_bail(1);
    }
    else if (!nwrote)
        return (0);
    else if (nwrote >= pd_this->pd_inter->i_guihead -
        pd_this->pd_inter->i_guitail)
            pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    else if (nwrote)
    {
        pd_this->pd_inter->i_guitail += nwrote;
        if (pd_this->pd_inter->i_guitail > (pd_this->pd_inter->i_guisize >> 2))
        {
            memmove(pd_this->pd_inter->i_guibuf,
                pd_this->pd_inter->i_guibuf + pd_this->pd_inter->i_guitail,
                    pd_this->pd_inter->i_guihead -
                        pd_this->pd_inter->i_guitail);
            pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guihead -
                pd_this->pd_inter->i_guitail;
            pd_this->pd_inter->i_guitail = 0;
        }
    }
    return (1);
}

void glob_ping(t_pd *dummy)
{
    pd_this->pd_inter->i_waitingforping = 0;
}

static int sys_flushqueue(void )
{
    int wherestop = pd_this->pd_inter->i_bytessincelastping + GUI_UPDATESLICE;
    if (wherestop + (GUI_UPDATESLICE >> 1) > GUI_BYTESPERPING)
        wherestop = 0x7fffffff;
    if (pd_this->pd_inter->i_waitingforping)
        return (0);
    if (!pd_this->pd_inter->i_guiqueuehead)
        return (0);
    while (1)
    {
        if (pd_this->pd_inter->i_bytessincelastping >= GUI_BYTESPERPING)
        {
            sys_gui("pdtk_ping\n");
            pd_this->pd_inter->i_bytessincelastping = 0;
            pd_this->pd_inter->i_waitingforping = 1;
            return (1);
        }
        if (pd_this->pd_inter->i_guiqueuehead)
        {
            t_guiqueue *headwas = pd_this->pd_inter->i_guiqueuehead;
            pd_this->pd_inter->i_guiqueuehead = headwas->gq_next;
            (*headwas->gq_fn)(headwas->gq_client, headwas->gq_glist);
            t_freebytes(headwas, sizeof(*headwas));
            if (pd_this->pd_inter->i_bytessincelastping >= wherestop)
                break;
        }
        else break;
    }
    sys_flushtogui();
    return (1);
}

    /* flush output buffer and update queue to gui in small time slices */
static int sys_poll_togui(void) /* returns 1 if did anything */
{
    if (!sys_havegui())
        return (0);
        /* in case there is stuff still in the buffer, try to flush it. */
    sys_flushtogui();
        /* if the flush wasn't complete, wait. */
    if (pd_this->pd_inter->i_guihead > pd_this->pd_inter->i_guitail)
        return (0);

        /* check for queued updates */
    if (sys_flushqueue())
        return (1);

    return (0);
}

    /* if some GUI object is having to do heavy computations, it can tell
    us to back off from doing more updates by faking a big one itself. */
void sys_pretendguibytes(int n)
{
    pd_this->pd_inter->i_bytessincelastping += n;
}

void sys_queuegui(void *client, t_glist *glist, t_guicallbackfn f)
{
    t_guiqueue **gqnextptr, *gq;
    if (!pd_this->pd_inter->i_guiqueuehead)
        gqnextptr = &pd_this->pd_inter->i_guiqueuehead;
    else
    {
        for (gq = pd_this->pd_inter->i_guiqueuehead; gq->gq_next;
            gq = gq->gq_next)
                if (gq->gq_client == client)
                    return;
        if (gq->gq_client == client)
            return;
        gqnextptr = &gq->gq_next;
    }
    gq = t_getbytes(sizeof(*gq));
    gq->gq_next = 0;
    gq->gq_client = client;
    gq->gq_glist = glist;
    gq->gq_fn = f;
    gq->gq_next = 0;
    *gqnextptr = gq;
}

void sys_unqueuegui(void *client)
{
    t_guiqueue *gq, *gq2;
    while (pd_this->pd_inter->i_guiqueuehead &&
        pd_this->pd_inter->i_guiqueuehead->gq_client == client)
    {
        gq = pd_this->pd_inter->i_guiqueuehead;
        pd_this->pd_inter->i_guiqueuehead =
            pd_this->pd_inter->i_guiqueuehead->gq_next;
        t_freebytes(gq, sizeof(*gq));
    }
    if (!pd_this->pd_inter->i_guiqueuehead)
        return;
    for (gq = pd_this->pd_inter->i_guiqueuehead; (gq2 = gq->gq_next); gq = gq2)
        if (gq2->gq_client == client)
    {
        gq->gq_next = gq2->gq_next;
        t_freebytes(gq2, sizeof(*gq2));
        break;
    }
}

int sys_pollgui(void)
{
    return (sys_domicrosleep(0, 1) || sys_poll_togui());
}

void sys_init_fdpoll(void)
{
    if (pd_this->pd_inter->i_fdpoll)
        return;
    /* create an empty FD poll list */
    pd_this->pd_inter->i_fdpoll = (t_fdpoll *)t_getbytes(0);
    pd_this->pd_inter->i_nfdpoll = 0;
    pd_this->pd_inter->i_inbinbuf = binbuf_new();
}

/* --------------------- starting up the GUI connection ------------- */

static int sys_watchfd;

#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)
void glob_watchdog(t_pd *dummy)
{
    if (write(sys_watchfd, "\n", 1) < 1)
    {
        fprintf(stderr, "pd: watchdog process died\n");
        sys_bail(1);
    }
}
#endif

static void sys_init_deken( void)
{
    const char*os =
#if defined __linux__
        "Linux"
#elif defined __APPLE__
        "Darwin"
#elif defined __FreeBSD__
        "FreeBSD"
#elif defined __NetBSD__
        "NetBSD"
#elif defined __OpenBSD__
        "OpenBSD"
#elif defined _WIN32
        "Windows"
#else
# if defined(__GNUC__)
#  warning unknown OS
# endif
        0
#endif
        ;
    const char*machine =
#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
        "amd64"
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__) || defined(_M_IX86)
        "i386"
#elif defined(__ppc__)
        "ppc"
#elif defined(__aarch64__)
        "arm64"
#elif defined (__ARM_ARCH)
        "armv" stringify(__ARM_ARCH)
# if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#  if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        "b"
#  endif
# endif
#else
# if defined(__GNUC__)
#  warning unknown architecture
# endif
        0
#endif
        ;

        /* only send the arch info, if we are sure about it... */
    if (os && machine)
        sys_vgui("::deken::set_platform %s %s %d %d\n",
                 os, machine,
                 8 * sizeof(char*),
                 8 * sizeof(t_float));
}

#define FIRSTPORTNUM 5400

static int sys_do_startgui(const char *libdir)
{
    char cmdbuf[4*MAXPDSTRING], *guicmd, apibuf[256], apibuf2[256];
    struct sockaddr_in server = {0};
    int msgsock;
    char buf[15];
    int len = sizeof(server);
    const int maxtry = 20;
    int ntry = 0, portno = FIRSTPORTNUM;
    int xsock = -1, dumbo = -1;
#ifndef _WIN32
    int stdinpipe[2];
    pid_t childpid;
#endif /* _WIN32 */

    sys_init_fdpoll();

    if (sys_guisetportnumber)  /* GUI exists and sent us a port number */
    {
        struct sockaddr_in server = {0};
        struct hostent *hp;
#ifdef __APPLE__
            /* guisock might be 1 or 2, which will have offensive results
            if somebody writes to stdout or stderr - so we just open a few
            files to try to fill fds 0 through 2.  (I tried using dup()
            instead, which would seem the logical way to do this, but couldn't
            get it to work.) */
        int burnfd1 = open("/dev/null", 0), burnfd2 = open("/dev/null", 0),
            burnfd3 = open("/dev/null", 0);
        if (burnfd1 > 2)
            close(burnfd1);
        if (burnfd2 > 2)
            close(burnfd2);
        if (burnfd3 > 2)
            close(burnfd3);
#endif
        /* create a socket */
        pd_this->pd_inter->i_guisock = socket(AF_INET, SOCK_STREAM, 0);
        if (pd_this->pd_inter->i_guisock < 0)
            sys_sockerror("socket");

        /* connect socket using hostname provided in command line */
        server.sin_family = AF_INET;

        hp = gethostbyname(LOCALHOST);

        if (hp == 0)
        {
            fprintf(stderr,
                "localhost not found (inet protocol not installed?)\n");
            return (1);
        }
        memcpy((char *)&server.sin_addr, (char *)hp->h_addr, hp->h_length);

        /* assign client port number */
        server.sin_port = htons((unsigned short)sys_guisetportnumber);

            /* try to connect */
        if (connect(pd_this->pd_inter->i_guisock,
            (struct sockaddr *) &server, sizeof (server)) < 0)
        {
            sys_sockerror("connecting stream socket");
            return (1);
        }
    }
    else    /* default behavior: start up the GUI ourselves. */
    {
#ifdef _WIN32
        char scriptbuf[MAXPDSTRING+30], wishbuf[MAXPDSTRING+30], portbuf[80];
        int spawnret;
        char intarg;
#else
        int intarg;
#endif

        /* create a socket */
        xsock = socket(AF_INET, SOCK_STREAM, 0);
        if (xsock < 0)
        {
            sys_sockerror("socket");
            return (1);
        }
        intarg = 1;
        if (setsockopt(xsock, IPPROTO_TCP, TCP_NODELAY,
            &intarg, sizeof(intarg)) < 0)
#ifndef _WIN32
                post("setsockopt (TCP_NODELAY) failed\n")
#endif
                    ;


        server.sin_family = AF_INET;
        server.sin_addr.s_addr = INADDR_ANY;

        /* assign server port number */
        server.sin_port =  htons((unsigned short)portno);
        /* name the socket */
        while (bind(xsock, (struct sockaddr *)&server, sizeof(server)) < 0)
        {
#ifdef _WIN32
            int err = WSAGetLastError();
#else
            int err = errno;
#endif
            if ((ntry++ > maxtry) || (err != EADDRINUSE))
            {
                perror("bind");
                fprintf(stderr,
                    "Pd was unable to find a port number to bind to\n");
                sys_closesocket(xsock);
                return (1);
            } else if (ntry > maxtry) {
                    /* last try: let the system pick a random port for us */
                portno = 0;
            } else
                portno++;
            server.sin_port = htons((unsigned short)(portno));
        }
        if (!portno) {
                /* if the system chose a port for us, we need to know which */
            socklen_t serversize=sizeof(server);
            if(!getsockname(xsock, (struct sockaddr *)&server, &serversize))
                portno = ntohs(server.sin_port);
        }
        if (sys_verbose) fprintf(stderr, "port %d\n", portno);


#ifndef _WIN32
        if (sys_guicmd)
            guicmd = sys_guicmd;
        else
        {
#ifdef __APPLE__
            int i;
            struct stat statbuf;
            glob_t glob_buffer;
            char *homedir = getenv("HOME");
            char embed_glob[FILENAME_MAX];
            char home_filename[FILENAME_MAX];
            char *wish_paths[11] = {
 "(custom wish not defined)",
 "(did not find a home directory)",
 "/Applications/Utilities/Wish.app/Contents/MacOS/Wish",
 "/Applications/Utilities/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/Applications/Wish.app/Contents/MacOS/Wish",
 "/Applications/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/Library/Frameworks/Tk.framework/Resources/Wish.app/Contents/MacOS/Wish",
 "/Library/Frameworks/Tk.framework/Resources/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/System/Library/Frameworks/Tk.framework/Resources/Wish.app/Contents/MacOS/Wish",
 "/System/Library/Frameworks/Tk.framework/Resources/Wish Shell.app/Contents/MacOS/Wish Shell",
 "/usr/bin/wish"
            };
            /* this glob is needed so the Wish executable can have the same
             * filename as the Pd.app, i.e. 'Pd-0.42-3.app' should have a Wish
             * executable called 'Pd-0.42-3.app/Contents/MacOS/Pd-0.42-3' */
            sprintf(embed_glob, "%s/../MacOS/Pd*", libdir);
            glob_buffer.gl_matchc = 1; /* we only need one match */
            glob(embed_glob, GLOB_LIMIT, NULL, &glob_buffer);
            /* If we are using a copy of Wish embedded in the Pd.app, then it
             * will automatically load pd-gui.tcl if that embedded Wish can
             * find ../Resources/Scripts/AppMain.tcl, then Wish doesn't want
             * to receive the pd-gui.tcl as an argument.  Otherwise it needs
             * to know how to find pd-gui.tcl */
            if (glob_buffer.gl_pathc > 0)
                sprintf(cmdbuf, "\"%s\" %d\n", glob_buffer.gl_pathv[0], portno);
            else
            {
                int wish_paths_count = sizeof(wish_paths)/sizeof(*wish_paths);
                #ifdef WISH
                    wish_paths[0] = WISH;
                #endif
                sprintf(home_filename,
                        "%s/Applications/Wish.app/Contents/MacOS/Wish",homedir);
                wish_paths[1] = home_filename;
                for(i=0; i<wish_paths_count; i++)
                {
                    if (sys_verbose)
                        fprintf(stderr, "Trying Wish at \"%s\"\n",
                            wish_paths[i]);
                    if (stat(wish_paths[i], &statbuf) >= 0)
                        break;
                }
                if(i>=wish_paths_count)
                {
                    fprintf(stderr, "sys_startgui couldn't find tcl/tk\n");
                    sys_closesocket(xsock);
                    return (1);
                }
                sprintf(cmdbuf, "\"%s\" \"%s/%spd-gui.tcl\" %d\n",
                        wish_paths[i], libdir, PDGUIDIR, portno);
            }
#else /* __APPLE__ */
            /* sprintf the wish command with needed environment variables.
            For some reason the wish script fails if HOME isn't defined so
            if necessary we put that in here too. */
            sprintf(cmdbuf,
  "TCL_LIBRARY=\"%s/lib/tcl/library\" TK_LIBRARY=\"%s/lib/tk/library\"%s \
  " WISH " \"%s/" PDGUIDIR "/pd-gui.tcl\" %d\n",
                 libdir, libdir, (getenv("HOME") ? "" : " HOME=/tmp"),
                    libdir, portno);
#endif /* __APPLE__ */
            guicmd = cmdbuf;
        }
        if (sys_verbose)
            fprintf(stderr, "%s", guicmd);

        childpid = fork();
        if (childpid < 0)
        {
            if (errno) perror("sys_startgui");
            else fprintf(stderr, "sys_startgui failed\n");
            sys_closesocket(xsock);
            return (1);
        }
        else if (!childpid)                     /* we're the child */
        {
            sys_closesocket(xsock);     /* child doesn't listen */
#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__GNU__)
            sys_set_priority(MODE_NRT);  /* child runs non-real-time */
#endif
#ifndef __APPLE__
// TODO this seems unneeded on any platform hans@eds.org
                /* the wish process in Unix will make a wish shell and
                    read/write standard in and out unless we close the
                    file descriptors.  Somehow this doesn't make the MAC OSX
                        version of Wish happy...*/
            if (pipe(stdinpipe) < 0)
                sys_sockerror("pipe");
            else
            {
                if (stdinpipe[0] != 0)
                {
                    close (0);
                    dup2(stdinpipe[0], 0);
                    close(stdinpipe[0]);
                }
            }
#endif /* NOT __APPLE__ */
            execl("/bin/sh", "sh", "-c", guicmd, (char*)0);
            perror("pd: exec");
            fprintf(stderr, "Perhaps tcl and tk aren't yet installed?\n");
            _exit(1);
       }
#else /* NOT _WIN32 */
        /* fprintf(stderr, "%s\n", libdir); */

        strcpy(scriptbuf, "\"");
        strcat(scriptbuf, libdir);
        strcat(scriptbuf, "/" PDGUIDIR "pd-gui.tcl\"");
        sys_bashfilename(scriptbuf, scriptbuf);

        sprintf(portbuf, "%d", portno);

        strcpy(wishbuf, libdir);
        strcat(wishbuf, "/" PDBINDIR WISH);
        sys_bashfilename(wishbuf, wishbuf);

        spawnret = _spawnl(P_NOWAIT, wishbuf, WISH, scriptbuf, portbuf, NULL);
        if (spawnret < 0)
        {
            perror("spawnl");
            fprintf(stderr, "%s: couldn't load TCL\n", wishbuf);
            return (1);
        }

#endif /* NOT _WIN32 */
    }


    if (!sys_guisetportnumber)
    {
        if (sys_verbose)
            fprintf(stderr, "Waiting for connection request... \n");
        if (listen(xsock, 5) < 0) sys_sockerror("listen");

        pd_this->pd_inter->i_guisock = accept(xsock,
            (struct sockaddr *) &server, (socklen_t *)&len);

        sys_closesocket(xsock);

        if (pd_this->pd_inter->i_guisock < 0) sys_sockerror("accept");
        if (sys_verbose)
            fprintf(stderr, "... connected\n");
        pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    }

    pd_this->pd_inter->i_socketreceiver = socketreceiver_new(0, 0, 0, 0);
    sys_addpollfn(pd_this->pd_inter->i_guisock,
        (t_fdpollfn)socketreceiver_read,
            pd_this->pd_inter->i_socketreceiver);

            /* here is where we start the pinging. */
#if defined(__linux__) || defined(__FreeBSD_kernel__)
    if (sys_hipriority)
        sys_gui("pdtk_watchdog\n");
#endif
    sys_get_audio_apis(apibuf);
    sys_get_midi_apis(apibuf2);
    sys_set_searchpath();     /* tell GUI about path and startup flags */
    sys_set_temppath();
    sys_set_extrapath();
    sys_set_startup();
                       /* ... and about font, medio APIS, etc */
    sys_vgui("pdtk_pd_startup %d %d %d {%s} %s %s {%s} %s\n",
             PD_MAJOR_VERSION, PD_MINOR_VERSION,
             PD_BUGFIX_VERSION, PD_TEST_VERSION,
             apibuf, apibuf2, sys_font, sys_fontweight);
    sys_vgui("set pd_whichapi %d\n", sys_audioapi);
    sys_vgui("set zoom_open %d\n", sys_zoom_open == 2);

    sys_init_deken();
    return (0);
}

void sys_setrealtime(const char *libdir)
{
    char cmdbuf[MAXPDSTRING];
#if defined(__linux__) || defined(__FreeBSD_kernel__)
        /*  promote this process's priority, if we can and want to.
        If sys_hipriority not specified (-1), we assume real-time was wanted.
        Starting in Linux 2.6 one can permit real-time operation of Pd by]
        putting lines like:
                @audio - rtprio 99
                @audio - memlock unlimited
        in the system limits file, perhaps /etc/limits.conf or
        /etc/security/limits.conf, and calling Pd from a user in group audio. */
    if (sys_hipriority == -1)
        sys_hipriority = 1;

    snprintf(cmdbuf, MAXPDSTRING, "%s/bin/pd-watchdog", libdir);
    cmdbuf[MAXPDSTRING-1] = 0;
    if (sys_hipriority)
    {
        struct stat statbuf;
        if (stat(cmdbuf, &statbuf) < 0)
        {
            fprintf(stderr,
              "disabling real-time priority due to missing pd-watchdog (%s)\n",
                cmdbuf);
            sys_hipriority = 0;
        }
    }
    if (sys_hipriority)
    {
        int pipe9[2], watchpid;
            /* To prevent lockup, we fork off a watchdog process with
            higher real-time priority than ours.  The GUI has to send
            a stream of ping messages to the watchdog THROUGH the Pd
            process which has to pick them up from the GUI and forward
            them.  If any of these things aren't happening the watchdog
            starts sending "stop" and "cont" signals to the Pd process
            to make it timeshare with the rest of the system.  (Version
            0.33P2 : if there's no GUI, the watchdog pinging is done
            from the scheduler idle routine in this process instead.) */

        if (pipe(pipe9) < 0)
        {
            sys_sockerror("pipe");
            return;
        }
        watchpid = fork();
        if (watchpid < 0)
        {
            if (errno)
                perror("sys_setpriority");
            else fprintf(stderr, "sys_setpriority failed\n");
            return;
        }
        else if (!watchpid)             /* we're the child */
        {
            sys_set_priority(MODE_WATCHDOG);
            if (pipe9[1] != 0)
            {
                dup2(pipe9[0], 0);
                close(pipe9[0]);
            }
            close(pipe9[1]);

            if (sys_verbose) fprintf(stderr, "%s\n", cmdbuf);
            execl("/bin/sh", "sh", "-c", cmdbuf, (char*)0);
            perror("pd: exec");
            _exit(1);
        }
        else                            /* we're the parent */
        {
            sys_set_priority(MODE_RT);
            close(pipe9[0]);
                /* set close-on-exec so that watchdog will see an EOF when we
                close our copy - otherwise it might hang waiting for some
                stupid child process (as seems to happen if jackd auto-starts
                for us.) */
            if(fcntl(pipe9[1], F_SETFD, FD_CLOEXEC) < 0)
              perror("close-on-exec");
            sys_watchfd = pipe9[1];
                /* We also have to start the ping loop in the GUI;
                this is done later when the socket is open. */
        }
    }
    else if (sys_verbose)
        post("not setting real-time priority");
#endif /* __linux__ */

#ifdef _WIN32
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        fprintf(stderr, "pd: couldn't set high priority class\n");
#endif
#ifdef __APPLE__
    if (sys_hipriority)
    {
        struct sched_param param;
        int policy = SCHED_RR;
        int err;
        param.sched_priority = 80; /* adjust 0 : 100 */

        err = pthread_setschedparam(pthread_self(), policy, &param);
        if (err)
            post("warning: high priority scheduling failed\n");
    }
#endif /* __APPLE__ */
}

extern void sys_exit(void);

/* This is called when something bad has happened, like a segfault.
Call glob_quit() below to exit cleanly.
LATER try to save dirty documents even in the bad case. */
void sys_bail(int n)
{
    static int reentered = 0;
    if (!reentered)
    {
        reentered = 1;
#if !defined(__linux__) && !defined(__FreeBSD_kernel__) && !defined(__GNU__)
            /* sys_close_audio() hangs if you're in a signal? */
        fprintf(stderr ,"gui socket %d - \n", pd_this->pd_inter->i_guisock);
        fprintf(stderr, "closing audio...\n");
        sys_close_audio();
        fprintf(stderr, "closing MIDI...\n");
        sys_close_midi();
        fprintf(stderr, "... done.\n");
#endif
        exit(n);
    }
    else _exit(1);
}

void glob_quit(void *dummy)
{
    sys_close_audio();
    sys_close_midi();
    if (sys_havegui())
    {
        sys_closesocket(pd_this->pd_inter->i_guisock);
        sys_rmpollfn(pd_this->pd_inter->i_guisock);
    }
    exit(0);
}

    /* recursively descend to all canvases and send them "vis" messages
    if they believe they're visible, to make it really so. */
static void glist_maybevis(t_glist *gl)
{
    t_gobj *g;
    for (g = gl->gl_list; g; g = g->g_next)
        if (pd_class(&g->g_pd) == canvas_class)
            glist_maybevis((t_glist *)g);
    if (gl->gl_havewindow)
    {
        canvas_vis(gl, 0);
        canvas_vis(gl, 1);
    }
}

int sys_startgui(const char *libdir)
{
    t_canvas *x;
    for (x = pd_getcanvaslist(); x; x = x->gl_next)
        canvas_vis(x, 0);
    pd_this->pd_inter->i_havegui = 1;
    pd_this->pd_inter->i_guihead = pd_this->pd_inter->i_guitail = 0;
    if (sys_do_startgui(libdir))
        return (-1);
    for (x = pd_getcanvaslist(); x; x = x->gl_next)
        if (strcmp(x->gl_name->s_name, "_float_template") &&
            strcmp(x->gl_name->s_name, "_float_array_template") &&
                strcmp(x->gl_name->s_name, "_text_template"))
    {
        glist_maybevis(x);
        canvas_vis(x, 1);
    }
    return (0);
}

 /* more work needed here - for some reason we can't restart the gui after
 shutting it down this way.  I think the second 'init' message never makes
 it because the to-gui buffer isn't re-initialized. */
void sys_stopgui( void)
{
    t_canvas *x;
    for (x = pd_getcanvaslist(); x; x = x->gl_next)
        canvas_vis(x, 0);
    sys_vgui("%s", "exit\n");
    if (pd_this->pd_inter->i_guisock >= 0)
    {
        sys_closesocket(pd_this->pd_inter->i_guisock);
        sys_rmpollfn(pd_this->pd_inter->i_guisock);
        pd_this->pd_inter->i_guisock = -1;
    }
    pd_this->pd_inter->i_havegui = 0;
}

/* ----------- mutexes for thread safety --------------- */

void s_inter_newpdinstance( void)
{
    pd_this->pd_inter = getbytes(sizeof(*pd_this->pd_inter));
#if PDTHREADS
    pthread_mutex_init(&pd_this->pd_inter->i_mutex, NULL);
    pd_this->pd_islocked = 0;
#endif
#ifdef _WIN32
    pd_this->pd_inter->i_freq = 0;
#endif
    pd_this->pd_inter->i_havegui = 0;
}

void s_inter_free(t_instanceinter *inter)
{
    if (inter->i_fdpoll) {
        binbuf_free(inter->i_inbinbuf);
        inter->i_inbinbuf = 0;
        t_freebytes(inter->i_fdpoll, inter->i_nfdpoll * sizeof(t_fdpoll));
        inter->i_fdpoll = 0;
        inter->i_nfdpoll = 0;
    }
    freebytes(inter, sizeof(*inter));
}

void s_inter_freepdinstance( void)
{
    s_inter_free(pd_this->pd_inter);
}

#if PDTHREADS
#ifdef PDINSTANCE
static pthread_rwlock_t sys_rwlock = PTHREAD_RWLOCK_INITIALIZER;
#else /* PDINSTANCE */
static pthread_mutex_t sys_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif /* PDINSTANCE */
#endif /* PDTHREADS */

#if PDTHREADS

/* routines to lock and unlock Pd's global class structure or list of Pd
instances.  These are called internally within Pd when creating classes, adding
methods to them, or creating or freeing Pd instances.  They should probably
not be called from outside Pd.  They should be called at a point where the
current instance of Pd is currently locked via sys_lock() below; this gains
read access to the class and instance lists which must be released for the
write-lock to be available. */

void pd_globallock( void)
{
#ifdef PDINSTANCE
    if (!pd_this->pd_islocked)
        bug("pd_globallock");
    pthread_rwlock_unlock(&sys_rwlock);
    pthread_rwlock_wrlock(&sys_rwlock);
#endif /* PDINSTANCE */
}

void pd_globalunlock( void)
{
#ifdef PDINSTANCE
    pthread_rwlock_unlock(&sys_rwlock);
    pthread_rwlock_rdlock(&sys_rwlock);
#endif /* PDINSTANCE */
}

/* routines to lock/unlock a Pd instance for thread safety.  Call pd_setinsance
first.  The "pd_this"  variable can be written and read thread-safely as it
is defined as per-thread storage. */
void sys_lock( void)
{
#ifdef PDINSTANCE
    pthread_mutex_lock(&pd_this->pd_inter->i_mutex);
    pthread_rwlock_rdlock(&sys_rwlock);
    pd_this->pd_islocked = 1;
#else
    pthread_mutex_lock(&sys_mutex);
#endif
}

void sys_unlock( void)
{
#ifdef PDINSTANCE
    pd_this->pd_islocked = 0;
    pthread_rwlock_unlock(&sys_rwlock);
    pthread_mutex_unlock(&pd_this->pd_inter->i_mutex);
#else
    pthread_mutex_unlock(&sys_mutex);
#endif
}

int sys_trylock( void)
{
#ifdef PDINSTANCE
    int ret;
    if (!(ret = pthread_mutex_trylock(&pd_this->pd_inter->i_mutex)))
    {
        if (!(ret = pthread_rwlock_tryrdlock(&sys_rwlock)))
            return (0);
        else
        {
            pthread_mutex_unlock(&pd_this->pd_inter->i_mutex);
            return (ret);
        }
    }
    else return (ret);
#else
    return pthread_mutex_trylock(&sys_mutex);
#endif
}

#else /* PDTHREADS */

void sys_lock( void) {}
void sys_unlock( void) {}
void pd_globallock( void) {}
void pd_globalunlock( void) {}

#endif /* PDTHREADS */