#include <err.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <sstream>

#include "ocijail/process.h"
#include "ocijail/tty.h"

namespace fs = std::filesystem;

extern "C" char** environ;

using nlohmann::json;

namespace ocijail {

process::process(const json& process_json,
                 std::optional<std::filesystem::path> console_socket,
                 bool detach,
                 int preserve_fds)
    : console_socket_(console_socket),
      detach_(detach),
      preserve_fds_(preserve_fds) {
    if (!process_json.is_object()) {
        malformed_config("process must be an object");
    }

    if (!process_json.contains("cwd")) {
        malformed_config("no process.cwd");
    }
    auto& process_json_cwd = process_json["cwd"];
    if (!process_json_cwd.is_string()) {
        malformed_config("process.cwd must be a string");
    }
    cwd_ = process_json_cwd;

    if (!process_json.contains("args")) {
        malformed_config("no process.args");
    }
    auto& config_args = process_json["args"];
    if (!config_args.is_array()) {
        malformed_config("process.args must be an array");
    }
    if (config_args.size() == 0) {
        malformed_config("process.args must have at least one element");
    }
    for (auto& arg : config_args) {
        if (!arg.is_string()) {
            malformed_config("process.args must be an array of strings");
        }
        args_.push_back(arg.get<std::string>());
    }

    if (process_json.contains("user")) {
        auto user = process_json["user"];
        if (!user.is_null()) {
            if (!user.is_object()) {
                malformed_config("process.user must be an object");
            }
            if (!user["uid"].is_number()) {
                malformed_config("process.user.uid must be a number");
            }
            uid_ = user["uid"];
            if (!user["gid"].is_number()) {
                malformed_config("process.user.gid must be a number");
            }
            gid_ = user["gid"];
            if (user.contains("umask") && !user["umask"].is_number()) {
                malformed_config("process.user.umask must be a number");
                umask_ = user["umask"];
            }
            gids_.push_back(gid_);
            if (user.contains("additionalGids")) {
                auto gids = user["additionalGids"];
                if (!gids.is_array()) {
                    malformed_config(
                        "process.user.additionalGids must be an array");
                }
                for (auto& gid : gids) {
                    if (!gid.is_number()) {
                        malformed_config(
                            "process.user.additionalGids must be an array of "
                            "numbers");
                    }
                    gids_.push_back(gid);
                }
            }
        }
    } else {
        uid_ = 0;
        gid_ = 0;
        gids_.push_back(0);
    }

    if (process_json.contains("env")) {
        auto& config_env = process_json["env"];
        if (!config_env.is_array()) {
            malformed_config("process.env must be an array");
        }
        for (auto& arg : config_env) {
            if (!arg.is_string()) {
                malformed_config("process.env must be an array of strings");
            }
            env_.push_back(arg.get<std::string>());
        }
    }

    if (process_json.contains("terminal")) {
        if (!process_json["terminal"].is_boolean()) {
            malformed_config("process.terminal must be a boolean");
        }
        terminal_ = process_json["terminal"];
    }
    if (terminal_) {
        if (detach_) {
            if (!console_socket_) {
                throw std::runtime_error{
                    "--console-socket is required when detached if "
                    "process.terminal is true"};
            }
            if (!fs::is_socket(*console_socket_)) {
                throw std::runtime_error{
                    "--console-socket must be a path to a local domain socket"};
            }
        }
    } else if (console_socket_) {
        throw std::runtime_error(
            "--console-socket provided but process.terminal is false");
    }
}

std::optional<std::string_view> process::getenv(std::string_view key) {
    for (const auto& env : env_) {
        std::string_view envv{env.data(), env.size()};
        auto pos = envv.find('=');
        if (key == envv.substr(0, pos)) {
            if (pos == std::string_view::npos) {
                return "";
            } else {
                return envv.substr(pos + 1);
            }
        }
    }
    return std::nullopt;
}

void process::setenv(std::string_view key, std::string_view val) {
    std::stringstream ss;
    ss << key << "=" << val;
    auto keyval = ss.str();
    for (auto& env : env_) {
        std::string_view envv{env.data(), env.size()};
        auto pos = envv.find('=');
        assert(pos != std::string_view::npos);
        if (key == envv.substr(0, pos)) {
            env = keyval;
            return;
        }
    }
    env_.push_back(keyval);
}

void process::validate() {
    if (args_[0][0] == '/') {
        auto cmd = args_[0];
        if (::eaccess(cmd.c_str(), X_OK) < 0) {
            throw std::system_error{errno, std::system_category(), args_[0]};
        }
        if (!fs::is_regular_file(cmd)) {
            throw std::system_error{EACCES, std::system_category(), args_[0]};
        }
        return;
    } else {
        fs::path cmd{args_[0]};
        auto lookup_path = getenv("PATH");
        if (lookup_path) {
            auto path = *lookup_path;
            while (path.size() > 0) {
                auto pos = path.find(':');
                std::string_view path_element;
                if (pos == std::string_view::npos) {
                    path_element = path;
                    path = "";
                } else {
                    path_element = path.substr(0, pos);
                    path = path.substr(pos + 1);
                }
                auto abs_cmd = path_element / cmd;
                if (::eaccess(abs_cmd.c_str(), X_OK) == 0) {
                    return;
                }
            }
        }
        // The command may be relative to the working directory
        auto workdir_cmd = cwd_ / cmd;
        if (::eaccess(workdir_cmd.c_str(), X_OK) == 0 &&
            fs::is_regular_file(workdir_cmd)) {
            return;
        }
        std::stringstream ss;
        ss << "'" << args_[0] << "' not found in $PATH";
        throw std::system_error{ENOENT, std::system_category(), ss.str()};
    }
}

std::tuple<int, int, int> process::pre_start() {
    int stdin_fd, stdout_fd, stderr_fd;
    if (terminal_ && console_socket_) {
        auto [control_fd, tty_fd] = open_pty();
        stdin_fd = stdout_fd = stderr_fd = tty_fd;
        send_pty_control_fd(*console_socket_, control_fd);
    } else {
        stdin_fd = 0;
        stdout_fd = 1;
        stderr_fd = 2;
        // Create a session for the container. Note: for the case
        // where terminal is requested, this happens as part of
        // send_pty_control_fd,
        if (!terminal_ && setsid() < 0) {
            throw std::system_error{
                errno, std::system_category(), "error calling setsid"};
        }
    }
    return {stdin_fd, stdout_fd, stderr_fd};
}

void process::reset_signals() {
    ::sigset_t mask;
    ::sigfillset(&mask);
    if (::sigprocmask(SIG_UNBLOCK, &mask, nullptr) < 0) {
        throw std::system_error{
            errno, std::system_category(), "setting signal mask"};
    }
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    ::sigemptyset(&sa.sa_mask);
    for (int sig = 0; sig < NSIG; sig++) {
        if (::sigaction(sig, &sa, nullptr) < 0 && errno != EINVAL) {
            throw std::system_error{
                errno, std::system_category(), "setting signal handler"};
        }
    }
}

void process::set_uid_gid() {
    if (::setgroups(gids_.size(), &gids_[0]) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling setgroups"};
    }
    if (::setgid(gid_) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling getgid"};
    }
    if (::setuid(uid_) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling setuid"};
    }
    if (::umask(umask_) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling umask"};
    }
}

void process::exec(int stdin_fd, int stdout_fd, int stderr_fd) {
    // Make sure that HOME is set and is not empty
    auto home = getenv("HOME");
    if (!home || *home == "") {
        setenv("HOME", "/");
    }

    // Prepare the environment for execvp.
    std::vector<char*> argv;
    std::vector<char*> envv;
    for (auto& s : env_) {
        envv.push_back(const_cast<char*>(s.c_str()));
    }
    envv.push_back(nullptr);
    for (auto& s : args_) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);
    environ = &envv[0];

    // Set the requested working directory.
    if (chdir(cwd_.c_str()) < 0) {
        throw std::system_error{errno,
                                std::system_category(),
                                "error changing directory to" + cwd_};
    }

    // Unblock signals
    reset_signals();

    // Set the uid, gid etc.
    set_uid_gid();

    // Setup stdin, stdout and stderr. Close everything else.
    if (stdin_fd != 0) {
        ::dup2(stdin_fd, 0);
    }
    if (stdout_fd != 1) {
        ::dup2(stdout_fd, 1);
    }
    if (stderr_fd != 2) {
        ::dup2(stderr_fd, 2);
    }
    ::close_range(3 + preserve_fds_, INT_MAX, CLOSE_RANGE_CLOEXEC);

    // exec the requested command.
    ::execvp(argv[0], &argv[0]);
    ::err(1, "error executing container command");
}

}  // namespace ocijail
