/*
 * Copyright 2003-2011 Jeffrey K. Hollingsworth
 *
 * This file is part of Active Harmony.
 *
 * Active Harmony is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Active Harmony is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Active Harmony.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "hclient.h"
#include "hmesg.h"
#include "hsession.h"
#include "hval.h"

typedef enum method_t {
    METHOD_WALL,
    METHOD_USER,
    METHOD_SYS,
    METHOD_OUTPUT,

    METHOD_MAX
} method_t;

struct strlist {
    char *str;
    struct strlist *next;
};
typedef struct strlist strlist_t;

typedef struct bundle_info {
    hval_type type;
    char *name;
    void *data;
    int used;
} bundle_info_t;

void usage(const char *);
void parseArgs(int, char **);
int handle_int(char *);
int handle_real(char *);
int handle_enum(char *);
int handle_method(char *);
int handle_chapel(char *);
int prepare_client_argv();
FILE *tuna_popen(const char *, char **, pid_t *);
double tv_to_double(struct timeval *);
int argv_add(char *);
bundle_info_t *tuna_bundle_add(hval_type, char *);
bundle_info_t *tuna_bundle_get(char **);
int is_exec(const char *filename);
char *find_exec(const char *);
pid_t launch_silent(const char *, char **);

char prog_env[FILENAME_MAX];
char prog_hsvr[FILENAME_MAX];

method_t method = METHOD_WALL;
hdesc_t *hdesc = NULL;
hsession_t sess;
unsigned int max_loop = 50;
unsigned int quiet = 0;
unsigned int verbose = 0;

#define MAX_BUNDLE 64
unsigned int bcount = 0;
bundle_info_t binfo[MAX_BUNDLE];

strlist_t *argv_template = NULL;
int client_argc = 0;
char **client_argv;
char *argv_buf = NULL;
int argv_buflen = 0;

int main(int argc, char **argv)
{
    int i, hresult, line_start, count;
    char readbuf[4096], *path, *hsvr_argv[2];
    const char *retval;
    FILE *fptr;
    double perf = 0.0;

    struct timeval wall_start, wall_end, wall_time;

    pid_t pid, svr_pid = 0;
    int client_status;
    struct rusage client_usage;

    if (argc < 2) {
        usage(argv[0]);
        return -1;
    }

    /* Find external support executables */
    path = find_exec("env");
    if (path != NULL) {
        strncpy(prog_env, path, sizeof(prog_env));
        if (argv_add(prog_env) < 0)
            return -1;
    } else if (!quiet) {
        fprintf(stderr, "*** Could not find env executable in $PATH."
                "  Will attempt direct execution.\n");
    }

    snprintf(prog_hsvr, sizeof(prog_hsvr), "%s/hserver", dirname(argv[0]));
    if (!is_exec(prog_hsvr)) {
        path = find_exec("hserver");
        if (path != NULL) {
            strncpy(prog_hsvr, path, sizeof(prog_hsvr));
            if (argv_add(prog_env) < 0)
                return -1;
        } else {
            fprintf(stderr, "*** Could not find hserver executable in $PATH."
                    "  Will attempt to connect to remote server.\n");
            prog_hsvr[0] = '\0';
        }
    }

    /* Initialize the Harmony descriptor */
    hdesc = harmony_init();
    if (hdesc == NULL) {
        fprintf(stderr, "Failed to initialize a Harmony descriptor.\n");
        return -1;
    }

    /* Parse the command line arguments. */
    hsession_init(&sess);
    hsession_name(&sess, "tuna");
    parseArgs(argc, argv);

    /* Sanity check before we attempt to connect to the server. */
    if (bcount < 1) {
        fprintf(stderr, "No tunable variables defined.\n");
        return -1;
    }

    /* Launch Harmony server if needed */
    if (prog_hsvr[0] != '\0') {
        hsvr_argv[0] = prog_hsvr;
        hsvr_argv[1] = NULL;
        svr_pid = launch_silent(hsvr_argv[0], hsvr_argv);
        if (svr_pid < 0)
            return -1;

        /* XXX - This should be replaced with a system where the hserver
           notifies tuna of its readiness.  Perhaps if a --slave mode was
           added to the server. */
        sleep(1);
    }

    retval = hsession_launch(&sess, NULL, 0);
    if (retval) {
        fprintf(stderr, "Error launching new tuning session: %s\n", retval);
        goto cleanup;
    }

    /* Connect to Harmony server and register ourselves as a client. */
    printf("Starting Harmony...\n");
    for (i = 0; i < 3; ++i) {
        if (harmony_join(hdesc, NULL, 0, "tuna") >= 0)
            break;
        if (verbose)
            fprintf(stderr, "Error connecting to harmony server."
                    "  Re-try in %d seconds...", i+1);
        sleep(i+1);
    }
    if (i == 3) {
        fprintf(stderr, "Could not connect to harmony server.\n");
        return -1;
    }

    for (i = 0; max_loop <= 0 || i < max_loop; ++i) {
        hresult = harmony_fetch(hdesc);
        if (hresult < 0) {
            fprintf(stderr, "Failed to fetch values from server.\n");
            goto cleanup;
        }
        else if (hresult > 0) {
            /* The Harmony system modified the variable values. */
            prepare_client_argv();
        }

        if (gettimeofday(&wall_start, NULL) != 0) {
            perror("Error on gettimeofday()");
            goto cleanup;
        }

        fptr = tuna_popen(client_argv[0], client_argv, &pid);
        if (!fptr)
            goto cleanup;

        count = 0;
        line_start = 1;
        while (fgets(readbuf, sizeof(readbuf), fptr)) {
            if (line_start) sscanf(readbuf, "%lf", &perf);
            if (!quiet) printf("%s", readbuf);
            line_start = (strchr(readbuf, '\n') != NULL);
            ++count;
        }
        fclose(fptr);

        do {
            pid = wait3(&client_status, 0, &client_usage);
            if (svr_pid && pid == svr_pid) {
                fprintf(stderr, "Server died prematurely."
                        "  Closing tuna session.\n");
                exit(-1);
            } else if (pid < 0) {
                perror("Error on wait3()");
                goto cleanup;
            }
        } while (pid == 0);

        if (gettimeofday(&wall_end, NULL) != 0) {
            perror("Error on gettimeofday()");
            goto cleanup;
        }
        timersub(&wall_end, &wall_start, &wall_time);

        switch (method) {
        case METHOD_WALL:   perf = tv_to_double(&wall_time); break;
        case METHOD_USER:   perf = tv_to_double(&client_usage.ru_utime); break;
        case METHOD_SYS:    perf = tv_to_double(&client_usage.ru_stime); break;
        case METHOD_OUTPUT: break;
        default:
            fprintf(stderr, "Unknown measurement method.\n");
            goto cleanup;
        }

        /* Update the performance result */
        if (harmony_report(hdesc, perf) < 0) {
            fprintf(stderr, "Failed to report performance to server.\n");
            goto cleanup;
        }

        if (harmony_converged(hdesc))
            break;
    }

    /* Close the session */
    if (harmony_leave(hdesc) < 0)
        fprintf(stderr, "Failed to disconnect from harmony server.\n");

  cleanup:
    if (svr_pid && kill(svr_pid, SIGKILL) < 0)
        fprintf(stderr, "Could not kill server process (%d).\n", svr_pid);

    return 0;
}

