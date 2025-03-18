#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

#define REDIRECTION_IN "<"
#define REDIRECTION_OUT ">"
#define PIPE "|"

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "show the current working directory"},
    {cmd_cd, "cd", "change the current working directory"}
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

int cmd_pwd(unused struct tokens* tokens) {
  char *cwd = getcwd(NULL, 0);  // Let getcwd allocate the buffer
    if (cwd != NULL) {
        printf("%s\n", cwd);
        free(cwd);  // Free the dynamically allocated memory
    } else {
        perror("");
        return 1;
    }
    return 0;
}

int cmd_cd(struct tokens* tokens) {
  if (tokens_get_length(tokens) != 2){
    printf("cd command get exactly one argument\n");
  } else{
    if (chdir(tokens_get_token(tokens, 1)) != 0){
      perror("");
    }
  }
  return 0;
}

char* search_path_programs(char* cmd){
  static char path_dir[4096];
  char *path = getenv("PATH");
  char path_copy[4096];
  strncpy(path_copy, path, sizeof(path_copy) - 1);
  char *saveptr;
  char *dir;
  dir = strtok_r(path_copy, ":", &saveptr);
  while (dir != NULL) {
      memset(path_dir, 0, sizeof(path_dir));
      strncpy(path_dir, dir, sizeof(path_dir) - 1);  // Copy the directory path
      path_dir[sizeof(path_dir) - 1] = '\0';          // Ensure null-termination
      strcat(path_dir, "/");
      strcat(path_dir, cmd);
      if (access(path_dir, F_OK) == 0)
        return path_dir;
      dir = strtok_r(NULL, ":", &saveptr);
  } 
  return NULL;
}

int redirect(bool in, char* path){
  int flag;
  int std_fd;
  int file_fd;

  if (in){
    flag = O_RDONLY;
    std_fd = STDIN_FILENO;
  } else {
    flag = O_WRONLY | O_CREAT | O_TRUNC;
    std_fd = STDOUT_FILENO;
  }
  if (in){
    file_fd = open(path, flag);
    if (file_fd < 0) {
        return 1;
    }
  }else {
    file_fd = open(path, flag, 0644);
    if (file_fd < 0) {
        close(file_fd);
        return 1;
    }
  }
  if (dup2(file_fd, std_fd) < 0) {
        close(file_fd);
        return 1;
  }
  
  close(file_fd);
  return 0;
}

int handle_args(char **args, struct tokens* tokens, char* program, size_t start, size_t end){
  char *token;
  bool redirect_in;
  bool redirect_out;
  int index = 1;

  args[0] = program;
  for (size_t i = start + 1; i < end; i++) {
    token =  tokens_get_token(tokens, i);
    if (redirect_in){
      if (redirect(true, token) == 1) {
        return 1;
      }
      redirect_in = false;
    } else if (redirect_out) {
      if (redirect(false, token) == 1){
        return 1;
      }
      redirect_out = false; 
    } else if (strcmp(token, REDIRECTION_IN) == 0){
      redirect_in = true;
      continue;
    } else if (strcmp(token, REDIRECTION_OUT) == 0){
      redirect_out = true;
      continue;
    } else{
      args[index++] = tokens_get_token(tokens, i);
    }
  }
  args[index] = NULL;

  return 0;

}

int exec_program(struct tokens* tokens, size_t start, size_t end, int pipes[][2], int n_proc){
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork failed");
  } else if (pid == 0) {
    if (n_proc != -1){
      if (end != tokens_get_length(tokens)){
        close(pipes[n_proc][0]); // Close unused read end of the pipe
        // Redirect stdout to the write end of the pipe
        if (dup2(pipes[n_proc][1], STDOUT_FILENO) == -1) {
            perror("dup2");
        }
        close(pipes[n_proc][1]); // Close the original write descriptor
      }

      if (start != 0){
        close(pipes[n_proc - 1][1]); // Close unused write end of the pipe
        // Redirect stdin to the read end of the pipe
        if (dup2(pipes[n_proc - 1][0], STDIN_FILENO) == -1) {
            printf("%d\n", n_proc);
            perror("dup2");
        }
        close(pipes[n_proc - 1][0]); // Close the original write descriptor
      }
    }
    char *args[end - start];
    char* program;
    program = search_path_programs(tokens_get_token(tokens, start));
    if (program == NULL)
      program = tokens_get_token(tokens, start);
    if (handle_args(args, tokens, program, start, end) == 1){
      perror("arguments are not correct");
      return 1;
    }
    if (execv(program, args) == -1) {
      perror("execv failed");
      return 1;
    }
  } else{
    int status;
    if (n_proc != -1 && start != 0){
      close(pipes[n_proc - 1][0]);
      close(pipes[n_proc - 1][1]);
    }
    waitpid(pid, &status, 0);
  }
  
  return 0;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char* argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* REPLACE this to run commands as programs. */
      int pipes [4096][2];
      int n_proc = 0;
      size_t start = 0;
      for (size_t i = 0; i < tokens_get_length(tokens); i++) {
        if (strcmp(tokens_get_token(tokens, i), PIPE) == 0){
          if (pipe(pipes[n_proc]) == -1) {
            perror("pipe");
          }
          exec_program(tokens, start, i, pipes, n_proc++);
          start = i + 1;
        }
      }
      if (n_proc == 0) {
        exec_program(tokens, start, tokens_get_length(tokens), pipes, -1);
      } else {
        exec_program(tokens, start, tokens_get_length(tokens), pipes, n_proc);
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
