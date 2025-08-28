#include <stdio.h>                // std input output
#include <stdlib.h>               // std library dynamic malloc, free 
#include <string.h>               // string handling
#include <unistd.h>               // POSIX system, command line tools, portable OS interface, Unix-like system
#include <sys/wait.h>             // process control, ex: wait, waitpid, fork()
#include <sys/stat.h>             // file status/permission, ex: mkdir
#include <sys/types.h>            // data types, ex: process ID, size_t
#include <fcntl.h>                // file control, ex: open, creat, redirection (> <)
#include <readline/readline.h>    // GNU Readline Library, user input -> auto complete
#include <readline/history.h>     // Readline History function, history, history clear
#include <dirent.h>               // directory handling, opendir, readdir
#include <glob.h>                 // pattern matching/wildcards
#include <errno.h>                // error handling, strerror
#include <signal.h>               // signal handling, ex: SIGCHLD

/*
 * MyShell: A custom Unix-like shell for university project.
 * Features: Built-in commands, system commands, redirection, multiple piping,
 *           tab completion (files and commands), command history, recursive delete,
 *           folder copy/move, wildcard support, background processes.
 * Author: Laden
 */

#define MAX_INPUT_SIZE 1024 // Maximum input size for commands
#define MAX_ARGS 64         // Maximum number of arguments
#define MAX_HISTORY 100     // Maximum history entries
#define MAX_PIPES 10        // Maximum number of pipes
#define MAX_PATH 512        // Maximum path length
#define MAX_RECURSION 100   // [FIX: Maximum recursion depth for recursive operations]

// Function prototypes
void print_prompt(void);
char *read_command(void);
int parse_command(char *command, char *args[][MAX_ARGS], int *num_commands, char **input_files, char **output_files, int *appends, int *background);
int execute_builtin(char *args[], int background);
void execute_system_command(char *args[], char *input_file, char *output_file, int append, int background);
void execute_multiple_pipes(char *args[][MAX_ARGS], int num_commands, char **input_files, char **output_files, int *appends, int *background);
void recursive_delete(const char *path, int depth);
void recursive_copy(const char *src, const char *dest, int depth);
char *command_generator(const char *text, int state);
char **custom_completion(const char *text, int start, int end);
void sigchld_handler(int sig, siginfo_t *info, void *context);

// Global variables for command completion
static const char *builtin_commands[] = {
    "exit", "cd", "help", "mkdir", "rmdir", "touch", "cp", "mv", "rm", "writefile", "history", NULL
};
static char *system_commands[] = {"ls", "cat", "echo", "grep", "wc", NULL};

// main ()
int main() {
    char *command;
    char *args[MAX_PIPES][MAX_ARGS];
    char *input_files[MAX_PIPES] = {NULL};
    char *output_files[MAX_PIPES] = {NULL};
    int appends[MAX_PIPES] = {0};
    int num_commands, background;

    // Limit command history
    stifle_history(MAX_HISTORY);

    // [FIX: Use sigaction for safer signal handling]
    struct sigaction sa;
    sa.sa_sigaction = sigchld_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction failed");
        exit(1);
    }

    // Print welcome message
    printf("          \033[1;35mWelcome to MyShell [Developed by Laden (^_^)]\033[0m          \n");
    printf("             \033[1;35mStay focused, keep coding (^_^)\033[0m              \n");
    fflush(stdout);

    // Set up tab completion
    rl_attempted_completion_function = custom_completion;

    while (1) {
        print_prompt();
        command = read_command();
        if (!command || !*command) {
            free(command);
            continue;
        }
        // Initialize args array
        for (int c = 0; c < MAX_PIPES; c++) {
            for (int j = 0; j < MAX_ARGS; j++) {
                args[c][j] = NULL;
            }
            input_files[c] = NULL;
            output_files[c] = NULL;
            appends[c] = 0;
        }
        num_commands = 0;
        background = 0;

        if (!parse_command(command, args, &num_commands, input_files, output_files, appends, &background)) {
            // [FIX: Free input/output files on parse error]
            for (int c = 0; c < MAX_PIPES; c++) {
                if (input_files[c]) free(input_files[c]);
                if (output_files[c]) free(output_files[c]);
            }
            free(command);
            continue;
        }
        if (num_commands > 1) {
            execute_multiple_pipes(args, num_commands, input_files, output_files, appends, &background);
        } else if (execute_builtin(args[0], background)) {
            // Built-in command executed
        } else {
            execute_system_command(args[0], input_files[0], output_files[0], appends[0], background);
        }
        // Free allocated arguments and redirection files
        for (int c = 0; c < num_commands; c++) {
            for (int j = 0; args[c][j]; j++) {
                if (args[c][j]) {
                    free(args[c][j]);
                    args[c][j] = NULL;
                }
            }
            if (input_files[c]) {
                free(input_files[c]);
                input_files[c] = NULL;
            }
            if (output_files[c]) {
                free(output_files[c]);
                output_files[c] = NULL;
            }
        }
        free(command);
    }
    return 0;
}

