/*  mrshmprobe — the user-space half of the shared-memory reachability control.
 *
 *  The whole helper-daemon design in src/mr_shared.h rests on one unproven assumption: that
 *  the driver, running inside coreaudiod's sandboxed Core-Audio-Driver-Service helper, can
 *  map a POSIX shared memory region created by an ordinary user-space process. The bundle
 *  declares sandboxSafe=true and an EMPTY AudioServerPlugIn_MachServices array, so there is
 *  a real possibility the sandbox refuses the name outright.
 *
 *  Answering that with a 40-line control before writing the daemon, rather than after, is
 *  the §2 lesson applied to the next unknown.
 *
 *  This creates the region, stamps a magic value into it, and waits. Build the driver with
 *  -DMR_SHM_PROBE=ON and it will try to attach and report what it got.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MR_SHM_NAME  "/mixerreturn.shm"
#define MR_PROBE_MAGIC 0x4D52534D  /* 'MRSM' */

typedef struct {
    uint32_t magic;
    uint32_t counter;
} ProbeRegion;

int main(void)
{
    /* 0666 so it does not matter which user the driver side runs as — coreaudiod's helper
     * runs as _coreaudiod, not as the user that created this. A permissions failure and a
     * sandbox denial both surface as EPERM/EACCES, and this rules the first one out so the
     * result means what it says. */
    int fd = shm_open(MR_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open (creator)");
        return 1;
    }
    if (ftruncate(fd, sizeof(ProbeRegion)) != 0) {
        /* EINVAL here is normal if the region already exists at a different size. */
        perror("ftruncate (non-fatal if region pre-exists)");
    }
    /* shm_open honours umask, so the mode above is not necessarily what landed. */
    fchmod(fd, 0666);

    ProbeRegion* r = mmap(NULL, sizeof(ProbeRegion), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) {
        perror("mmap (creator)");
        return 1;
    }

    r->magic = MR_PROBE_MAGIC;
    r->counter = 0;

    struct stat st;
    fstat(fd, &st);
    printf("created %s  size=%lld mode=%o uid=%d\n",
           MR_SHM_NAME, (long long) st.st_size, st.st_mode & 07777, st.st_uid);
    printf("magic=0x%08X — leaving it mapped; the driver side should now see this.\n", r->magic);
    fflush(stdout);

    /* Tick, so a driver that attaches can prove it is seeing LIVE memory rather than a
     * copy-on-write snapshot or its own freshly created empty region. */
    for (int i = 0; i < 600; ++i) {
        r->counter = (uint32_t) i;
        sleep(1);
    }
    return 0;
}
