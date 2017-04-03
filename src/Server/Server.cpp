/*
   Copyright 2016 Nidium Inc. All rights reserved.
   Use of this source code is governed by a MIT license
   that can be found in the LICENSE file.
*/
#define _HAVE_SSL_SUPPORT 1

#include "Server/Server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <list>

#ifdef _MSC_VER
typedef int pid_t;
#include "Port/MSWindows.h"
#ifndef NIDIUM_NO_FORK
#define NIDIUM_NO_FORK
#endif
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#include <prinit.h>
#include <prproces.h>

#include <ape_netlib.h>

#include "Binding/JSProcess.h"
#include "Server/Context.h"
#include "Server/REPL.h"

using Nidium::Binding::NidiumJS;
using Nidium::Binding::JSProcess;

extern "C" {
unsigned long _ape_seed;
}

namespace Nidium {
namespace Server {

#define NIDIUM_SERVER_VERSION "0.2-dev"
#define NIDIUM_MAX_WORKERS 64

// {{{ Static
static std::list<pid_t> pidList;

static void signal_handler(int sign)
{
    APE_loop_stop();
    for (auto it : pidList) {
        kill(it, sign);
    }
}

#ifndef _MSC_VER
static int inc_rlimit(int nofile)
{
    struct rlimit rl;

    rl.rlim_cur = nofile;
    rl.rlim_max = nofile;

    return setrlimit(RLIMIT_NOFILE, &rl);
}
#endif

static int NidiumCheckParentAlive_ping(void *arg)
{
    pid_t ppid = getppid();
    /*
        If the parent's pid is 0 or 1, it means that our parent is dead. Exit.
    */
    if (ppid == 0 || ppid == 1) {
        PR_Cleanup();
        PR_ProcessExit(0);
    }

    return 1000;
}
// }}}


// {{{ Server
#ifdef _MSC_VER
void Server::daemonize(int pidfile)
{
    printf("Daemon not implemented on windows.\n");
    //third-party/mozilla-central/nsprpub/pr/tests/parent.c
}

void Server::wait()
{
    int pid;
    int state;

    if (m_HasREPL) {
        this->displayVersion();
    }
    printf("TODO break this endless loop.\n");
    while ((pid == 1 /*todo waitpid(-1, &state, 0)*/ )) {

        if (errno == ECHILD) {
            break;
        } else {

            if (WIFEXITED(state)) {
                PR_Cleanup();
                PR_ProcessExit(WEXITSTATUS(state));
            }

            if (WIFSIGNALED(state)) {
                int idx_crash = m_PidIdxMapper[pid];
                fprintf(stderr, "[Crash] Worker %d has crashed :'( (%d)\n",
                        idx_crash, WTERMSIG(state));
                if (this->initWorker(&idx_crash) == 0) {
                    return;
                }
            }
        }
    }
}
#else
void Server::daemonize(int pidfile)
{
    if (0 != fork()) {
        PR_Cleanup();
        PR_ProcessExit(0);
    }
    if (-1 == setsid()) {
        PR_Cleanup();
        PR_ProcessExit(0);
    }
    signal(SIGHUP, SIG_IGN);
    if (0 != fork()) {
        PR_Cleanup();
        PR_ProcessExit(0);
    }
    if (pidfile > 0) {
        char pidstring[32];
        int len;
        len = sprintf(pidstring, "%i", (int)getpid());
        write(pidfile, pidstring, len);
        close(pidfile);
    }
}

void Server::wait()
{
    int pid;
    int state;

    if (m_HasREPL) {
        this->displayVersion();
    }
    while ((pid = waitpid(-1, &state, 0))) {
        if (errno == ECHILD) {
            break;
        } else {

            if (WIFEXITED(state)) {
                PR_Cleanup();
                PR_ProcessExit(WEXITSTATUS(state));
            }

            if (WIFSIGNALED(state)) {
                int idx_crash = m_PidIdxMapper[pid];
                fprintf(stderr, "[Crash] Worker %d has crashed :'( (%s)\n",
                        idx_crash, strsignal(WTERMSIG(state)));
                if (this->initWorker(&idx_crash) == 0) {
                    return;
                }
            }
        }
    }
}
#endif

void Server::displayVersion()
{
#include "ASCII.h"
    fprintf(stdout, nidium_ascii, NIDIUM_SERVER_VERSION, __DATE__, __TIME__,
            getpid(), m_NWorkers);
}

int Server::initWorker(int *idx)
{
    if (!(*idx)) {
        *idx = ++m_WorkerIdx;
    }

    pid_t pid = 0;
/* Execute the worker for the child process and returns 0 */
#ifndef NIDIUM_NO_FORK
    if ((pid = fork()) == 0) {
#endif
        Worker worker(*idx, (m_HasREPL && *idx == 1));
#ifdef USE_SETPROCTITLE
        setproctitle("Nidium-Server:<%s> (worker %d)",
                     m_InstanceName ? m_InstanceName : "noname", *idx);
#endif
        worker.run(m_Args.argc, m_Args.argv, m_JSStrictMode);

        return 0;
#ifndef NIDIUM_NO_FORK
    }

    pidList.push_back(pid);
#endif

    m_PidIdxMapper[pid] = *idx;

    /* Parent process returns the pid */
    return pid;
}

int Server::Start(int argc, char *argv[])
{
    PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif

    Server *server = new Server(argc, argv);

    return server->init();
}

void Server::Usage(option_t *long_options, const char **text_blocks)
{
    option_t *opt;
    size_t i;

    opt = long_options;
    i = 0;
    Server::displayVersion();
    fprintf(stdout, "Usage: %s [options] [[script.js] [arguments]]\n\nOptions: \n",
            "nidium-server");

    while (opt->name != NULL) {
        char const * text = text_blocks[i];
        fprintf(stdout, "  -%c, --%-12s %s\n", opt->val, opt->name, text);
        opt++;
        i++;
    }
}

int Server::init()
{
    bool daemon = false;
    int workers = 1;

    static char const *text_blocks[]
        = { "Enable Strict mode", 
#ifndef _MSC_VER
            "Run the interactive console (REPL)",
            "Set process name", 
            "Run as daemon", 
#endif
            "Start multiple workers",
             "This text" };

    static option_t long_options[]
        = { { "strict", no_argument, 0, 's' },
#ifndef _MSC_VER
            { "interactive", no_argument, 0, 'i' },
            { "name", required_argument, 0, 'n' },
            { "daemon", no_argument, 0, 'd' },
#endif
            { "workers", required_argument, 0, 'w' },
            { "help", no_argument, 0, 'h' },
            { 0, 0, 0, 0 } };

    _ape_seed = time(NULL) ^ (getpid() << 16);

    signal(SIGINT, &signal_handler);
    signal(SIGTERM, &signal_handler);
#ifndef _MSC_VER
    signal(SIGQUIT, &signal_handler);
    // signal(SIGCHLD, SIG_IGN);

    /*
        Needed on macosx so that arguments doesn't fail after the .js file
    */
    setenv("POSIXLY_CORRECT", "1", 1);
#endif
    int ch;
    while ((ch = getopt_long(m_Args.argc, m_Args.argv, "dsiw:n:h?", long_options,
                             NULL)) != -1) {
        switch (ch) {
#ifndef _MSC_VER
            case 'd':
                daemon = true;
                break;
            case 'i':
                m_HasREPL = true;
                break;
            case 'n':
                m_InstanceName = strdup(optarg);
                break;
#endif
            case 's':
                m_JSStrictMode = true;
                break;
            case ':':
            case 'h':
            case '?':
                Server::Usage(&long_options[0], text_blocks);
                PR_Cleanup();
                PR_ProcessExit(1);
                break;
            case 'w':
                workers = atoi(optarg);
                break;
            default:
                break;
        }
    }

    /*
        Only 'forward' the 'rest' of the arguments to the js Process/worker
    */
    m_Args.argc -= optind;
    m_Args.argv += optind;

    if (workers > NIDIUM_MAX_WORKERS) {
        fprintf(stderr, "[Error] Too many worker requested : max %d\n",
                NIDIUM_MAX_WORKERS);
        PR_Cleanup();
        PR_ProcessExit(1);
    }

    /*
        Don't demonize if no JS file was provided
    */
    if (daemon && m_Args.argc > 0) {
        m_HasREPL = false;
        this->daemonize();
    } else if (daemon) {
        fprintf(stderr, "Can't daemonize if no JS file is provided\n");
        Server::Usage(&long_options[0], text_blocks);
        PR_Cleanup();
        PR_ProcessExit(1);
    } else if (m_Args.argc == 0) {
        m_HasREPL = true;
    }

    if (workers) {
        m_NWorkers = workers;
        for (int i = 0; i < workers; i++) {
            int idx = 0;
            if (this->initWorker(&idx) == 0) {
                return 1;
            }
        }
    }

    /*
        Only executed by the parent process
    */
    this->wait();

    return 1;
}

Server::Server(int argc, char **argv)
    : m_WorkerIdx(0), m_InstanceName(NULL), m_HasREPL(false),
      m_JSStrictMode(false), m_NWorkers(0)
{
    m_Args.argc = argc;
    m_Args.argv = argv;
}
// }}}

// {{{ Worker
Worker::Worker(int idx, bool repl) : m_Idx(idx), m_RunREPL(repl)
{
}

Worker::~Worker()
{
}

int Worker::run(int argc, char **argv, bool jsstrict)
{
    REPL *repl      = NULL;
    ape_global *net = APE_init();

#ifndef _MSC_VER
    inc_rlimit(64000);
    //todo windows investigate WSAECONNRESET instead of EPIPE errorcode
    signal(SIGPIPE, SIG_IGN);
#endif

    Nidium::Server::Context ctx(net, this, jsstrict, m_RunREPL);

    const NidiumJS *js = ctx.getNJS();
    JSProcess::RegisterObject(js->getJSContext(), argv, argc,
                              this->getIdentifier());

/*
        Daemon requires a .js to load
    */

#ifdef DEBUG
    ndm_log(NDM_LOG_WARN, "Server", "Running in Debug mode");
#endif
    if (jsstrict) {
        ndm_log(NDM_LOG_INFO, "Server", "JS strict mode is enabled");
    }
    if (argc >= 1) {
        if (!js) {
            ndm_log(NDM_LOG_ERROR, "Server", "Failed to get JS");
            return 0;
        }
        ctx.getNJS()->LoadScript(argv[0]);
    }

    /* Heap allocated because we need to be
    sure that it's deleted before NidiumJS */
    if (m_RunREPL) {
        repl = new REPL(ctx.getNJS());
    }

    APE_timer_create(net, 1, NidiumCheckParentAlive_ping, NULL);
    APE_loop_run(net);

    if (repl) {
        delete repl;
    }
#if 0
    TODO : heap use after free ?
    APE_destroy(net);
#endif
    return 0;
}
// }}}

} // namespace Server
} // namespace Nidium