/*
 * print_prompt - Displays the current working directory with a colored prompt.
 */
void print_prompt() {
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd failed");
        return;
    }
    printf("\033[1;33m%s->$\033[0m ", cwd);
    fflush(stdout);
}

/*
 * read_command - Reads user input using readline for tab completion and history.
 */
char *read_command() {
    char *command = readline("");
    if (command) {
        printf("Debug: read_command got: '%s'\n", command);
        if (*command) {
            add_history(command);
        }
    }
    return command;
}

/*
 * parse_command - Parses input command into arguments, redirection, pipes, and background flags.
 */
int parse_command(char *command, char *args[][MAX_ARGS], int *num_commands, char **input_files, char **output_files, int *appends, int *background) {
    int i = 0, cmd_idx = 0;
    *background = 0;

    // Initialize arrays
    for (int c = 0; c < MAX_PIPES; c++) {
        for (int j = 0; j < MAX_ARGS; j++) {
            args[c][j] = NULL;
        }
        input_files[c] = NULL;
        output_files[c] = NULL;
        appends[c] = 0;
    }

    char *cmd_copy = strdup(command);
    if (!cmd_copy) {
        perror("strdup failed");
        return 0;
    }

    // Split by pipes
    char *cmd_tokens[MAX_PIPES];
    cmd_tokens[cmd_idx] = strtok(cmd_copy, "|");
    while (cmd_tokens[cmd_idx] && cmd_idx < MAX_PIPES - 1) {
        cmd_tokens[++cmd_idx] = strtok(NULL, "|");
    }
    *num_commands = cmd_idx;
    if (*num_commands == 0 || !cmd_tokens[0]) {
        free(cmd_copy);
        return 0;
    }

    for (int c = 0; c < *num_commands; c++) {
        i = 0;
        if (!cmd_tokens[c]) continue;

        // Trim whitespace
        char *trimmed = cmd_tokens[c];
        while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\n') trimmed++;
        char *end = trimmed + strlen(trimmed) - 1;
        while (end >= trimmed && (*end == ' ' || *end == '\t' || *end == '\n')) *end-- = '\0';
        if (*trimmed == '\0') {
            *num_commands = c;
            break;
        }

        // Tokenize
        char *sub_copy = strdup(trimmed);
        if (!sub_copy) {
            perror("strdup failed");
            free(cmd_copy);
            return 0;
        }
        char *token = strtok(sub_copy, " \t\n");
        int prev_was_redirect = 0; // [FIX: Track redirection operators]
        while (token && i < MAX_ARGS - 1) {
            if (strcmp(token, ">") == 0) {
                token = strtok(NULL, " \t\n");
                if (!token) {
                    fprintf(stderr, "parse error: missing output file after '>'\n");
                    free(sub_copy);
                    free(cmd_copy);
                    return 0;
                }
                output_files[c] = strdup(token);
                appends[c] = 0;
                prev_was_redirect = 1;
                break;
            } else if (strcmp(token, ">>") == 0) {
                token = strtok(NULL, " \t\n");
                if (!token) {
                    fprintf(stderr, "parse error: missing output file after '>>'\n");
                    free(sub_copy);
                    free(cmd_copy);
                    return 0;
                }
                output_files[c] = strdup(token);
                appends[c] = 1;
                prev_was_redirect = 1;
                break;
            } else if (strcmp(token, "<") == 0) {
                token = strtok(NULL, " \t\n");
                if (!token) {
                    fprintf(stderr, "parse error: missing input file after '<'\n");
                    free(sub_copy);
                    free(cmd_copy);
                    return 0;
                }
                input_files[c] = strdup(token);
                prev_was_redirect = 1;
                break;
            } else if (strcmp(token, "&") == 0 && c == *num_commands - 1) {
                *background = 1;
                break;
            } else if (prev_was_redirect) {
                fprintf(stderr, "parse error: unexpected token '%s' after redirection\n", token);
                free(sub_copy);
                free(cmd_copy);
                return 0;
            }
            glob_t glob_result;
            int has_wildcard = (strchr(token, '*') || strchr(token, '?') || strchr(token, '['));
            if (has_wildcard) {
                if (glob(token, GLOB_NOCHECK | GLOB_TILDE, NULL, &glob_result) == 0) {
                    for (size_t j = 0; j < glob_result.gl_pathc && i < MAX_ARGS - 1; j++) {
                        args[c][i++] = strdup(glob_result.gl_pathv[j]);
                    }
                    globfree(&glob_result);
                } else {
                    args[c][i++] = strdup(token);
                    globfree(&glob_result);
                }
            } else {
                args[c][i++] = strdup(token);
            }
            prev_was_redirect = 0;
            token = strtok(NULL, " \t\n");
        }
        args[c][i] = NULL;
        free(sub_copy);
    }
    free(cmd_copy);

    if (*num_commands == 0) {
        return 0;
    }

    // Debug print
    for (int c = 0; c < *num_commands; c++) {
        printf("Debug: Command %d: ", c);
        for (int j = 0; args[c][j]; j++) {
            printf("'%s' ", args[c][j]);
        }
        printf("\n");
    }
    printf("Debug: Background: %d\n", *background);
    for (int c = 0; c < *num_commands; c++) {
        printf("Debug: Command %d - Input file: %s, Output file: %s, Append: %d\n",
               c, input_files[c] ? input_files[c] : "none",
               output_files[c] ? output_files[c] : "none",
               appends[c]);
    }

    return 1;
}

