// Setuid-root helper that restores what SteamOS updates wipe: the passwordless sudo rule and the Monado session
// file. Fixed contents and paths, no arguments, so it can't be used to write anything else.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char SUDOERS[] = "steamos ALL=(ALL:ALL) NOPASSWD: ALL\n";
static const char SESSION[] = "[Desktop Entry]\n"
                              "Name=Monado\n"
                              "Comment=Monado with Valve's tracking, no SteamVR\n"
                              "Exec=/home/steamos/sf/monado-session\n"
                              "Type=Application\n"
                              "DesktopNames=Monado\n";

static int
put(const char *path, const char *data, mode_t mode)
{
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s.sf-restore", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
	if (fd < 0 || write(fd, data, strlen(data)) != (ssize_t)strlen(data) || fchmod(fd, mode) != 0 ||
	    fchown(fd, 0, 0) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
		perror(path);
		return 1;
	}
	printf("restored %s\n", path);
	return 0;
}

int
main(void)
{
	if (setgid(0) != 0 || setuid(0) != 0) {
		perror("setuid (is the binary root-owned with mode 4755?)");
		return 1;
	}
	umask(022);
	mkdir("/etc/steamframe", 0755);
	mkdir("/etc/steamframe/wayland-sessions", 0755);
	return put("/etc/sudoers.d/zz-steamframe", SUDOERS, 0440) |
	       put("/etc/steamframe/wayland-sessions/monado.desktop", SESSION, 0644);
}
