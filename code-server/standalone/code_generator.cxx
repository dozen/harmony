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

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <cassert>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "code_generator.h"
#include "hcfg.h"
#include "hmesg.h"
#include "hpoint.h"
#include "hsockutil.h"
#include "hutil.h"
#include "defaults.h"

/*
 * Session and configuration information from the Harmony Server.
 */
hmesg_t session_mesg = HMESG_INITIALIZER;
hsession_t *sess;

using namespace std;

typedef struct {
    int pid;
    int step;
    hmesg_t mesg;
    string hostname;
} generator_t;

typedef struct {
    string path;
    string host;
    string user;
    string port;
} url_t;

void generator_main(generator_t &gen);
int codeserver_init(string &filename);
int dir_erase(string &dirname);
int parse_slave_list(const char *hostlist);
int slave_complete(pid_t pid);
vector<long> values_of(hpoint_t *pt);
string vector_to_string(vector<long> &v);
string vector_to_bash_array_local(vector<long> &v);
string vector_to_bash_array_remote(vector<long> &v);
int file_type(const char *fileName);
void logger(const string &message);
double time_stamp(void);
int url_parse(const char *buf, url_t &url);
int mesg_write(hmesg_t &mesg, int step);
int mesg_read(const char *filename, hmesg_t *msg);
int read_loop(int fd, char *buf, int len);
int write_loop(int fd, char *buf, int len);

/*
 * Global Variable Declaration
 */
const char infile_name[] = "candidate";
const char outfile_name[] = "code_complete";
unsigned nchildren = 1;
int timestep = 0;
vector<generator_t> gen_list;

string log_file;
stringstream log_message;

/* Configuration values passed in from the Harmony server. */
string appname, slave_path;
url_t local_url, reply_url, target_url;

/*
 * generators: These are the real work-horses. For each new configuration, we 
 *  fork a new process to generate code. These processes die after the code
 *  generation is complete. We block all the signals because we do not want to
 *  interrupt any code generation activity.
 */
pid_t generator_make(generator_t &gen)
{
    pid_t pid;

    pid = fork();
    if (pid > 0) {
        /* Parent case */
        gen.pid = pid;
        return pid;
    }
    else if (pid < 0) {
        /* Error case */
        cerr << "Error on fork(); " << strerror(errno) << "\n";
        return 0;
    }

    generator_main(gen); // child continues
    return 0;
}

// this gets the code generation parameters from the code manager and fires
//  scripts to use the underlying code generation tool to generate the code.
//  Scripts for different code and different code-generation utility need to
//   be provided by the user.

void generator_main(generator_t &gen)
{
    // this is where the code generation happens
    //  make a call to chill_script.appname.sh
    // Note that appname has to match the name given to this session.

    vector<long> values = values_of(&gen.mesg.data.fetch.cand);

    /* Print a message to the logger */
    log_message.str("");
    log_message << gen.hostname << ": " << vector_to_string(values) << "\n";
    logger(log_message.str());

    // set which machine to use
    // first check to see if there is an underscore in the machine name.
    string generator_name;
    generator_name = gen.hostname.substr(0, gen.hostname.find("_"));

    /* Different machines might be configured differently. So a check here
     * should be made to make sure that the hostname matches uniformly across
     * generator_hosts file and hostname gathered here.
     */

    /* Determine if slave is on a remote host */
    bool flag = (generator_name == local_url.host);

    stringstream ss;
    ss.str("");

    if (!flag) {
        // remote
        ss << "ssh " << generator_name << " ";
    }
    ss << "exec " << slave_path << "/" << gen.hostname
       << "_" << appname << "/chill_script." << appname << ".sh ";

    if (flag) {
        ss << vector_to_bash_array_local(values);
    } else {
        ss << vector_to_bash_array_remote(values);
    }
    ss << generator_name << " "
       << slave_path << "/" << gen.hostname << "_" << appname << " "
       << target_url.host << " "
       << target_url.path;

    cout << "Executing: " << ss.str() << endl;
    int sys_return = system(ss.str().c_str());
    cout << "Returned: " << sys_return << endl;

    while (gen_list.size()) {
        hmesg_fini(&gen_list.back().mesg);
        gen_list.pop_back();
    }

    // error check not done yet.
    exit(0);
}