void usage(const char *me)
{
    fprintf(stderr, "Usage: %s tunable_vars [options] prog [prog_args]\n", me);
    fprintf(stderr, "\n"
"  Tunes an application by modifying its input parameters.  The tunable\n"
"  variables are specified using parameters described in the \"Tunable\n"
"  Variable Description\" section below.  After all options, the program\n"
"  binary to launch should be provided.  Optionally, additional arguments\n"
"  may be provided to control how the variables should be supplied to the\n"
"  client application.  The format of this string is described in the\n"
"  \"Optional Argument String\" section below.\n"
"\n"
"Tunable Variable Description\n"
"  -i=name,min,max,step    Describe an integer number variable called\n"
"                            <name> where valid values fall between <min>\n"
"                            and <max> with strides of size <step>.\n"
"  -r=name,min,max,step    Describe a real number variable called <name>\n"
"                            where valid values fall between <min> and\n"
"                            <max> with strides of size <step>.\n"
"  -e=name,val_1,..,val_n  Describe an enumerated variable called <name>\n"
"                            whose values must be <val_1> or <val_2> or ..\n"
"                            or <val_n>.\n"
"\n"
"Options\n"
"  -m=<metric>             Calculate performance of child process using\n"
"                            one of the following metrics:\n"
"                              wall   = Wall time. (default)\n"
"                              user   = Reported user CPU time.\n"
"                              sys    = Reported system CPU time.\n"
"                              output = Read final line of child output.\n"
"  -q                      Suppress client application output.\n"
"  -v                      Print additional informational output.\n"
"  -n=<num>                Run child program at most <num> times.\n"
"\n"
"Controlling Program Arguments\n"
"  If the tunable variables cannot be supplied directly as arguments to\n"
"  the client application, then you must provide additional parameters to\n"
"  describe the format of the argument vector.  Each argument (starting with\n"
"  and including the program binary) may include a percent sign (%%)\n"
"  followed by the name of a previously defined tunable variable.  This\n"
"  identifier may be optionally bracketed by curly-braces.  Values from the\n"
"  tuning server will then be used to complete a command-line instance.\n"
"  A backslash (\\) may be used to produce a literal %%.  For example:\n"
"\n"
"    %s -i=tile,1,10,1 -i=unroll,1,10,1 \\\n"
"        ./matrix_mult -t %%tile -u %%unroll`\n\n", me);
}

