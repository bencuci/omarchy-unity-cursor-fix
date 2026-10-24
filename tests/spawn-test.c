/*
 * spawn-test: check that the shim adds itself to the environment of a Unity
 * Editor child process and keeps it out of unrelated children.
 *
 *   LD_PRELOAD=../shim/libunity-cursor-wrap.so ./spawn-test execv  /path/to/unityhub-unity-editor-fake
 *   LD_PRELOAD=../shim/libunity-cursor-wrap.so ./spawn-test execv  /bin/sh -c 'echo $LD_PRELOAD'
 */

#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <execv|execve|execvp|spawn|spawnp> <path> [args...]\n", argv[0]);
        return 2;
    }

    const char *mode = argv[1];
    char **target = &argv[2];

    if (strcmp(mode, "execv") == 0) {
        execv(target[0], target);
    } else if (strcmp(mode, "execve") == 0) {
        execve(target[0], target, environ);
    } else if (strcmp(mode, "execvp") == 0) {
        execvp(target[0], target);
    } else if (strcmp(mode, "spawn") == 0 || strcmp(mode, "spawnp") == 0) {
        pid_t pid = 0;
        int rc = (mode[5] == 'p')
                     ? posix_spawnp(&pid, target[0], NULL, NULL, target, environ)
                     : posix_spawn(&pid, target[0], NULL, NULL, target, environ);
        if (rc != 0) {
            fprintf(stderr, "posix_spawn failed: %d\n", rc);
            return 1;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }

    perror("exec");
    return 1;
}