int main(int argc, char **argv)
{
    stringstream ss;
    int status, num_ready;
    unsigned i;
    pid_t pid;

    if (argc != 2) {
        cerr << "Usage: ./code_generator <codegen_path>\n";
        cerr << " Where <codegen_path> should match the path specified"
                " in the harmony server's configuration file.\n";
        return -1;
    }

    if (file_type(argv[1]) != 2) {
        cerr << argv[1] << " is not a valid directory.  Exiting.\n";
        return -1;
    }

    local_url.path = argv[1];
    local_url.host.clear();

    sess = &session_mesg.data.session;

    // main loop starts here
    // update the log file

    if (dir_erase(local_url.path) < 0) {
        cerr << "Could not prepare local directory for incoming messages.\n";
        return -1;
    }

    string init_filename = local_url.path + "/" + infile_name + ".-1";
    string next_filename;
    while (true) {
        /* Construct the next timestep filename. */
        ss.str("");
        ss << local_url.path << "/" << infile_name << "." << timestep;
        next_filename = ss.str();

        cout << "Waiting to hear from harmony server..." << endl;
        log_message << "Waiting to hear from harmony server...\n";
        while (!file_type(init_filename.c_str()) &&
               !file_type(next_filename.c_str()))
        {
            // Quick check to see if any slaves have completed.
            while ( (pid = waitpid(-1, &status, WNOHANG)) > 0) {
                if (slave_complete(pid) == 0)
                    ++num_ready;
            }
            sleep(1);
        }

        if (file_type(init_filename.c_str())) {
            cout << "Harmony initialization file found." << endl;
            if (codeserver_init(init_filename) < 0) {
                cerr << "Removing invalid configuration file.\n";
            }
            else {
                // record some data? How many variants produced in total?
                timestep = 0;
                num_ready = gen_list.size();
                cout << "Beginning new code server session." << endl;
            }
            remove(init_filename.c_str());
            continue;
        }

        cout << "Filename: " << next_filename << endl;

	double time1__, time2__;
	time1__=time_stamp();

        // Find an available generator slot
        for (i = 0; i < gen_list.size(); ++i) {
            if (gen_list[i].pid == 0) {
                mesg_read(next_filename.c_str(), &gen_list[i].mesg);
                gen_list[i].step = timestep;
                generator_make(gen_list[i]);
                --num_ready;
                break;
            }
        }

        if (i == gen_list.size())
            assert(0 && "Generator vector overflow");

        if (num_ready == 0) {
            // All slaves are busy.  Sit and wait until one returns.
            pid = waitpid(-1, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                cerr << "Process " << i
                     << " (pid " << pid << ") failed.\n";
                exit(1);
            }
            if (slave_complete(pid) == 0)
                ++num_ready;
        }

        time2__=time_stamp();
        double elapsed__=time2__-time1__;
	log_message << "Total time for iteration "<< timestep
                    << " : " << elapsed__ << "\n------------------\n";
        logger(log_message.str());
	log_message.str("");

	/* Remove the conf file we just processed. */
        std::remove(next_filename.c_str());
        cout << "Iteration complete." << endl;

        // increment the timestep
        timestep++;
    } // mainloop

    return 0;
}