void parseArgs(int argc, char **argv)
{
    int i, chapel = 0;
    char *arg;
    bundle_info_t *bun;

    for (i = 1; i < argc && *argv[i] == '-'; ++i) {
        arg = argv[i] + 1;
        while (*arg != '\0') {
            switch (*arg) {
            case 'h': usage(argv[0]); exit(-1);
            case 'i': if (handle_int(arg)  != 0) exit(-1); break;
            case 'r': if (handle_real(arg) != 0) exit(-1); break;
            case 'e': if (handle_enum(arg) != 0) exit(-1); break;
            case 'm': if (handle_method(arg)  != 0) exit(-1); break;
            case 'q': quiet = 1; break;
            case 'v': verbose = 1; break;
            case 'n':
                ++arg;
                if (*arg == '=')
                    ++arg;

                errno = 0;
                max_loop = strtoul(arg, &arg, 0);
                if (errno != 0) {
                    fprintf(stderr, "Invalid -n value.\n");
                    exit(-1);
                }
                if (*arg != '\0') {
                    fprintf(stderr, "Trailing characters after n flag\n");
                    exit(-1);
                }
                break;
            case '-':
                if      (strcmp(arg, "-help") == 0) {usage(argv[0]); exit(-1);}
                else if (strcmp(arg, "-chapel") == 0) {chapel = 1;}
                break;
            default:
                fprintf(stderr, "Unknown flag: -%c\n", *arg);
                exit(-1);
            }
            if (*arg == 'q' || *arg == 'v')
                ++arg;
            else break;
        }
    }

    while (i < argc) {
        if (argv_add(argv[i]) < 0)
            exit(-1);

        for (arg = argv[i]; *arg != '\0'; ++arg) {
            if (*arg == '%') {
                bun = tuna_bundle_get(&arg);
                if (bun == NULL)
                    exit(-1);
                bun->used = 1;
            }
            else if (*arg == '\\') ++arg;
        }

        if (chapel == 1) {
            if (handle_chapel(argv[i]) < 0)
                exit(-1);
            chapel = 0;
        }
        ++i;
    }

    for (i = 0; i < bcount; ++i) {
        if (binfo[i].used == 0) {
            if (verbose)
                fprintf(stdout, "Warning: Appending unused bundle \"%s\""
                        " to target argv.\n", binfo[i].name);

            arg = (char *) malloc(strlen(binfo[i].name) + 2);
            if (arg == NULL) {
                perror("Malloc error");
                exit(-1);
            }
            sprintf(arg, "%%%s", binfo[i].name);
            if (argv_add(arg) < 0)
                exit(-1);
        }
    }

    client_argv = (char **) malloc(sizeof(char *) * (client_argc + 1));
    if (client_argv == NULL) {
        perror("Malloc error");
        exit(-1);
    }
}

