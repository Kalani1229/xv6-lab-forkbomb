// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);
void runcmd(struct cmd*) __attribute__((noreturn));

// Global array to track background job PIDs
int jobs[NPROC];
int njobs = 0;  // Number of background jobs

// Remove a PID from the jobs array
void remove_job(int pid) {
  for(int i = 0; i < njobs; i++){
    if(jobs[i] == pid){
      // Move all elements after i one position to the left
      for(int j = i; j < njobs - 1; j++){
        jobs[j] = jobs[j + 1];
      }
      njobs--;
      break;
    }
  }
}

// Add a PID to the jobs array
void add_job(int pid) {
  if(njobs < NPROC){
    jobs[njobs] = pid;
    njobs++;
  }
}

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(1);
    // If the command doesn't start with '/', prepend '/'
    char *path = ecmd->argv[0];
    char fullpath[512];
    if(path[0] != '/'){
      fullpath[0] = '/';
      strcpy(fullpath + 1, path);
      path = fullpath;
    }
    exec(path, ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    exit(0);  // Exit with status 0 if exec fails (as per test requirements)

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    // 遞迴呼叫 runcmd 來執行 bcmd->cmd
    // 因為我們在 main 中已經 fork 
    // 這裡的 runcmd 是在子進程中被呼叫的
    runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

int
getcmd(char *buf, int nbuf)
{
  // write(2, "$ ", 2); // <<< 刪除或註解掉這一行
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}

// Read a line from file descriptor fd into buf
int
getline_from_fd(int fd, char *buf, int nbuf)
{
  memset(buf, 0, nbuf);
  int i = 0;
  char c;
  while(i < nbuf - 1){
    if(read(fd, &c, 1) <= 0){
      if(i == 0)
        return -1; // EOF
      break;
    }
    if(c == '\n'){
      buf[i] = '\n';
      i++;
      break;
    }
    buf[i] = c;
    i++;
  }
  buf[i] = 0;
  return 0;
}

int
main(int argc, char* argv[])
{
  static char buf[100];
  int fd;
  struct cmd *cmd; // <<< 把 cmd 宣告移到迴圈外
  int script_fd = -1;  // File descriptor for script file
  
  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // If script file is provided, open it
  if(argc > 1){
    script_fd = open(argv[1], O_RDONLY);
    if(script_fd < 0){
      fprintf(2, "sh: cannot open %s\n", argv[1]);
      exit(1);
    }
  }

  // Read and run input commands.
  while(1){
    // 1. 在印出 $「之前」，回收殭屍
    int exit_status;
    int bg_pid;
    while((bg_pid = wait_noblock(&exit_status)) > 0){
      printf("[bg %d] exited with status %d\n", bg_pid, exit_status);
      remove_job(bg_pid);  // Remove from jobs array
    }

    // 2. 現在由 main 負責印出 $ (only if not reading from script)
    if(script_fd < 0){
      write(2, "$ ", 2);
    }

    // 3. 從腳本文件或標準輸入讀取命令
    if(script_fd >= 0){
      // Read from script file
      if(getline_from_fd(script_fd, buf, sizeof(buf)) < 0){
        close(script_fd);
        break; // End of script file
      }
    } else {
      // Read from standard input
      if(getcmd(buf, sizeof(buf)) < 0){
        break; // 偵測到 EOF (Ctrl+D)，退出 shell
      }
    }

    // Check for completed background processes after reading command
    // This ensures background process output appears before the next command
    while((bg_pid = wait_noblock(&exit_status)) > 0){
      printf("[bg %d] exited with status %d\n", bg_pid, exit_status);
      remove_job(bg_pid);  // Remove from jobs array
    }

    // Skip empty lines
    if(buf[0] == '\n' || buf[0] == '\0'){
      continue;
    }

    // Handle jobs command (must be checked before parsecmd)
    // Check if the command is exactly "jobs" followed by newline, null, space, or carriage return
    if(buf[0] == 'j' && buf[1] == 'o' && buf[2] == 'b' && buf[3] == 's'){
      // Check if it's exactly "jobs" with optional trailing whitespace
      if(buf[4] == '\n' || buf[4] == '\0' || buf[4] == ' ' || buf[4] == '\t' || buf[4] == '\r'){
        // Print all background job PIDs
        for(int i = 0; i < njobs; i++){
          printf("%d\n", jobs[i]);
        }
        continue;
      }
    }

    if(buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' '){
      // Chdir must be called by the parent, not the child.
      buf[strlen(buf)-1] = 0;  // chop \n
      if(chdir(buf+3) < 0)
        fprintf(2, "cannot cd %s\n", buf+3);
      continue;
    }

    cmd = parsecmd(buf); //1. 在 fork 之前解析
    int pid = fork1();
    if(pid == 0){
      if(cmd->type == BACK){
        // 3. 如果是背景，執行「裡面」的命令
        struct backcmd *bcmd = (struct backcmd*)cmd;
        runcmd(bcmd->cmd);
      } else {
        // 4. 如果是前景，正常執行
        runcmd(cmd);
      }
    }else if(pid > 0){
      // --- 父進程 (Shell) ---
      if(cmd->type == BACK){
        // 5. 只有「不是」背景命令時，才 wait()
        // Check for completed background processes BEFORE printing new PID
        // This ensures correct output order
        while((bg_pid = wait_noblock(&exit_status)) > 0){
          printf("[bg %d] exited with status %d\n", bg_pid, exit_status);
          remove_job(bg_pid);  // Remove from jobs array
        }
        printf("[%d]\n", pid);
        add_job(pid);  // Add to jobs array
        // Check for completed background processes after starting a new one
        // This catches quickly-completing background processes
        while((bg_pid = wait_noblock(&exit_status)) > 0){
          printf("[bg %d] exited with status %d\n", bg_pid, exit_status);
          remove_job(bg_pid);  // Remove from jobs array
        }
      }else {
        // Wait for foreground process, but check background processes periodically
        int fg_pid = pid;
        int wpid;
        int exit_status;
        int fg_done = 0;
        while(!fg_done){
          // First check for background processes
          while((bg_pid = wait_noblock(&exit_status)) > 0){
            if(bg_pid == fg_pid){
              // Foreground process is done
              fg_done = 1;
              break;
            }
            printf("[bg %d] exited with status %d\n", bg_pid, exit_status);
            remove_job(bg_pid);  // Remove from jobs array
          }
          if(fg_done){
            break; // Foreground process completed
          }
          // No zombie processes, use blocking wait with status
          wpid = wait(&exit_status);
          if(wpid == fg_pid){
            fg_done = 1;
            break; // Foreground process completed
          } else if(wpid > 0){
            // Some background process completed
            printf("[bg %d] exited with status %d\n", wpid, exit_status);
            remove_job(wpid);
            continue;
          } else {
            break; // Error or no children
          }
        }
      }
      // 如果是 BACK 命令，父進程什麼都不做，直接印下一個 $
    }
  }
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