int codeserver_init(string &filename)
{
    int msglen;
    const char *cfgval;
    stringstream ss;

    if (mesg_read(filename.c_str(), &session_mesg) < 0) {
        fprintf(stderr, "Could not parse initial message.\n");
        return -1;
    }
    std::remove(filename.c_str());

    if (dir_erase(local_url.path) < 0) {
        cerr << "Could not clear incoming directory.\n";
        return -1;
    }

    appname = sess->sig.name;

    cfgval = hcfg_get(sess->cfg, CFGKEY_CG_SERVER_URL);
    if (!cfgval) {
        cerr << "Session does not define local URL.\n";
        return -1;
    }
    if (url_parse(cfgval, local_url) < 0) {
        cerr << "Invalid local URL: '" << cfgval << "'\n";
        return -1;
    }

    cfgval = hcfg_get(sess->cfg, CFGKEY_CG_TARGET_URL);
    if (!cfgval) {
        cerr << "Session does not define target URL.\n";
        return -1;
    }
    if (url_parse(cfgval, target_url) < 0) {
        cerr << "Invalid target URL: '" << cfgval << "'\n";
        return -1;
    }

    cfgval = hcfg_get(sess->cfg, CFGKEY_CG_REPLY_URL);
    if (!cfgval) {
        cerr << "Session does not define reply URL.\n";
        return -1;
    }
    if (url_parse(cfgval, reply_url) < 0) {
        cerr << "Invalid reply URL: '" << cfgval << "'\n";
        return -1;
    }

    cfgval = hcfg_get(sess->cfg, CFGKEY_CG_SLAVE_LIST);
    if (!cfgval) {
        cerr << "Session does not define slave list.\n";
        return -1;
    }
    if (parse_slave_list(cfgval) < 0) {
        cerr << "Error: codegen_slave_list config directive invalid.\n"
             << "Please fix the harmony server's global config file.\n";
        return -1;
    }

    cfgval = hcfg_get(sess->cfg, CFGKEY_CG_SLAVE_PATH);
    if (!cfgval) {
        cerr << "Session does not define slave directory.\n";
        return -1;
    }
    slave_path = cfgval;

    /* Initialize the application log file. */
    ss.str("");
    ss << "generation." << appname << ".log";
    log_file = ss.str();
    cout << "Generating code for: " << appname << endl;

    cout << "The list of available machines:" << endl;
    log_message << "-------------------------------------------\n";
    log_message << "The list of available machines: ";

    for (unsigned i = 0; i < gen_list.size(); ++i)
    {
        cout << gen_list[i].hostname << " ";
        log_message << gen_list[i].hostname << " ";
    }
    cout << "\n";
    log_message << "\n";

    logger(log_message.str());
    log_message.str("");

    /* Run the setup_code_gen_hosts.sh script. */
    ss.str("");
    ss << "/bin/sh setup_code_gen_hosts.sh " << appname <<  " "
       << slave_path << " " << local_url.host;
    for (unsigned i = 0; i < gen_list.size(); ++i) {
        ss << " " << gen_list[i].hostname;
    }
    if (system(ss.str().c_str()) != 0) {
        cout << "Error on system(" << ss.str() << ")" << endl;
        return -1;
    }

    /* Respond to the harmony server. */
    session_mesg.status = HMESG_STATUS_OK;
    msglen = hmesg_serialize(&session_mesg);
    *(unsigned short *)(session_mesg.buf + sizeof(int)) = ntohs(msglen);

    if (mesg_write(session_mesg, -1) < 0) {
        cerr << "Could not write/send initial reply message.\n";
        return -1;
    }

    cout << "Session initialized.  Ready to generate code.\n";
    return 0;
}

/*
  Helpers
*/

double time_stamp(void)
{
  struct timeval t;
  double time;
  gettimeofday(&t, NULL);
  time = t.tv_sec + 1.0e-6*t.tv_usec;
  return time;
}

/* This function only parses out hosts and paths.  A more
 * sophisticated version will be required when the codeserver is
 * overhauled.
 */
int url_parse(const char *str, url_t &url)
{
    const char *ptr;

    ptr = strstr(str, "//");
    if (!ptr)
        return -1;
    ptr += 2;

    if (strncmp("dir://", str, ptr - str) == 0) {
        url.path = ptr;
        url.host.clear();
        return 0;
    }
    else if (strncmp("ssh://", str, ptr - str) == 0) {
        str = ptr;
        ptr = strchr(str, '@');
        if (ptr) {
            url.user = string(str, ptr - str);
            str = ptr + 1;
        }
        else {
            url.user.clear();
        }

        ptr = strchr(str, ':');
        if (ptr) {
            url.host = string(str, ptr - str);
        }
        else {
            url.host.clear();
            url.port.clear();
        }

        ptr = strchr(str, '/');
        if (!ptr) {
            cerr << "Error parsing URL: No path separator.\n";
            return -1;
        }

        if (url.host.empty())
            url.host = string(str, ptr - str);
        else
            url.port = string(str, ptr - str);

        url.path = ++ptr;
        return 0;
    }
    else if (strncmp("tcp://", str, ptr - str) == 0) {
        /* Not implemented yet. */
    }
    return -1;
}