int handle_int(char *arg)
{
    char *arg_orig, *name;
    long min, max, step;
    bundle_info_t *bun;

    assert(*arg == 'i');
    arg_orig = arg;

    ++arg;
    if (*arg == '=')
        ++arg;
    name = arg;

    while (*arg != ',' && *arg != '\0') ++arg;
    if (*arg != ',') {
        fprintf(stderr, "Invalid description: \"%s\"\n", arg_orig);
        return -1;
    }
    *(arg++) = '\0';

    if (sscanf(arg, "%ld,%ld,%ld", &min, &max, &step) != 3) {
        fprintf(stderr, "Invalid description for variable \"%s\".\n", name);
        return -1;
    }

    bun = tuna_bundle_add(HVAL_INT, name);
    if (bun == NULL)
        return -1;

    if (hsession_int(&sess, name, min, max, step) < 0) {
        fprintf(stderr, "Error registering variable '%s'.\n", name);
        return -1;
    }

    if (harmony_bind_int(hdesc, name, bun->data) < 0) {
        fprintf(stderr, "Error binding data to variable '%s'.\n", name);
        return -1;
    }

    return 0;
}

int handle_real(char *arg)
{
    char *arg_orig, *name;
    double min, max, step;
    bundle_info_t *bun;

    assert(*arg == 'r');
    arg_orig = arg;

    ++arg;
    if (*arg == '=')
        ++arg;
    name = arg;

    while (*arg != ',' && *arg != '\0') ++arg;
    if (*arg != ',') {
        fprintf(stderr, "Invalid description: \"%s\"\n", arg_orig);
        return -1;
    }
    *(arg++) = '\0';

    if (sscanf(arg, "%lf,%lf,%lf", &min, &max, &step) != 3) {
        fprintf(stderr, "Invalid description for variable \"%s\".\n", name);
        return -1;
    }

    bun = tuna_bundle_add(HVAL_REAL, name);
    if (bun == NULL)
        return -1;

    if (hsession_real(&sess, name, min, max, step) < 0) {
        fprintf(stderr, "Error registering variable '%s'.\n", name);
        return -1;
    }

    if (harmony_bind_real(hdesc, name, bun->data) < 0) {
        fprintf(stderr, "Error binding data to variable '%s'.\n", name);
        return -1;
    }

    return 0;
}

int handle_enum(char *arg)
{
    char *arg_orig, *name, *val;
    bundle_info_t *bun;

    assert(*arg == 'e');
    arg_orig = arg;

    ++arg;
    if (*arg == '=')
        ++arg;
    name = arg;

    while (*arg != ',' && *arg != '\0') ++arg;
    if (*arg != ',') {
        fprintf(stderr, "Invalid description: \"%s\"\n", arg_orig);
        return -1;
    }
    *(arg++) = '\0';

    bun = tuna_bundle_add(HVAL_STR, name);
    if (bun == NULL)
        return -1;

    while (*arg != '\0') {
        val = arg;
        while (*arg != ',' && *arg != '\0') ++arg;
        if (*arg != '\0')
            *(arg++) = '\0';

        if (hsession_enum(&sess, name, val) < 0) {
            fprintf(stderr, "Invalid value string for variable '%s'.\n", name);
            return -1;
        }
    }

    if (harmony_bind_enum(hdesc, name, bun->data) < 0) {
        fprintf(stderr, "Error binding data to variable '%s'.\n", name);
        return -1;
    }

    return 0;
}

