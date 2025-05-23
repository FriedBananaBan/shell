#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

// allow us to store info of a job
typedef struct job {
    pid_t pid;
    char *command;
} job;

job jobs[100]; // max of 100 suspended jobs
int numJobs = 0;

// Displays prompt of shell with [nyush baseName]$
void displayPrompt() {
    char cwd[100];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        // Find start of base directory
        int baseIndex = 0;
        for (int i = 0; i < 100 && cwd[i]; i++) {
            if (cwd[i] == '/') {
                baseIndex = i;
            }
        }
        // At root directory use /
        if (baseIndex == 0) {
            printf("[nyush /]$ ");
        }
        // Else print basename of cwd
        else {
            printf("[nyush %.*s]$ ", 100 - baseIndex, &cwd[baseIndex+1]);
        }
   }
}
// Stores each command and argument into array
char *getCmdLine() {
    // Command at most 1000 chars
    char *cmdBuffer = malloc(1000);
    size_t cmdBufferSize = 1000;
    if (getline(&cmdBuffer, &cmdBufferSize, stdin) == -1) {
        free(cmdBuffer);
        return NULL;
    }
    fflush(stdout);
    return cmdBuffer;
}
// Splits the command line into separate arguments
char **splitCommands(char *cmdLine) {
    // Assume less than 10 commands at first
    int bufferSize = 10;
    char **commands = malloc(bufferSize * sizeof(char *));
    char *cur;

    cur = strtok(cmdLine, " \t\r\n\a");
    int commandsIndex = 0;
    while (cur != NULL) {
        commands[commandsIndex] = cur;
        commandsIndex++;
        // If we have more commands than expected, need bigger buffer
        if (commandsIndex >= bufferSize) {
            bufferSize = bufferSize * 2;
            commands = realloc(commands, bufferSize * sizeof(char *));
        }
        cur = strtok(NULL, " \t\r\n\a");
    }
    // NULL so we know when our commmands ended
    commands[commandsIndex] = NULL;
    return commands;
}

// Handled I/O redirection
void redirect(char **args) {
    int index = 0;
    int fd; 

    while (args[index] != NULL) {
        // Check for ">" (will overwrite the file)
        if (strcmp(args[index], ">") == 0) {
            // No file specified
            if (args[index + 1] == NULL) {
                fprintf(stderr, "Error: invalid command\n");
            }
            else {
                fd = open(args[index+1], O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd < 0) {
                    fprintf(stderr, "Can't open this file!\n");
                    exit(EXIT_FAILURE);
                }
                else {
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                    args[index] = NULL;
                }
            }
        }
        // Check for ">>" (will append to the file)
        else if (strcmp(args[index], ">>") == 0) {
            // No file specified
            if (args[index + 1] == NULL) {
                fprintf(stderr, "Error: invalid command\n");
            }
            else {
                fd = open(args[index+1], O_CREAT | O_WRONLY | O_APPEND, 0644);
                if (fd < 0) {
                    fprintf(stderr, "Can't open this file!\n");
                    exit(EXIT_FAILURE);
                }
                else {
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                    args[index] = NULL;
                }
            }
        }
        // Check for "<" (input file)
        else if (strcmp(args[index], "<") == 0) {
            // No file specified
            if (args[index + 1] == NULL) {
                fprintf(stderr, "Error: invalid command\n");
            }
            else {
                fd = open(args[index+1], O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "Error: invalid file\n");
                    exit(EXIT_FAILURE);
                }
                else {
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                    args[index] = NULL;
                }
            }
        }
        index++;
    }
}

