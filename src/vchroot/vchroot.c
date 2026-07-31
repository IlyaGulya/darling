#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

extern int lkm_call(int nr, ...);
extern int __darling_vchroot(int dfd);

int main(int argc, const char** argv)
{
    if (argc < 3)
	{
		fprintf(stderr, "vchroot <directory-fd> <binary> [args...]\n");
		return 1;
	}

	char* end = NULL;
	errno = 0;
	long parsed_fd = strtol(argv[1], &end, 10);
	struct stat prefix_stat;
	if (errno != 0 || end == argv[1] || *end != '\0' ||
		parsed_fd < 0 || parsed_fd > INT_MAX ||
		fstat((int)parsed_fd, &prefix_stat) != 0 ||
		!S_ISDIR(prefix_stat.st_mode))
	{
		fprintf(stderr, "Invalid retained vchroot directory capability\n");
		return 1;
	}
	int dfd = (int)parsed_fd;

	const char* target = argv[2];
	while (*target == '/')
		target++;
	if (*target == '\0' || faccessat(dfd, target, F_OK, 0) != 0)
	{
		fprintf(stderr, "Target executable not found below retained prefix: %s\n",
			argv[2]);
		return 5;
	}

	if (fchdir(dfd) == -1)
	{
		perror("fchdir");
		return 2;
	}

	if (__darling_vchroot(dfd) < 0)
	{
		perror("vchroot");
		return 3;
	}

	// This is only needed for this binary and shouldn't be passed down
	unsetenv("DYLD_ROOT_PATH");

	// printf("Will execv %s\n", argv[2]);
	execv(argv[2], (char * const *) argv+2);
	close(dfd);
	perror("execv");

	return 4;
}