int handle_method(char *arg)
{
    assert(*arg == 'm');
    ++arg;
    if (*arg == '=')
        ++arg;

    if (strcmp("wall", arg) == 0)        method = METHOD_WALL;
    else if (strcmp("user", arg) == 0)   method = METHOD_USER;
    else if (strcmp("sys", arg) == 0)    method = METHOD_SYS;
    else if (strcmp("output", arg) == 0) method = METHOD_OUTPUT;
    else {
        fprintf(stderr, "Unknown method choice.\n");
        return -1;
    }

    return 0;
}

int handle_chapel(char *prog)
{
    char buf[4096], *arg;
    int chpl_flag = 0;

    char *name;
    long min, max, step;
    bundle_info_t *bun;

    FILE *fd;
    char *help_argv[3];

    help_argv[0] = prog;
    help_argv[1] = "--help";
    help_argv[2] = NULL;
    fd = tuna_popen(prog, help_argv, NULL);
    if (fd == NULL)
        return -1;

    while (fgets(buf, sizeof(buf), fd) != NULL) {
        if (strcmp(buf, "CONFIG VARS:\n") == 0) {
            chpl_flag = 1;
            break;
        }
    }
    if (!chpl_flag) {
        fprintf(stderr, "%s is not a Chapel program.\n", prog);
        return -1;
    }

    bun = tuna_bundle_add(HVAL_INT, "dataParTsk");
    if (bun == NULL)
        return -1;
    if (hsession_int(&sess, "dataParTsk", 1, 64, 1) < 0) {
        fprintf(stderr, "Error registering variable 'dataParTsk'.\n");
        return -1;
    }
    if (harmony_bind_int(hdesc, "dataParTsk", bun->data) < 0) {
        fprintf(stderr, "Error binding data to variable 'dataParTsk'.\n");
        return -1;
    }
    if (argv_add("--dataParTasksPerLocale=%dataParTsk"))
        return -1;
    bun->used = 1;

    bun = tuna_bundle_add(HVAL_INT, "numThr");
    if (bun == NULL)
        return -1;
    if (hsession_int(&sess, "numThr", 1, 32, 1) < 0) {
        fprintf(stderr, "Error registering variable 'numThr'.\n");
        return -1;
    }
    if (harmony_bind_int(hdesc, "numThr", bun->data) < 0) {
        fprintf(stderr, "Error binding data to variable 'numThr'.\n");
        return -1;
    }
    if (argv_add("--numThreadsPerLocale=%numThr"))
        return -1;
    bun->used = 1;

    while (fgets(buf, sizeof(buf), fd) != NULL) {
        if (strstr(buf, ") in (") == NULL)
            continue;

        min = LONG_MIN;
        max = LONG_MAX;
        step = 1;

        name = buf;
        while (isspace(*name)) ++name;

        if (sscanf(name, "%*s %*s in (%ld .. %ld) by %ld",
                   &min, &max, &step) != 3 &&
            sscanf(name, "%*s %*s in (%ld .. %ld)", &min, &max) != 2 &&
            sscanf(name, "%*s %*s in (%ld .. )", &min) != 1 &&
            sscanf(name, "%*s %*s in ( .. %ld)", &max) != 1)
        {
            fprintf(stderr, "Malformed Chapel output: target may not be"
                    " a Chapel program.\n");
            return -1;
        }

        if (strchr(name, ':') == NULL) {
            fprintf(stderr, "Malformed Chapel output: target may not be"
                    " a Chapel program.\n");
            return -1;
        }
        *strchr(name, ':') = '\0';

        arg = (char *)malloc((strlen(name) * 2) + 4);
        if (arg == NULL) {
            perror("Malloc error");
            return -1;
        }
        strcpy(arg, name);
        bun = tuna_bundle_add(HVAL_INT, arg);
        if (bun == NULL)
            return -1;

        if (hsession_int(&sess, name, min, max, step) < 0) {
            fprintf(stderr, "Error registering variable '%s'.\n", name);
            return -1;
        }
        if (harmony_bind_int(hdesc, name, bun->data) < 0) {
            fprintf(stderr, "Error binding data to variable '%s'.\n", name);
            return -1;
        }

        arg = (char *)malloc((strlen(name) * 2) + 4);
        if (arg == NULL) {
            perror("Malloc error");
            return -1;
        }
        sprintf(arg, "--%s=%%%s", name, name);
        if (argv_add(arg))
            return -1;

        bun->used = 1;
    }

    return 0;
}