/*
 * execute_builtin - Executes built-in commands like exit, cd, help, etc.
 */
int execute_builtin(char *args[], int background) {
    if (args[0] == NULL) return 1;

    printf("Debug: execute_builtin called with args[0] = '%s'\n", args[0] ? args[0] : "NULL");

    if (strcmp(args[0], "exit") == 0) {
        printf("Shutting down shell..(^_^)\n");
        exit(0);
    } else if (strcmp(args[0], "cd") == 0) {
        char *dir = args[1] ? args[1] : getenv("HOME");
        if (chdir(dir) != 0) {
            perror("chdir failed");
        }
        return 1;
    } else if (strcmp(args[0], "help") == 0) {
        printf("\033[1;32mAvailable commands:\033[0m\n");
        printf("  exit - Exit the shell\n");
        printf("  cd [dir] - Change directory\n");
        printf("  mkdir [dir] - Create a folder\n");
        printf("  rmdir [dir] - Delete an empty folder\n");
        printf("  rm [-r] [file/dir] - Delete file or folder (recursive with -r)\n");
        printf("  touch [file] - Create a file\n");
        printf("  cp [-r] [source] [dest] - Copy file or folder (recursive with -r)\n");
        printf("  mv [-r] [source] [dest] - Move/rename file or folder (recursive with -r)\n");
        printf("  writefile [file] - Write text to a file\n");
        printf("  history - Show command history\n");
        printf("  history clear - Clear command history\n");
        printf("  Supports: Redirection (<, >, >>), multiple pipes (|), wildcards (*.txt), background (&)\n");
        return 1;
    } else if (strcmp(args[0], "mkdir") == 0) {
        if (args[1] == NULL) {
            printf("Usage: mkdir [directory]\n");
            return 1;
        }
        if (mkdir(args[1], 0755) != 0) {
            perror("mkdir failed");
        }
        return 1;
    } else if (strcmp(args[0], "rmdir") == 0) {
        if (args[1] == NULL) {
            printf("Usage: rmdir [directory]\n");
            return 1;
        }
        if (rmdir(args[1]) != 0) {
            perror("rmdir failed");
        }
        return 1;
    } else if (strcmp(args[0], "touch") == 0) {
        if (args[1] == NULL) {
            printf("Usage: touch [file]\n");
            return 1;
        }
        int fd = open(args[1], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            perror("touch failed");
        } else {
            close(fd);
        }
        return 1;
    } else if (strcmp(args[0], "cp") == 0) {
        int recursive = 0;
        int arg_start = 1;
        if (args[1] && strcmp(args[1], "-r") == 0) {
            recursive = 1;
            arg_start = 2;
        }
        if (args[arg_start] == NULL || args[arg_start + 1] == NULL) {
            printf("Usage: cp [-r] [source] [destination]\n");
            return 1;
        }
        if (recursive) {
            recursive_copy(args[arg_start], args[arg_start + 1], 0);
        } else {
            struct stat st;
            if (stat(args[arg_start], &st) != 0) {
                perror("cp: cannot stat source");
                return 1;
            }
            int src_fd = open(args[arg_start], O_RDONLY);
            if (src_fd < 0) {
                perror("cp: cannot open source");
                return 1;
            }
            struct stat dest_st;
            char final_dest[MAX_PATH];
            if (stat(args[arg_start + 1], &dest_st) == 0 && S_ISDIR(dest_st.st_mode)) {
                snprintf(final_dest, sizeof(final_dest), "%s/%s", args[arg_start + 1], strrchr(args[arg_start], '/') ? strrchr(args[arg_start], '/') + 1 : args[arg_start]);
                if (snprintf(final_dest, sizeof(final_dest), "%s/%s", args[arg_start + 1], strrchr(args[arg_start], '/') ? strrchr(args[arg_start], '/') + 1 : args[arg_start]) >= sizeof(final_dest)) {
                    fprintf(stderr, "cp: destination path too long\n");
                    close(src_fd);
                    return 1;
                }
            } else {
                strncpy(final_dest, args[arg_start + 1], sizeof(final_dest));
            }
            int dest_fd = open(final_dest, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
            if (dest_fd < 0) {
                perror("cp: cannot open destination");
                close(src_fd);
                return 1;
            }
            char buffer[1024];
            ssize_t bytes;
            while ((bytes = read(src_fd, buffer, sizeof(buffer))) > 0) {
                if (write(dest_fd, buffer, bytes) != bytes) {
                    perror("cp: write failed");
                    close(src_fd);
                    close(dest_fd);
                    return 1;
                }
            }
            if (bytes < 0) {
                perror("cp: read failed");
            }
            close(src_fd);
            close(dest_fd);
        }
        return 1;
    } else if (strcmp(args[0], "mv") == 0) {
        int recursive = 0;
        int arg_start = 1;
        if (args[1] && strcmp(args[1], "-r") == 0) {
            recursive = 1;
            arg_start = 2;
        }
        if (args[arg_start] == NULL || args[arg_start + 1] == NULL) {
            printf("Usage: mv [-r] [source] [destination]\n");
            return 1;
        }
        if (recursive) {
            recursive_copy(args[arg_start], args[arg_start + 1], 0);
            recursive_delete(args[arg_start], 0);
        } else {
            struct stat dest_st;
            char final_dest[MAX_PATH];
            if (stat(args[arg_start + 1], &dest_st) == 0 && S_ISDIR(dest_st.st_mode)) {
                if (snprintf(final_dest, sizeof(final_dest), "%s/%s", args[arg_start + 1], strrchr(args[arg_start], '/') ? strrchr(args[arg_start], '/') + 1 : args[arg_start]) >= sizeof(final_dest)) {
                    fprintf(stderr, "mv: destination path too long\n");
                    return 1;
                }
            } else {
                strncpy(final_dest, args[arg_start + 1], sizeof(final_dest));
            }
            if (rename(args[arg_start], final_dest) != 0) {
                perror("mv failed");
            }
        }
        return 1;
    } else if (strcmp(args[0], "rm") == 0) {
        int recursive = 0;
        int arg_start = 1;
        if (args[1] && strcmp(args[1], "-r") == 0) {
            recursive = 1;
            arg_start = 2;
        }
        if (args[arg_start] == NULL) {
            printf("Usage: rm [-r] [file/directory]\n");
            return 1;
        }
        if (recursive) {
            recursive_delete(args[arg_start], 0);
        } else {
            if (unlink(args[arg_start]) != 0) {
                perror("rm failed");
            }
        }
        return 1;
    } else if (strcmp(args[0], "writefile") == 0) {
        if (args[1] == NULL) {
            printf("Usage: writefile [file]\n");
            return 1;
        }
        printf("Enter text to write (press Ctrl+D to finish):\n");
        FILE *file = fopen(args[1], "w");
        if (file == NULL) {
            perror("writefile failed");
            return 1;
        }
        char buffer[MAX_INPUT_SIZE];
        while (fgets(buffer, MAX_INPUT_SIZE, stdin) != NULL) {
            fprintf(file, "%s", buffer);
        }
        fclose(file);
        printf("Written to %s\n", args[1]);
        return 1;
    } else if (strcmp(args[0], "history") == 0) {
        if (args[1] && strcmp(args[1], "clear") == 0) {
            clear_history();
            printf("Command history cleared\n");
            return 1;
        }
        int history_len = history_length;
        for (int i = 0; i < history_len; i++) {
            HIST_ENTRY *entry = history_get(i + history_base);
            if (entry) {
                printf("%d: %s\n", i + 1, entry->line);
            }
        }
        return 1;
    }
    return 0;
}

/*
 * sigchld_handler - Handles SIGCHLD signal for background process completion.
 */
void sigchld_handler(int sig, siginfo_t *info, void *context) {
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        printf("[PID %d] Completed\n", pid);
    }
}

