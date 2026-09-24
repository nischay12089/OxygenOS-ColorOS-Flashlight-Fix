#include "service.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "log.h"

namespace flashlightd::service {
namespace {

constexpr char kPidFile[] = "/dev/flashlightd.pid";  // tmpfs: gone after reboot, like init state
constexpr char kSupervisorName[] = "flashlightd-svc";
constexpr int kRestartDelaySec = 5;                  // init's delay before restarting a service
constexpr char kInitService[] = "vendor.flashlightd";  // init/flashlightd.rc
constexpr char kInitStateProp[] = "init.svc.vendor.flashlightd";

std::string property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

// When /vendor/etc/init/flashlightd.rc is installed, init owns the daemon: drive it through
// ctl.start/ctl.stop (what `setprop ctl.*` does) instead of forking an unmanaged copy.
bool initManaged() { return !property(kInitStateProp).empty(); }

int initControl(const char* ctl, const char* want) {
    if (__system_property_set(ctl, kInitService) != 0) {
        std::fprintf(stderr, "flashlightd: %s %s refused\n", ctl, kInitService);
        return 1;
    }
    for (int i = 0; i < 30 && property(kInitStateProp) != want; ++i) usleep(100 * 1000);
    const std::string state = property(kInitStateProp);
    std::printf("flashlightd: init service %s is %s\n", kInitService, state.c_str());
    return state == want ? 0 : 1;
}

volatile sig_atomic_t gStopRequested = 0;
volatile pid_t gChild = 0;

std::string readSmallFile(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return {};
    std::string s(buf, static_cast<size_t>(n));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string selfExe() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return buf;
}

// Supervisor pid from the pidfile, verified by process name so a recycled pid is ignored.
pid_t runningSupervisor() {
    const pid_t pid = atoi(readSmallFile(kPidFile).c_str());
    if (pid <= 0 || kill(pid, 0) != 0) return 0;
    return readSmallFile("/proc/" + std::to_string(pid) + "/comm") == kSupervisorName ? pid : 0;
}

// Direct child of `parent` (the daemon under the supervisor), 0 if none.
pid_t childOf(pid_t parent) {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    pid_t found = 0;
    while (dirent* e = readdir(d)) {
        const pid_t pid = atoi(e->d_name);
        if (pid <= 0) continue;
        // /proc/<pid>/stat: "pid (comm) state ppid ..."; comm may contain spaces.
        const std::string stat = readSmallFile(std::string("/proc/") + e->d_name + "/stat");
        const size_t close = stat.rfind(')');
        if (close == std::string::npos) continue;
        int ppid = 0;
        if (sscanf(stat.c_str() + close + 1, " %*c %d", &ppid) == 1 && ppid == parent) {
            found = pid;
            break;
        }
    }
    closedir(d);
    return found;
}

void onTerminate(int) {
    gStopRequested = 1;
    if (gChild > 0) kill(gChild, SIGTERM);
}

void writePidFile() {
    int fd = open(kPidFile, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    const std::string pid = std::to_string(getpid()) + "\n";
    (void)!write(fd, pid.data(), pid.size());
    close(fd);
}

// Runs the daemon and restarts it whenever it exits, until SIGTERM/SIGINT.
[[noreturn]] void supervise(const std::string& exe) {
    struct sigaction sa = {};
    sa.sa_handler = onTerminate;  // no SA_RESTART: waitpid/nanosleep must wake up on stop
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGHUP, SIG_IGN);

    while (!gStopRequested) {
        const pid_t pid = fork();
        if (pid == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            execl(exe.c_str(), exe.c_str(), "daemon", static_cast<char*>(nullptr));
            _exit(127);
        }
        if (pid < 0) {
            LOGE("fork: %s", strerror(errno));
        } else {
            gChild = pid;
            if (gStopRequested) kill(pid, SIGTERM);  // stop raced with the fork
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            gChild = 0;
            if (gStopRequested) break;
            if (WIFSIGNALED(status)) LOGW("daemon killed by signal %d", WTERMSIG(status));
            else LOGW("daemon exited with %d", WEXITSTATUS(status));
        }
        LOGW("restarting daemon in %d s", kRestartDelaySec);
        timespec delay = {kRestartDelaySec, 0};
        nanosleep(&delay, nullptr);  // cut short by SIGTERM
    }
    LOGI("service stopped");
    if (runningSupervisor() == getpid()) unlink(kPidFile);
    _exit(0);
}

}  // namespace

int start() {
    if (initManaged()) return initControl("ctl.start", "running");
    if (pid_t sv = runningSupervisor()) {
        std::printf("flashlightd: already running (supervisor %d)\n", sv);
        return 0;
    }
    const std::string exe = selfExe();
    if (exe.empty()) {
        LOGE("cannot resolve /proc/self/exe");
        return 1;
    }

    // Classic double fork: the supervisor ends up in its own session, re-parented to init,
    // so it survives the adb shell / su session that started it.
    const pid_t first = fork();
    if (first < 0) {
        LOGE("fork: %s", strerror(errno));
        return 1;
    }
    if (first == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        prctl(PR_SET_NAME, kSupervisorName);
        umask(022);
        if (chdir("/") != 0) _exit(1);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        gLogToStderr = false;
        writePidFile();
        LOGI("service starting");
        supervise(exe);
    }
    waitpid(first, nullptr, 0);

    for (int i = 0; i < 30; ++i) {  // up to 3 s for supervisor + daemon to appear
        if (pid_t sv = runningSupervisor(); sv && childOf(sv)) {
            std::printf("flashlightd: started (supervisor %d, daemon %d)\n", sv, childOf(sv));
            return 0;
        }
        usleep(100 * 1000);
    }
    std::fprintf(stderr, "flashlightd: failed to start, see `logcat -s flashlightd`\n");
    return 1;
}

int stop() {
    if (initManaged()) return initControl("ctl.stop", "stopped");
    const pid_t sv = runningSupervisor();
    if (!sv) {
        unlink(kPidFile);
        std::printf("flashlightd: not running\n");
        return 0;
    }
    const pid_t daemon = childOf(sv);
    kill(sv, SIGTERM);  // supervisor forwards it to the daemon and does not restart
    for (int i = 0; i < 30 && kill(sv, 0) == 0; ++i) usleep(100 * 1000);
    if (kill(sv, 0) == 0) {
        kill(sv, SIGKILL);
        if (daemon > 0) kill(daemon, SIGKILL);
    }
    unlink(kPidFile);
    std::printf("flashlightd: stopped\n");
    return 0;
}

int restart() {
    stop();
    return start();
}

bool printState() {
    if (initManaged()) {
        const std::string state = property(kInitStateProp);
        const std::string pid = property("init.svc_debug_pid.vendor.flashlightd");
        std::printf("service: init %s %s (pid %s)\n", kInitService, state.c_str(),
                    pid.empty() ? "-" : pid.c_str());
        std::fflush(stdout);
        return state == "running";
    }
    const pid_t sv = runningSupervisor();
    if (!sv) {
        std::printf("service: stopped\n");
        std::fflush(stdout);
        return false;
    }
    std::printf("service: running (supervisor %d, daemon %d)\n", sv, childOf(sv));
    std::fflush(stdout);  // keep ordering with the stderr log lines that follow
    return true;
}

}  // namespace flashlightd::service