int prepare_client_argv()
{
    unsigned int i = 0;
    int count = 0, len, remainder;
    char *arg;
    strlist_t *arglist;
    bundle_info_t *bun;

    for (arglist = argv_template; arglist != NULL; arglist = arglist->next) {
        client_argv[i++] = argv_buf + count;

        for (arg = arglist->str; *arg != '\0'; ++arg) {
            if (*arg == '%') {
                bun = tuna_bundle_get(&arg);
                if (bun == NULL)
                    return -1;

                remainder = 0;
                if (count < argv_buflen)
                    remainder = argv_buflen - count;

                switch (bun->type) {
                case HVAL_INT:  len = snprintf(argv_buf + count, remainder,
                                               "%ld", *(long *)bun->data);
                    break;
                case HVAL_REAL: len = snprintf(argv_buf + count, remainder,
                                               "%lf", *(double *)bun->data);
                    break;
                case HVAL_STR:  len = snprintf(argv_buf + count, remainder,
                                               "%s", *(char **)bun->data);
                    break;
                default:        assert(0 && "Invalid parameter type.");
                }
                count += len;
            }
            else {
                if (*arg == '\\')
                    ++arg;
                if (count < argv_buflen)
                    *(argv_buf + count) = *arg;
                ++count;
            }
        }
        if (count < argv_buflen)
            *(argv_buf + count) = '\0';
        ++count;
    }
    client_argv[i] = NULL;

    if (count > argv_buflen) {
        argv_buf = (char *)realloc(argv_buf, count);
        if (argv_buf == NULL) {
            perror("Realloc error");
            return -1;
        }
        argv_buflen = count;
        return prepare_client_argv();
    }
    return 0;
}

FILE *tuna_popen(const char *prog, char **argv, pid_t *ret_pid)
{
    int i, pipefd[2];
    FILE *fptr;
    pid_t pid;

    if (pipe(pipefd) != 0) {
        perror("Could not create pipe");
        return NULL;
    }

    if (verbose) {
        printf("Launching %s", prog);
        for (i = 1; argv[i] != NULL; ++i) {
            printf(" %s", argv[i]);
        }
        printf("\n");
    }

    pid = fork();
    if (pid == 0) {
        /* Child Case */
        close(pipefd[0]); /* Close (historically) read side of pipe. */

        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0)
        {
            perror("Could not redirect stdout or stderr via dup2()");
            exit(-1);
        }

        close(pipefd[1]);
        execv(prog, argv);
        exit(-2);
    }
    else if (pid < 0) {
        perror("Error on fork()");
        return NULL;
    }
    close(pipefd[1]); /* Close (historically) write side of pipe. */

    /* Convert raw socket to stream based FILE ptr. */
    fptr = fdopen(pipefd[0], "r");
    if (fptr == NULL) {
        perror("Cannot convert pipefd to FILE ptr");
        exit(-2);
    }

    if (ret_pid)
        *ret_pid = pid;
    return fptr;
}

pid_t launch_silent(const char *prog, char **argv)
{
    int i, fd;
    pid_t pid;

    fd = open("/dev/null", O_WRONLY);
    if (fd < 0) {
        perror("Error opening /dev/null");
        return -1;
    }

    if (verbose) {
        printf("Launching %s", prog);
        for (i = 1; argv[i] != NULL; ++i) {
            printf(" %s", argv[i]);
        }
        printf(" > /dev/null\n");
    }

    pid = fork();
    if (pid == 0) {
        /* Child Case */
        if (dup2(fd, STDOUT_FILENO) < 0 ||
            dup2(fd, STDERR_FILENO) < 0)
        {
            perror("Could not redirect stdout or stderr via dup2()");
            exit(-1);
        }
        close(fd);

        execv(prog, argv);
        exit(-2);
    }
    else if (pid < 0)
        perror("Error on fork()");

    close(fd);
    return pid;
}