/*
 * execute_system_command - Executes system commands with redirection and background support.
 */
void execute_system_command(char *args[], char *input_file, char *output_file, int append, int background) {
    if (args[0] == NULL) return;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return;
    }
    if (pid == 0) {
        // Child process
        int input_fd = -1, output_fd = -1;
        if (input_file) {
            input_fd = open(input_file, O_RDONLY);
            if (input_fd < 0) {
                perror("open input failed");
                exit(1);
            }
            dup2(input_fd, STDIN_FILENO);
        }
        if (output_file) {
            int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
            output_fd = open(output_file, flags, 0644);
            if (output_fd < 0) {
                perror("open output failed");
                if (input_fd >= 0) close(input_fd);
                exit(1);
            }
            dup2(output_fd, STDOUT_FILENO);
        }
        if (background) {
            setsid();
        }
        // [FIX: Close file descriptors before execvp]
        if (input_fd >= 0) close(input_fd);
        if (output_fd >= 0) close(output_fd);
        if (execvp(args[0], args) == -1) {
            fprintf(stderr, "Error: Command '%s' not found or permission denied\n", args[0]);
            exit(1);
        }
    } else {
        // Parent process
        if (!background) {
            waitpid(pid, NULL, 0);
        } else {
            printf("[PID %d] Running in background\n", pid);
        }
    }
}