int dir_erase(string &dirname)
{
    DIR *dirfd;
    struct dirent *dent;
    stringstream initfile;

    dirfd = opendir(dirname.c_str());
    if (!dirfd)
        return -1;

    initfile << infile_name << ".-1";
    while ( (dent = readdir(dirfd))) {
        if (initfile.str() == dent->d_name)
            continue;  /* Do not delete an initial file, if found. */

        if (strncmp(dent->d_name, infile_name, strlen(infile_name)) == 0) {
            string fullpath = dirname + "/" + dent->d_name;
            std::remove(fullpath.c_str());
        }
    }

    if (closedir(dirfd) < 0)
        return -1;

    return 0;
}

void logger(const string &message)
{
    string line;
    ofstream out_file;
    out_file.open(log_file.c_str(),ios::app);
    if(!out_file) 
    {
        cerr << "Error file could not be opened \n";
        exit(1);
    }
    
    out_file << message;
    out_file.flush();
    out_file.close();
}

int parse_slave_list(const char *hostlist)
{
    const char *end, *head, *tail, *host_ptr;
    char *num_ptr;
    string host;
    long num;
    stringstream ss;
    generator_t newgen;

    if (hostlist == NULL)
        return -1;

    while (gen_list.size()) {
        if (gen_list.back().pid)
            hmesg_scrub(&gen_list.back().mesg);
        gen_list.pop_back();
    }

    newgen.pid = 0;
    newgen.mesg = HMESG_INITIALIZER;

    end = hostlist + strlen(hostlist);
    head = hostlist;
    while (head < end) {
        host = "";
        num = -1;

        /* Find the entry boundary. */
        tail = (char *)memchr(head, ',', end - head);
        if (!tail) {
            tail = end;
        }

        /* Skip leading whitespace. */
        while (head < tail && (head == '\0' || isspace(*head))) {
            ++head;
        }
        host_ptr = head;

        /* Find host boundary whitespace. */
        while (head < tail && (head != '\0' && !isspace(*head))) {
            ++head;
        }
        host = string(host_ptr, head++);

        /* Find the unsigned integer after the host. */
        errno = 0;
        num = strtol(head, &num_ptr, 0);
        if (errno != 0) {
            num = -1;
            head = tail;
        } else {
            head = num_ptr;
        }

        /* Skip trailing whitespace. */
        while (head < tail && (head == '\0' || isspace(*head))) {
            ++head;
        }

        /* Error check */
        if (host.empty() || num == -1 || head != tail) {
            cerr << "<Error parsing slave host list ("
                 << hostlist << ")\n";

            while (gen_list.size()) {
                if (gen_list.back().pid)
                    hmesg_scrub(&gen_list.back().mesg);
                gen_list.pop_back();
            }
            return -1;
        }

        for (long i = 1; i <= num; ++i) {
            ss.str("");
            ss << host << "_" << i;
            newgen.hostname = ss.str();

            gen_list.push_back(newgen);
        }
        ++head;
    }
    return 0;
}

int slave_complete(pid_t pid)
{
    for (unsigned i = 0; i < gen_list.size(); ++i) {
        if (gen_list[i].pid == pid) {
            mesg_write(gen_list[i].mesg, gen_list[i].step);
            hmesg_scrub(&gen_list[i].mesg);
            gen_list[i].pid = 0;
            return 0;
        }
    }
    return -1;
}

vector<long> values_of(hpoint_t *pt)
{
    hval_t val;
    vector<long> retval;

    for (int i = 0; i < pt->idx_cap; ++i) {
        index_value(&sess->sig, i, pt->idx[i], &val);
        if (val.type != HVAL_INT) {
            cerr << "Codeserver only implemented for int ranges for now.\n";
            retval.clear();
            break;
        }
        retval.push_back(val.value.i);
    }
    return retval;
}