double tv_to_double(struct timeval *tv)
{
    double retval = tv->tv_usec;
    retval /= 1000000;
    return retval + tv->tv_sec;
}

int argv_add(char *str)
{
    static strlist_t **tail;

    if (argv_template == NULL)
        tail = &argv_template;

    *tail = (strlist_t *)malloc(sizeof(strlist_t));
    if (*tail == NULL) {
        perror("Malloc error");
        return -1;
    }

    ++client_argc;
    (*tail)->str = str;
    (*tail)->next = NULL;
    tail = &((*tail)->next);
    return 0;
}

bundle_info_t *tuna_bundle_add(hval_type type, char *name)
{
    void *data;

    if (bcount >= MAX_BUNDLE) {
        fprintf(stderr, "Maximum number of tunable parameters"
                " exceeded (%d).\n", MAX_BUNDLE);
        return NULL;
    }

    switch (type) {
    case HVAL_INT:  data = malloc(sizeof(long)); break;
    case HVAL_REAL: data = malloc(sizeof(double)); break;
    case HVAL_STR:  data = malloc(sizeof(char *)); break;
    default: assert(0 && "Invalid parameter type.");
    }

    if (!data) {
        fprintf(stderr, "Malloc error for variable \"%s\".\n", name);
        return NULL;
    }

    binfo[bcount].type = type;
    binfo[bcount].name = name;
    binfo[bcount].data = data;
    binfo[bcount].used = 0;
    return &binfo[bcount++];
}

bundle_info_t *tuna_bundle_get(char **name)
{
    int i;
    char *end = NULL;
    bundle_info_t *retval = NULL;

    if (**name == '%') {
        ++(*name);
        if (**name == '{') {
            end = ++(*name);
            while (*end && *end != '}') ++end;
        }
    }
    if (end == NULL) {
        end = *name;
        while (*end && !isspace(*end)) ++end;
    }

    for (i = 0; i < bcount; ++i) {
        if (strncmp(*name, binfo[i].name, end - *name) == 0 &&
            binfo[i].name[end - *name] == '\0')
        {
            retval = binfo + i;
            break;
            return binfo + i;
        }
    }

    if (retval == NULL)
        fprintf(stderr, "Invalid reference to tunable variable \"%.*s\"\n",
                (int)(end - *name), *name);

    *name = end;
    if (**name != '}')
        --(*name);

    return retval;
}

char *find_exec(const char *filename)
{
    static char fullpath[FILENAME_MAX];
    char *path, *ptr;

    path = getenv("PATH");
    if (path == NULL)
        return NULL;

    while (path != NULL) {
        ptr = strchr(path, ':');
        if (ptr == NULL)
            ptr = path + strlen(path);

        snprintf(fullpath, sizeof(fullpath), "%.*s/%s",
                 (int)(ptr - path), path, filename);

        if (is_exec(fullpath))
            return fullpath;

        if (*ptr == '\0')
            path = NULL;
        else
            path = ptr + 1;
    }
    return NULL;
}

int is_exec(const char *filename)
{
    uid_t uid, euid;
    gid_t gid, egid;
    struct stat sb;

    uid = getuid();
    euid = geteuid();
    gid = getgid();
    egid = getegid();

    return ((stat(filename, &sb) == 0) && (S_ISREG(sb.st_mode) ||
                                           S_ISLNK(sb.st_mode)) &&

            (((sb.st_mode & S_IXOTH) != 0x0) ||
             ((sb.st_mode & S_IXGRP) != 0x0 && (sb.st_gid == gid ||
                                                sb.st_gid == egid ||
                                                egid == 0)) ||
             ((sb.st_mode & S_IXUSR) != 0x0 && (sb.st_uid == uid ||
                                                sb.st_uid == euid ||
                                                euid == 0))));
}