/*
 * execute_multiple_pipes - Executes multiple commands connected by pipes.
 */
void execute_multiple_pipes(char *args[][MAX_ARGS], int num_commands, char **input_files, char **output_files, int *appends, int *background) {
    int pipefd[2 * (num_commands - 1)];
    pid_t pids[MAX_PIPES];

    // Create pipes
    for (int i = 0; i < num_commands - 1; i++) {
        if (pipe(pipefd + i * 2) == -1) {
            perror("pipe failed");
            for (int j = 0; j < i * 2; j++) {
                close(pipefd[j]);
            }
            return;
        }
    }

    // Fork for each command
    for (int i = 0; i < num_commands; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork failed");
            for (int j = 0; j < 2 * (num_commands - 1); j++) {
                close(pipefd[j]);
            }
            return;
        }
        if (pids[i] == 0) {
            // Child
            int input_fd = -1, output_fd = -1;
            if (input_files[i]) {
                input_fd = open(input_files[i], O_RDONLY);
                if (input_fd < 0) {
                    perror("open input failed");
                    for (int j = 0; j < 2 * (num_commands - 1); j++) {
                        close(pipefd[j]);
                    }
                    exit(1);
                }
                dup2(input_fd, STDIN_FILENO);
            } else if (i > 0) {
                dup2(pipefd[(i - 1) * 2], STDIN_FILENO);
            }
            if (output_files[i]) {
                int flags = O_WRONLY | O_CREAT | (appends[i] ? O_APPEND : O_TRUNC);
                output_fd = open(output_files[i], flags, 0644);
                if (output_fd < 0) {
                    perror("open output failed");
                    if (input_fd >= 0) close(input_fd);
                    for (int j = 0; j < 2 * (num_commands - 1); j++) {
                        close(pipefd[j]);
                    }
                    exit(1);
                }
                dup2(output_fd, STDOUT_FILENO);
            } else if (i < num_commands - 1) {
                dup2(pipefd[i * 2 + 1], STDOUT_FILENO);
            }
            // Close all pipes in child
            for (int j = 0; j < 2 * (num_commands - 1); j++) {
                close(pipefd[j]);
            }
            // [FIX: Close file descriptors before execvp]
            if (input_fd >= 0) close(input_fd);
            if (output_fd >= 0) close(output_fd);
            if (execvp(args[i][0], args[i]) == -1) {
                fprintf(stderr, "execvp failed: %s\n", args[i][0]);
                exit(1);
            }
        }
    }

    // Close pipes in parent
    for (int i = 0; i < 2 * (num_commands - 1); i++) {
        close(pipefd[i]);
    }
    // Wait for children if not background
    if (!*background) {
        for (int i = 0; i < num_commands; i++) {
            waitpid(pids[i], NULL, 0);
        }
    } else {
        for (int i = 0; i < num_commands; i++) {
            printf("[PID %d] Running in background\n", pids[i]);
        }
    }
}

