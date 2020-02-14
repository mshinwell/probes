#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <linux/ptrace.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "read_note.h"
#include <CAMLprim .

#define CMP_OPCODE 0x3d
#define CALL_OPCODE 0xe8

void raise_error(char * msg)
{
  DEBUG(fprintf(stderr, msg);)
  caml_raise_with_string(*caml_named_value("caml_probes_lib_stub_exception"),
                         msg);
}

void signal_and_error(pid_t cpid, char * msg) {
  ptrace(PTRACE_KILL, cpid, NULL, NULL);
  wait(NULL);
  raise_error(msg);
}

int modify_probe(pid_t cpid, unsigned long addr, bool enable) {
  unsigned long data;
  unsigned long cur;
  unsigned long new;
  errno = 0;
  addr = addr - 5;
  data = ptrace(PTRACE_PEEKTEXT, cpid, addr, NULL);
  if (errno != 0) {
    fprintf (stderr, "Modify probe: read from %lx failed\n", addr);
    return 1;
  }
  if (enable) {
    cur = CMP_OPCODE; new = CALL_OPCODE;
  } else {
    cur = CALL_OPCODE; new = CMP_OPCODE;
  }
  fprintf (stderr, "cur at %lx: %lx\n", addr, data);
  if ((data & 0xff) != cur) {
    fprintf(stderr, "Warning: unexpected instruction at %lx! %lx instead of %lx\n",
            addr, (data & 0xff), cur);
  }
  data = (data & ~0xff) | new;
  fprintf (stderr, "new at %lx: %lx\n", addr, data);
  return ptrace(PTRACE_POKETEXT, cpid, addr, data);
}

int modify_semaphore(pid_t cpid, int delta, size_t addr) {
  errno = 0;
  unsigned long data = ptrace(PTRACE_PEEKDATA, cpid, addr, NULL);
  if (errno != 0) {
    fprintf (stderr, "cur at %lx: %lx\n", addr, data);
    return 1;
  }
  data = data+delta;
  fprintf (stderr, "new at %lx: %lx\n", addr, data);
  return ptrace(PTRACE_POKEDATA, cpid, addr, data);
}

int trace(int argc, char *argv[]) {
  int enable = true;
  if (addr == 0) {
    fprintf(stderr, "Enabling all probes specified in elf notes\n");
    for(int i = 0; i < note_result.num_probes; ++i) {
      struct probe_note *note = note_result.probe_notes[i];
      unsigned long addr = note->offset;
      fprintf(stderr, "probe %d: \"%s\" at %lx with semaphore at %lx\n",
              i, note->name, addr, note->semaphore);

      if (modify_probe(cpid, addr, enable)) {
        fprintf(stderr, "could not rewrite probe at address %lx to %s\n",
                addr, (enable?"enabled":"disabled"));
        goto signal_and_error;
      }

      if (modify_semaphore(cpid, 1, note->semaphore)) {
        fprintf(stderr, "error modifying semaphore\n");
        goto signal_and_error;
      }
    }
  } else {
    if (modify_probe(cpid, addr, enable)) {
      fprintf(stderr, "could not rewrite probe at address %lx to %s\n",
              addr, (enable?"enabled":"disabled"));
      goto signal_and_error;
    }
  }

  if (stay_attached) {
    if (ptrace(PTRACE_SETOPTIONS, cpid, 0, PTRACE_O_EXITKILL)) {
        fprintf(stderr, "cannot jail %d\n", cpid);
    }

    errno = 0;
    if(ptrace(PTRACE_CONT, cpid, NULL, NULL)==-1) {
      fprintf(stderr, "could not continue, errno=%d\n", errno);
      goto signal_and_error;
    }

    status = 0;
    do {
      waitpid(cpid, &status, 0);
      if(WIFEXITED(status)) {
        fprintf(stderr, "child %d exited, status=%d\n", cpid, WEXITSTATUS(status));
      } else if (WIFSIGNALED(status)) {
        int signum = WTERMSIG(status);
        printf("child %d killed by signal %d\n", cpid, signum);
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, cpid, NULL, &regs);
        fprintf (stderr, "signal: %d, eip: 0x%08llx\n", signum, regs.rip);
        return status;
      } else if (WIFSTOPPED(status)) {
        fprintf(stderr, "stopped by signal %d\n", WSTOPSIG(status));
      } else if (WIFCONTINUED(status)) {
        fprintf(stderr, "continued\n");
      } else {
        fprintf(stderr, "child did not exit or signal or continued \n");
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, cpid, NULL, &regs);
        fprintf(stderr, "eip: 0x%08llx\n", regs.rip);
      }
    } while (!WIFEXITED(status) && !WIFSIGNALED(status));

  } else {
    if (ptrace(PTRACE_DETACH, cpid, NULL, NULL)) {
      fprintf(stderr, "could not detach, errno=%d\n", errno);
    }
  }

  return 0;
  signal_and_error:
  ptrace(PTRACE_KILL, cpid, NULL, NULL);
  wait(NULL);
  return 1;
}