int forkAndRun(char **args, char *argString) {
    pid_t pid;
    int status;

    pid = fork();
    // Child process, run the commands
    if (pid == 0) {
        // unignore these signals for processes
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);

        // handle I/O redirections
        redirect(args);

        if (execvp(args[0], args) == -1) {
            fprintf(stderr, "Error: invalid program\n");
        }
        exit(EXIT_FAILURE);
    }
    // Fork error
    else if (pid < 0) {
        fprintf(stderr, "Error forking\n");
    }
    // Parent process, wait for child to finish
    else {
        do {
            waitpid(pid, &status, WUNTRACED);
            if (WIFSTOPPED(status)) {
                // If job is suspended, add to the jobs array
                jobs[numJobs].pid = pid;
                jobs[numJobs].command = strdup(argString);
                numJobs++;
                return 1;
            }
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
    return 1;
}

// Handles pipes
void handlePipes(char ***cmds, int count) {
 int pipefd[2];
    pid_t pid;
    int status;
    int in_fd = 0;

    for (int i = 0; i < count; i++) {
        pipe(pipefd);

        pid = fork();
        if (pid == 0) { 
            dup2(in_fd, STDIN_FILENO); 
            if (i < count - 1) {
                dup2(pipefd[1], STDOUT_FILENO); 
            }
            close(pipefd[0]);
            close(pipefd[1]);

            redirect(cmds[i]);
            if (execvp(cmds[i][0], cmds[i]) == -1) {
                fprintf(stderr, "Error: invalid program\n");
                exit(EXIT_FAILURE);
            }
        } else if (pid < 0) {
            fprintf(stderr, "Fork failed\n");
            exit(EXIT_FAILURE);
        }

        close(pipefd[1]);
        in_fd = pipefd[0];
    }

    while (wait(&status) > 0);
}

// Handles built in shell commands
int runCommands(char **commands, char *cmdLine) {
    // Empty command
    if (commands[0] == NULL) {
        fprintf(stderr, "Error: invalid command\n");
        return 1;
    }
    // Calls cd
    else if (strcmp(commands[0], "cd") == 0) {
        // No path specified
        if (commands[1] == NULL) {
            fprintf(stderr, "Error: invalid command\n");
        }
        else {
            // Too many arguments
            if (commands[2] != NULL) {
                fprintf(stderr, "Error: invalid command\n");
            }
            else {
                // Path not found
                if (chdir(commands[1]) != 0) {
                    fprintf(stderr, "Error: invalid directory\n");
                }
            }
        }
        return 1;
    }
    // Calls exit
    else if (strcmp(commands[0], "exit") == 0) {
        // No arguments allowed with exit
        if (commands[1] != NULL) {
            fprintf(stderr, "Error: invalid command\n");
            return 1;
        }
        else {
            if (numJobs > 0) {
                fprintf(stderr, "Error: there are suspended jobs\n");
                return 1;
            }
            else {
                return 0;
            }
        }
    }
    // calls jobs
    else if (strcmp(commands[0], "jobs") == 0) {
        // No arguments allowed with jobs
        if (commands[1] != NULL) {
            fprintf(stderr, "Error: invalid command\n");
            return 1;
        }
        else {
            for (int i = 0; i < numJobs; i++) {
                printf("[%d] %s", i+1, jobs[i].command);
            }
        }
    }
    // calls fg(index) to run suspended job again
    else if (strcmp(commands[0], "fg") == 0) {
        if (commands[1] == NULL || commands[2] != NULL) {
            fprintf(stderr, "Error: invalid command\n");
        }
        else {
            int index = atoi(commands[1]) - 1;
            if (index < 0 || index >= numJobs) {
                fprintf(stderr, "Error: invalid job\n");
            }
            else {
                pid_t jobPid = jobs[index].pid;
                // use kill to resume
                if (kill(jobPid, SIGCONT) == -1) {
                    fprintf(stderr, "Error resuming job\n");
                    return 1;
                }

                char *cmdCpy = strdup(jobs[index].command);
                // now we remove the job
                free(jobs[index].command);
                for (int i = index; i < numJobs - 1; i++) {
                    jobs[i] = jobs[i+1]; // fill in the hole
                }
                numJobs--;
                // wait for the job to finish
                int status;
                waitpid(jobPid, &status, WUNTRACED);
                if (WIFSTOPPED(status)) {
                    // suspended again, add back in
                    jobs[numJobs].pid = jobPid;
                    jobs[numJobs].command = cmdCpy;
                    numJobs++;
                }
                else {
                    free(cmdCpy);
                }
                return 1;
            }
        }
    }
    // not any self-implement commands, fork and use exec()
    else {
        // handles piping here:
        // we will store each command with in char ***
        int pipeCount = 0;

        // first count how many "|" we have
        for (int i = 0; commands[i] != NULL; i++) {
            if (strcmp(commands[i], "|") == 0) {
                pipeCount++;
            }
        }
        if (pipeCount > 0) {
            // this means we should have pipeCount + 1 different commands to exec
            pipeCount++;
            char ***cmds = malloc(pipeCount * sizeof(char**));

            // split the command array by each execs
            int cmdsIndex = 0; // index to char ***cmds
            int commandsIndex = 0; // index to char** commands

            // first command cannot be a pipe!
            if (strcmp(commands[0], "|") == 0) {
                fprintf(stderr, "Error: invalid command\n");
                return 1;
            }

            // store address of each command in cmds
            for (int i = 0; commands[i] != NULL; i++) {
                if (strcmp(commands[i], "|") == 0) {
                    commands[i] = NULL; // NULL marks end of this command
                    cmds[cmdsIndex++] = &commands[commandsIndex];
                    commandsIndex = i + 1;
                }
            }
            // store the last command
            cmds[cmdsIndex] = &commands[commandsIndex];
            handlePipes(cmds, pipeCount);
            free(cmds);
        }
        // no pipes just run normally
        else {
            return forkAndRun(commands, cmdLine);
        }
    }
    return 1;
}

int main() {
    char *cmdLine;
    char **commands;
    int status;
    setenv("PATH", "/usr/bin:/bin", 1);
    do  
    {
        // Ignore signals SIGINT, SIGQUIT, SIGTSTP
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        displayPrompt();
        cmdLine = getCmdLine(); 
        if (cmdLine == NULL) {
            return 0;
        }
        char *cmdLineCpy = strdup(cmdLine);
        commands = splitCommands(cmdLine); 
        status = runCommands(commands, cmdLineCpy);

        free(cmdLine);
        free(commands);
    } while (status);

    return EXIT_SUCCESS;
}
