#define _GNU_SOURCE
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#define STACK_SIZE (1024 * 1024)
#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define MAX_CONTAINERS 32
#define RESPONSE_SIZE 8192

struct child_args {
    char rootfs[512];
    char command[256];
    char hostname[32];
    int log_fd;
};

struct container {
    char id[32];
    pid_t pid;
    pid_t logger_pid;
    int running;
    int stop_requested;
    char state[32];
    char log_path[256];
    void *stack;
    struct child_args *args;
};

static struct container containers[MAX_CONTAINERS];
static int container_count = 0;

static void safe_copy(char *dst, size_t size, const char *src) {
    if (size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

static int find_container_index(const char *id) {
    int i;
    for (i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_container_index_by_pid(pid_t pid) {
    int i;
    for (i = 0; i < container_count; i++) {
        if (containers[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static void free_container_runtime_data(int idx) {
    if (idx < 0 || idx >= container_count) return;

    if (containers[idx].stack) {
        free(containers[idx].stack);
        containers[idx].stack = NULL;
    }

    if (containers[idx].args) {
        free(containers[idx].args);
        containers[idx].args = NULL;
    }
}

static void reap_children(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int idx = find_container_index_by_pid(pid);

        if (idx >= 0) {
            containers[idx].running = 0;

            if (WIFEXITED(status)) {
                safe_copy(containers[idx].state, sizeof(containers[idx].state), "exited");
            } else if (WIFSIGNALED(status)) {
                if (containers[idx].stop_requested) {
                    safe_copy(containers[idx].state, sizeof(containers[idx].state), "stopped");
                } else {
                    safe_copy(containers[idx].state, sizeof(containers[idx].state), "signaled");
                }
            } else {
                safe_copy(containers[idx].state, sizeof(containers[idx].state), "unknown");
            }

            free_container_runtime_data(idx);
        }
    }
}

static int child_main(void *arg) {
    struct child_args *args = (struct child_args *)arg;

    if (args->log_fd >= 0) {
        dup2(args->log_fd, STDOUT_FILENO);
        dup2(args->log_fd, STDERR_FILENO);
        close(args->log_fd);
    }

    if (sethostname(args->hostname, strlen(args->hostname)) != 0) {
        perror("sethostname");
        return 1;
    }

    if (chdir(args->rootfs) != 0) {
        perror("chdir rootfs");
        return 1;
    }

    if (chroot(args->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    mkdir("/proc", 0555);

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc");
        return 1;
    }

    char *cmd[] = { args->command, NULL };
    execv(args->command, cmd);

    perror("exec");
    return 1;
}

static pid_t spawn_logger_process(int read_fd, const char *log_path) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork logger");
        close(read_fd);
        return -1;
    }

    if (pid == 0) {
        int logfd;
        char buffer[1024];
        ssize_t n;

        logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd < 0) {
            perror("open log file");
            close(read_fd);
            _exit(1);
        }

        while ((n = read(read_fd, buffer, sizeof(buffer))) > 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t m = write(logfd, buffer + written, (size_t)(n - written));
                if (m < 0) {
                    perror("write log file");
                    close(logfd);
                    close(read_fd);
                    _exit(1);
                }
                written += m;
            }
        }

        close(logfd);
        close(read_fd);
        _exit(0);
    }

    close(read_fd);
    return pid;
}

static void start_container(const char *id, const char *rootfs, const char *cmd, char *response, size_t response_size) {
    void *stack;
    void *stack_top;
    struct child_args *args;
    int flags;
    pid_t pid;
    int idx;
    int log_pipe[2];
    pid_t logger_pid;
    char log_path[256];

    reap_children();

    if (container_count >= MAX_CONTAINERS) {
        snprintf(response, response_size, "No more container slots available\n");
        return;
    }

    idx = find_container_index(id);
    if (idx >= 0 && containers[idx].running) {
        snprintf(response, response_size, "Container id '%s' already exists and is running\n", id);
        return;
    }

    if (mkdir("logs", 0755) != 0 && errno != EEXIST) {
        snprintf(response, response_size, "Failed to create logs directory: %s\n", strerror(errno));
        return;
    }

    snprintf(log_path, sizeof(log_path), "logs/%s.log", id);
    unlink(log_path);

    if (pipe(log_pipe) != 0) {
        snprintf(response, response_size, "pipe failed: %s\n", strerror(errno));
        return;
    }

    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(log_pipe[0]);
        close(log_pipe[1]);
        snprintf(response, response_size, "malloc failed: %s\n", strerror(errno));
        return;
    }

    args = malloc(sizeof(struct child_args));
    if (!args) {
        close(log_pipe[0]);
        close(log_pipe[1]);
        free(stack);
        snprintf(response, response_size, "malloc failed: %s\n", strerror(errno));
        return;
    }

    memset(args, 0, sizeof(*args));
    safe_copy(args->rootfs, sizeof(args->rootfs), rootfs);
    safe_copy(args->command, sizeof(args->command), cmd);
    safe_copy(args->hostname, sizeof(args->hostname), id);
    args->log_fd = log_pipe[1];

    stack_top = (char *)stack + STACK_SIZE;
    flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    pid = clone(child_main, stack_top, flags, args);
    if (pid < 0) {
        perror("clone");
        close(log_pipe[0]);
        close(log_pipe[1]);
        free(args);
        free(stack);
        snprintf(response, response_size, "clone failed: %s\n", strerror(errno));
        return;
    }

    close(log_pipe[1]);
    logger_pid = spawn_logger_process(log_pipe[0], log_path);

    if (idx >= 0) {
        containers[idx].pid = pid;
        containers[idx].logger_pid = logger_pid;
        containers[idx].running = 1;
        containers[idx].stop_requested = 0;
        containers[idx].stack = stack;
        containers[idx].args = args;
        safe_copy(containers[idx].state, sizeof(containers[idx].state), "running");
        safe_copy(containers[idx].log_path, sizeof(containers[idx].log_path), log_path);
    } else {
        safe_copy(containers[container_count].id, sizeof(containers[container_count].id), id);
        containers[container_count].pid = pid;
        containers[container_count].logger_pid = logger_pid;
        containers[container_count].running = 1;
        containers[container_count].stop_requested = 0;
        containers[container_count].stack = stack;
        containers[container_count].args = args;
        safe_copy(containers[container_count].state, sizeof(containers[container_count].state), "running");
        safe_copy(containers[container_count].log_path, sizeof(containers[container_count].log_path), log_path);
        container_count++;
    }

    snprintf(response, response_size, "Started container '%s' with pid %d\n", id, pid);
}

static void list_containers(char *response, size_t response_size) {
    int i;
    size_t used = 0;

    reap_children();

    used += snprintf(response + used, response_size - used, "ID\tPID\tSTATE\n");
    for (i = 0; i < container_count; i++) {
        if (used < response_size) {
            used += snprintf(response + used, response_size - used,
                             "%s\t%d\t%s\n",
                             containers[i].id,
                             containers[i].pid,
                             containers[i].state);
        }
    }
}

static void stop_container(const char *id, char *response, size_t response_size) {
    int idx = find_container_index(id);
    pid_t pid;

    if (idx < 0) {
        snprintf(response, response_size, "Container not found\n");
        return;
    }

    if (!containers[idx].running) {
        snprintf(response, response_size, "Container '%s' is not running\n", id);
        return;
    }

    pid = containers[idx].pid;
    containers[idx].stop_requested = 1;

    kill(pid, SIGTERM);
    sleep(1);

    if (kill(pid, 0) == 0) {
        kill(pid, SIGKILL);
    }

    sleep(1);
    reap_children();

    if (containers[idx].running) {
        snprintf(response, response_size, "Container '%s' stop requested but still appears running\n", id);
    } else {
        snprintf(response, response_size, "Container '%s' stopped\n", id);
    }
}

static void show_logs(const char *id, char *response, size_t response_size) {
    int idx = find_container_index(id);
    FILE *fp;
    size_t used = 0;

    if (idx < 0) {
        snprintf(response, response_size, "Container not found\n");
        return;
    }

    fp = fopen(containers[idx].log_path, "r");
    if (!fp) {
        snprintf(response, response_size, "No logs found for '%s'\n", id);
        return;
    }

    response[0] = '\0';

    while (!feof(fp) && used < response_size - 1) {
        size_t n = fread(response + used, 1, response_size - 1 - used, fp);
        used += n;
    }

    response[used] = '\0';
    fclose(fp);

    if (used == 0) {
        snprintf(response, response_size, "(log file is empty)\n");
    }
}

static void run_supervisor(const char *base_rootfs) {
    int server_fd;
    int client_fd;
    struct sockaddr_un addr;

    (void)base_rootfs;

    unlink(SOCKET_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_copy(addr.sun_path, sizeof(addr.sun_path), SOCKET_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server_fd);
        return;
    }

    if (chmod(SOCKET_PATH, 0666) != 0) {
        perror("chmod");
    }

    if (listen(server_fd, 5) != 0) {
        perror("listen");
        close(server_fd);
        unlink(SOCKET_PATH);
        return;
    }

    printf("Supervisor listening on %s\n", SOCKET_PATH);

    while (1) {
        char buffer[512];
        char cmd[32];
        char id[64];
        char rootfs[256];
        char command[256];
        char response[RESPONSE_SIZE];
        ssize_t n;

        reap_children();

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        memset(buffer, 0, sizeof(buffer));
        memset(cmd, 0, sizeof(cmd));
        memset(id, 0, sizeof(id));
        memset(rootfs, 0, sizeof(rootfs));
        memset(command, 0, sizeof(command));
        memset(response, 0, sizeof(response));

        n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            close(client_fd);
            continue;
        }

        sscanf(buffer, "%31s %63s %255s %255s", cmd, id, rootfs, command);

        if (strcmp(cmd, "start") == 0) {
            start_container(id, rootfs, command, response, sizeof(response));
        } else if (strcmp(cmd, "ps") == 0) {
            list_containers(response, sizeof(response));
        } else if (strcmp(cmd, "stop") == 0) {
            stop_container(id, response, sizeof(response));
        } else if (strcmp(cmd, "logs") == 0) {
            show_logs(id, response, sizeof(response));
        } else {
            snprintf(response, sizeof(response), "Unknown command\n");
        }

        write(client_fd, response, strlen(response));
        close(client_fd);
    }
}

static void send_command(int argc, char *argv[]) {
    int sock;
    struct sockaddr_un addr;
    char buffer[512];
    char response[RESPONSE_SIZE];
    ssize_t n;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_copy(addr.sun_path, sizeof(addr.sun_path), SOCKET_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("Connect failed. Is supervisor running?\n");
        close(sock);
        return;
    }

    memset(buffer, 0, sizeof(buffer));
    memset(response, 0, sizeof(response));

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 5) {
            printf("Usage: ./engine start <id> <rootfs> <command>\n");
            close(sock);
            return;
        }
        snprintf(buffer, sizeof(buffer), "start %s %s %s", argv[2], argv[3], argv[4]);
    } else if (strcmp(argv[1], "ps") == 0) {
        snprintf(buffer, sizeof(buffer), "ps x x x");
    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            printf("Usage: ./engine stop <id>\n");
            close(sock);
            return;
        }
        snprintf(buffer, sizeof(buffer), "stop %s x x", argv[2]);
    } else if (strcmp(argv[1], "logs") == 0) {
        if (argc < 3) {
            printf("Usage: ./engine logs <id>\n");
            close(sock);
            return;
        }
        snprintf(buffer, sizeof(buffer), "logs %s x x", argv[2]);
    } else {
        printf("Unknown command\n");
        close(sock);
        return;
    }

    write(sock, buffer, strlen(buffer));

    n = read(sock, response, sizeof(response) - 1);
    if (n > 0) {
        response[n] = '\0';
        printf("%s", response);
    }

    close(sock);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("./engine supervisor <base-rootfs>\n");
        printf("./engine start <id> <rootfs> <command>\n");
        printf("./engine ps\n");
        printf("./engine stop <id>\n");
        printf("./engine logs <id>\n");
        return 0;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            printf("Usage: ./engine supervisor <base-rootfs>\n");
            return 0;
        }
        run_supervisor(argv[2]);
    } else {
        send_command(argc, argv);
    }

    return 0;
}