int update(pid_t cpid) {

  fprintf(stderr, "Modifying address %lx in running process %d\n", addr, cpid);
  unsigned long data = ptrace(PTRACE_PEEKTEXT, cpid, addr, NULL);
  fprintf (stderr, "before: %lx\n", data);
  if ((data & 0xff) != 0x3d) {
    fprintf(stderr, "Unexpected instruction! %lx\n", (data & 0xff));
    return 1;
  }
  data = (data & ~0xff) | 0xe8;
  fprintf (stderr, "after:  %lx\n", data);
  ptrace(PTRACE_POKETEXT, cpid, addr, data);

  if(ptrace(PTRACE_CONT, cpid, NULL, NULL)==-1) {
    fprintf(stderr, "could not continue, errno=%d\n", errno);
    return 1;
  }

  wait(&status);
  if(!WIFEXITED(status)) {
    int signum = WSTOPSIG(status);
    struct user_regs_struct regs;
    ptrace(PTRACE_GETREGS, cpid, NULL, &regs);
    printf ("signal: %d, eip: 0x%08llx\n", signum, regs.rip);
    return status;
  }
  else {
    fprintf(stderr, "child exited\n");
  }
  return 0;
}

pid_t start (char **argv) {
  pid_t cpid = fork();
  if(cpid==-1) {
    raise_error("error doing fork\n");
  }

  if(cpid==0) {
    if(ptrace(PTRACE_TRACEME, 0, NULL, NULL)) {
      raise_error("ptrace traceme error\n");
    }
    execv(argv[0], argv);
    raise_error("error running exec\n");
  }

  int status = 0;
  wait(&status);
  if(!WIFSTOPPED(status)) {
    signal_and_error(cpid, sprintf("not stopped %d\n", status));
  }
}

CAMLprim value caml_probes_lib_start (value v_argv)
{
  CAMLparam1(v_argv);
  int argc = Wosize_val(v_argv);

  if (argc < 1) {
    raise_error("Missing executable name\n");
  }
  int size = Wosize_val(arg);
  const char ** argv =
    (const char **) caml_stat_alloc((argc + 1 /* for NULL */)
                                    * sizeof(const char *));
  for (i = 0; i < argc; i++) argv[i] = String_val(Field(v_argv, i));
  argv[argc] = NULL;

  pid_t cpid = start(argv);

  caml_stat_free argv;
  CAMLreturn(Val_long(cpid));
}


CAMLprim value caml_probes_lib_attach (value v_pid)
{
  pid_t cpid = Long_val(v_pid);
  if(ptrace(PTRACE_ATTACH, cpid, NULL, NULL)) {
    raise_error(sprintf ("ptrace attach %d error\n" cpid));
  }

  int status = 0;
  pid_t res = waitpid(cpid, &status, WUNTRACED);
  if ((res != cpid) || !(WIFSTOPPED(status)) ) {
    raise_error (sprintf("Unexpected wait result res %d stat %x\n",res,status));
  }
  return Val_unit;
}

CAMLprim value caml_probes_lib_read_notes (value v_filename) {
  struct note_result note_result;
  char *filename = String_val(v_filename);
  if(read_notes(filename, &note_result)) {
    raise_error (sprintf ("could not parse probe notes from %s\n" filename));
  }

  alloc_custom
  caml_alloc_array (caml_copy_string, argv)
../
}

CAMLprim value caml_probes_lib_get_names (value v_filename) {
  // last entry in result->probe_notes array is null, for creating an ocaml array
  result->probe_notes[num_notes] = NULL;
}

CAMLprim value caml_probes_lib_detach (value v_pid)
{
  pid_t cpid = Long_val(v_pid);
  if (ptrace(PTRACE_DETACH, cpid, NULL, NULL)) {
    raise_error (sprintf ("could not detach, errno=%d\n", errno));
  }
  return Val_unit;
}
