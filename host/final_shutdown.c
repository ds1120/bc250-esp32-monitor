/* Final systemd shutdown hook. Never notify while any disk filesystem is writable.
 * Only libc is needed here. Networking daemons and IP routes may already be gone;
 * the authenticated Ethernet/IP/UDP frame was prepared while the agent was alive.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <mntent.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int storage_safe(void) {
    struct statvfs fs;
    if (statvfs("/", &fs) || !(fs.f_flag & ST_RDONLY)) return 0;
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts) return 0;
    struct mntent *entry;
    int safe = 1;
    while ((entry = getmntent(mounts))) {
        if (!strncmp(entry->mnt_fsname, "/dev/", 5) && hasmntopt(entry, "rw")) {
            safe = 0;
            break;
        }
    }
    endmntent(mounts);
    return safe;
}

int main(int argc, char **argv) {
    int inspect = argc == 2 && !strcmp(argv[1], "--inspect");
    if (argc != 2 || (!inspect && strcmp(argv[1], "poweroff"))) return 0;
    if (!inspect && !storage_safe()) {
        fputs("BC250: no final notification: disk filesystems still writable\n", stderr);
        return 75;
    }
    unsigned char data[1024];
    int fd = open("/run/bc250-power-manager/final-packet", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 077)) { close(fd); return 1; }
    ssize_t size = read(fd, data, sizeof(data));
    close(fd);
    if (size < 104 || memcmp(data, "BC250F1\n", 8)) return 1;
    char interface[17], boot[37], current[64];
    memcpy(interface, data + 8, 16); interface[16] = 0;
    memcpy(boot, data + 24, 36); boot[36] = 0;
    size_t length = ((unsigned)data[60] << 8) | data[61];
    if (length + 62 != (size_t)size || length < 42 || length > 900) return 1;
    FILE *file = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!file || !fgets(current, sizeof(current), file)) { if(file)fclose(file); return 1; }
    fclose(file);
    if (strncmp(current, boot, 36)) return 1;
    if (inspect) {
        printf("Valid current-boot final packet: interface=%s frame_bytes=%zu storage_safe=%d; no frame sent\n", interface, length, storage_safe());
        return 0;
    }
    sync();
    unsigned index = if_nametoindex(interface);
    if (!index) return 1;
    int socket_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_IP));
    if (socket_fd < 0) return 1;
    struct ifreq request = {0};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
    if (ioctl(socket_fd, SIOCGIFFLAGS, &request)) { close(socket_fd); return 1; }
    if (!(request.ifr_flags & IFF_UP)) {
        request.ifr_flags |= IFF_UP;
        if (ioctl(socket_fd, SIOCSIFFLAGS, &request)) { close(socket_fd); return 1; }
    }
    /* Restoring admin-up may require Ethernet carrier negotiation. Bound the wait. */
    char carrier_path[128];
    snprintf(carrier_path, sizeof(carrier_path), "/sys/class/net/%s/carrier", interface);
    for (int i=0; i<50; ++i) {
        int carrier=0;
        file=fopen(carrier_path,"r");
        if(file){ if(fscanf(file,"%d",&carrier)!=1)carrier=0; fclose(file); }
        if(carrier)break;
        usleep(100000);
    }
    struct sockaddr_ll address = {0};
    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(ETH_P_IP);
    address.sll_ifindex = (int)index;
    address.sll_halen = 6;
    memcpy(address.sll_addr, data + 62, 6);
    int sent=0;
    for(int i=0;i<4;++i){
        if(sendto(socket_fd,data+62,length,0,(struct sockaddr*)&address,sizeof(address))==(ssize_t)length)sent=1;
        usleep(200000);
    }
    close(socket_fd);
    return sent?0:1;
}