string vector_to_string(vector<long> &v)
{
    stringstream ss;
    ss << " ";
    for (unsigned i = 0; i < v.size(); i++)
        ss << v[i] << " ";

    return ss.str();
}

string vector_to_bash_array_remote(vector<long> &v)
{
    stringstream ss;
    ss << "\\\"";
    for (unsigned i = 0; i < v.size(); i++)
    {
        ss << v[i] << " ";
    }
    ss << "\\\" ";
    return ss.str();
}

string vector_to_bash_array_local(vector<long> &v)
{
    stringstream ss;
    ss << "\"";
    for (unsigned i = 0; i < v.size(); i++)
    {
        ss << v[i] << " ";
    }
    ss << "\" ";
    return ss.str();
}

int file_type(const char *fileName)
{
    struct stat buf;
    if (fileName == NULL) {
        return 0;
    }

    int i = stat ( fileName, &buf );
    if (i != 0) {
        return 0;

    } else if (S_ISREG(buf.st_mode) && buf.st_size > 0) {
        return 1;

    } else if (S_ISDIR(buf.st_mode)) {
        return 2;
    }
    return 0;
}

int mesg_read(const char *filename, hmesg_t *msg)
{
    int fd;
    unsigned short msglen;
    char hdr[HARMONY_HDRLEN], *newbuf;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        cerr << "Could not open mesg file " << filename
             << ": " << strerror(errno) << "\n";
        return -1;
    }

    if (read_loop(fd, hdr, sizeof(hdr)) < 0)
        return -1;

    if (ntohl(*(unsigned int *)hdr) != HARMONY_MAGIC)
        return -1;

    msglen = ntohs(*(unsigned short *)(hdr + sizeof(int)));
    if (msg->buflen <= msglen) {
        newbuf = (char *) realloc(msg->buf, msglen + 1);
        if (!newbuf)
            return -1;
        msg->buf = newbuf;
        msg->buflen = msglen + 1;
    }

    memcpy(msg->buf, hdr, sizeof(hdr));
    if (read_loop(fd, msg->buf + sizeof(hdr), msglen - sizeof(hdr)) < 0)
        return -1;
    msg->buf[msglen] = '\0';

    if (close(fd) < 0) {
        cerr << "Error closing mesg file " << filename
             << ": " << strerror(errno) << "\n";
        return -1;
    }

    hmesg_scrub(msg);
    return hmesg_deserialize(msg);
}

int mesg_write(hmesg_t &mesg, int step)
{
    stringstream ss;
    string filename;
    unsigned short msglen;
    int fd;

    ss << local_url.path << "/" << outfile_name << "." << step;
    filename = ss.str();
    fd = open(filename.c_str(), O_WRONLY | O_CREAT, 0666);
    if (!fd)
        return -1;

    msglen = ntohs(*(unsigned short *)(mesg.buf + sizeof(int)));

    if (write_loop(fd, mesg.buf, msglen) < 0)
        return -1;

    if (close(fd) < 0)
        return -1;

    if (!reply_url.host.empty()) {
        /* Call scp to transfer the file. */
        ss.str("");
        ss << "scp ";

        if (!reply_url.port.empty())
            ss << "-P " << reply_url.port;

        ss << filename << " ";

        if (!reply_url.user.empty())
            ss << reply_url.user << "@";

        ss << reply_url.host << ":" << reply_url.path;

        system(ss.str().c_str());
        std::remove(filename.c_str());
    }
    return 0;
}

int read_loop(int fd, char *buf, int len)
{
    int count;

    while (len > 0) {
        count = read(fd, buf, len);
        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return -1;

        buf += count;
        len -= count;
    }
    return 0;
}

int write_loop(int fd, char *buf, int len)
{
    int count;

    while (len > 0) {
        count = write(fd, buf, len);
        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return -1;

        buf += count;
        len -= count;
    }
    return 0;
}