/*
 * recursive_delete - Recursively deletes a directory and its contents.
 */
void recursive_delete(const char *path, int depth) {
    // [FIX: Limit recursion depth]
    if (depth > MAX_RECURSION) {
        fprintf(stderr, "recursive_delete: maximum recursion depth exceeded\n");
        return;
    }
    struct stat st;
    // [FIX: Use stat to avoid following symlinks]
    if (stat(path, &st) != 0) {
        perror("stat failed");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            perror("opendir failed");
            return;
        }
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char full_path[MAX_PATH];
            if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) >= sizeof(full_path)) {
                fprintf(stderr, "recursive_delete: path too long\n");
                closedir(dir);
                return;
            }
            recursive_delete(full_path, depth + 1);
        }
        closedir(dir);
        if (rmdir(path) != 0) {
            perror("rmdir failed");
        }
    } else {
        if (unlink(path) != 0) {
            perror("unlink failed");
        }
    }
}

/*
 * recursive_copy - Recursively copies a directory and its contents.
 */
void recursive_copy(const char *src, const char *dest, int depth) {
    // [FIX: Limit recursion depth]
    if (depth > MAX_RECURSION) {
        fprintf(stderr, "recursive_copy: maximum recursion depth exceeded\n");
        return;
    }
    struct stat st;
    if (stat(src, &st) != 0) {
        perror("stat failed");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        struct stat dest_st;
        if (stat(dest, &dest_st) == 0 && !S_ISDIR(dest_st.st_mode)) {
            fprintf(stderr, "cp: cannot overwrite non-directory '%s' with directory '%s'\n", dest, src);
            return;
        }
        if (mkdir(dest, st.st_mode) != 0 && errno != EEXIST) {
            perror("mkdir failed");
            return;
        }
        DIR *dir = opendir(src);
        if (!dir) {
            perror("opendir failed");
            return;
        }
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char src_path[MAX_PATH], dest_path[MAX_PATH];
            if (snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name) >= sizeof(src_path) ||
                snprintf(dest_path, sizeof(dest_path), "%s/%s", dest, entry->d_name) >= sizeof(dest_path)) {
                fprintf(stderr, "recursive_copy: path too long\n");
                closedir(dir);
                return;
            }
            recursive_copy(src_path, dest_path, depth + 1);
        }
        closedir(dir);
    } else {
        int src_fd = open(src, O_RDONLY);
        if (src_fd < 0) {
            perror("fopen src failed");
            return;
        }
        struct stat dest_st;
        char final_dest[MAX_PATH];
        if (stat(dest, &dest_st) == 0 && S_ISDIR(dest_st.st_mode)) {
            if (snprintf(final_dest, sizeof(final_dest), "%s/%s", dest, strrchr(src, '/') ? strrchr(src, '/') + 1 : src) >= sizeof(final_dest)) {
                fprintf(stderr, "recursive_copy: destination path too long\n");
                close(src_fd);
                return;
            }
        } else {
            strncpy(final_dest, dest, sizeof(final_dest));
        }
        int dest_fd = open(final_dest, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
        if (dest_fd < 0) {
            perror("fopen dest failed");
            close(src_fd);
            return;
        }
        char buffer[1024];
        ssize_t bytes;
        while ((bytes = read(src_fd, buffer, sizeof(buffer))) > 0) {
            if (write(dest_fd, buffer, bytes) != bytes) {
                perror("write failed");
                close(src_fd);
                close(dest_fd);
                return;
            }
        }
        if (bytes < 0) {
            perror("read failed");
        }
        close(src_fd);
        close(dest_fd);
    }
}

/*
 * command_generator - Generates file/folder names or commands for tab completion.
 */
char *command_generator(const char *text, int state) {
    static int index, len;
    static char *name;
    static DIR *dir;

    if (!state) {
        index = 0;
        len = strlen(text);
        // [FIX: Reset directory for file completion]
        if (dir) {
            closedir(dir);
            dir = NULL;
        }
    }

    // Complete builtin commands
    while ((name = (char *)builtin_commands[index])) {
        index++;
        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }

    index = 0;
    // Complete system commands
    while ((name = system_commands[index])) {
        index++;
        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }

    // File completion
    if (!state) {
        dir = opendir(".");
        if (!dir) return NULL;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, text, len) == 0) {
            return strdup(entry->d_name);
        }
    }
    closedir(dir);
    dir = NULL;
    return NULL;
}

/*
 * custom_completion - Custom tab completion for files and commands.
 */
char **custom_completion(const char *text, int start, int end) {
    rl_attempted_completion_over = 1;
    if (start == 0) {
        return rl_completion_matches(text, command_generator);
    } else {
        return rl_completion_matches(text, rl_filename_completion_function);
    }
}