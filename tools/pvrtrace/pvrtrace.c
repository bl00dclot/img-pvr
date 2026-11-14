// SPDX-License-Identifier: MIT
//
// pvrtrace: record the PowerVR services bridge calls a process makes, for establishing the DDK 1.13
// client command layouts (docs/ddk113/CAPTURE.md). Development tool: needs root (ptrace of another
// process) and is never shipped in a release.
//
//   pvrtrace <out.bin> <program> [args...]
//
// Runs <program> under ptrace (following threads and forks) and, for every DRM_IOCTL_SRVKM_CMD on the
// PVR render node, writes one record: the bridge group and function, the IN buffer as sent and the
// OUT buffer as returned. For the kicks it also records the client command buffers the IN struct
// points to (TA, 3D-PR, 3D, compute, and each transfer prepare), located through the generated
// DDK 1.13 bridge structs. Records are read back with tools/pvrtrace/decode.py.
//
// The output contains the vendor driver's command streams. It is for local analysis only: never
// commit it (docs/ddk113/CAPTURE.md, "Rules").
//
// Build (armv7, static): $NDK/.../armv7a-linux-androideabi29-clang -O2 -static -Wall \
//   -I src/imagination/vulkan/winsys/pvrsrvkm tools/pvrtrace/pvrtrace.c -o pvrtrace

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pvr_srv_bridge_ddk113.h"

#if !defined(__arm__)
#error "pvrtrace traces 32-bit ARM processes and must itself be built for armv7"
#endif

#define NR_IOCTL 54                 /* ARM EABI __NR_ioctl */
#define DRM_IOCTL_SRVKM_CMD 0xc0206440u /* DRM_IOWR(0x40 + 0, struct drm_pvr_srvkm_cmd), 32 bytes */
#define GROUP_RGXTQ 128u
#define GROUP_RGXCMP 129u
#define GROUP_RGXTA3D 130u
#define MAX_BLOB (1u << 20)
#define MAX_PREPARES 64u

struct drm_srvkm_cmd {
   uint32_t bridge_id, bridge_func_id;
   uint64_t in_data_ptr, out_data_ptr;
   uint32_t in_data_size, out_data_size;
};

/* Record layout (little endian):
 *   "PVRT" u32 version=2
 *   then records: u32 magic 0x52454332 ('REC2'), u32 flags (bit 0 OUT unreadable, bit 1 prepares
 *                 truncated, bit 2 a command blob could not be read), u32 tid, u32 group, u32 func, s32 ioctl_ret,
 *                 u32 in_size, u32 out_size, u32 nblobs, in bytes, out bytes,
 *                 nblobs x { u32 tag, u32 size, bytes }
 *   tags: 1 TA, 2 3D-PR, 3 3D, 4 compute, 0x100+i transfer prepare i. */

static FILE *out;
static unsigned long records, dropped_calls;

static int read_mem(pid_t pid, uint64_t addr, void *dst, size_t size)
{
   if (!size)
      return 0;
   struct iovec local = { dst, size };
   struct iovec remote = { (void *)(uintptr_t)addr, size };
   return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)size ? 0 : -1;
}

static int fd_is_pvr(pid_t pid, int fd)
{
   char link[64], target[PATH_MAX];
   snprintf(link, sizeof(link), "/proc/%d/fd/%d", pid, fd);
   ssize_t n = readlink(link, target, sizeof(target) - 1);
   if (n < 0)
      return 0;
   target[n] = 0;
   return strncmp(target, "/dev/dri/renderD", 16) == 0 || strncmp(target, "/dev/dri/card", 13) == 0;
}

struct blob {
   uint32_t tag, size;
   uint8_t *data;
};

static void add_blob(pid_t pid, struct blob *blobs, uint32_t *n, uint32_t tag, uint64_t ptr,
                     uint32_t size, uint32_t *dropped)
{
   if (!ptr || !size)
      return;
   if (size > MAX_BLOB || *n >= MAX_PREPARES + 4) {
      (*dropped)++;
      return;
   }
   uint8_t *data = malloc(size);
   if (!data || read_mem(pid, ptr, data, size)) {
      free(data);
      (*dropped)++;
      return;
   }
   blobs[(*n)++] = (struct blob){ tag, size, data };
}

/* The client command buffers a kick points at, found through the generated 1.13 IN structs. */
static uint32_t collect_blobs(pid_t pid, uint32_t group, uint32_t func, const uint8_t *in,
                              uint32_t in_size, struct blob *blobs, uint32_t *truncated,
                              uint32_t *dropped)
{
   uint32_t n = 0;
   *truncated = 0;
   *dropped = 0;

   if (group == GROUP_RGXTA3D && func == DDK113_BRIDGE_RGXTA3D_RGXKICKTA3D2 &&
       in_size == sizeof(struct ddk113_in_rgxkickta3d2)) {
      const struct ddk113_in_rgxkickta3d2 *k = (const void *)in;
      add_blob(pid, blobs, &n, 1, k->psTACmd, k->ui32TACmdSize, dropped);
      add_blob(pid, blobs, &n, 2, k->ps3DPRCmd, k->ui323DPRCmdSize, dropped);
      add_blob(pid, blobs, &n, 3, k->ps3DCmd, k->ui323DCmdSize, dropped);
   } else if (group == GROUP_RGXCMP && func == DDK113_BRIDGE_RGXCMP_RGXKICKCDM2 &&
              in_size == sizeof(struct ddk113_in_rgxkickcdm2)) {
      const struct ddk113_in_rgxkickcdm2 *k = (const void *)in;
      add_blob(pid, blobs, &n, 4, k->psDMCmd, k->ui32CmdSize, dropped);
   } else if (group == GROUP_RGXTQ && func == DDK113_BRIDGE_RGXTQ_RGXSUBMITTRANSFER2 &&
              in_size == sizeof(struct ddk113_in_rgxsubmittransfer2)) {
      const struct ddk113_in_rgxsubmittransfer2 *k = (const void *)in;
      uint32_t count = k->ui32PrepareCount < MAX_PREPARES ? k->ui32PrepareCount : MAX_PREPARES;
      *truncated = k->ui32PrepareCount - count;
      uint32_t sizes[MAX_PREPARES];
      uint64_t cmds[MAX_PREPARES];
      if (read_mem(pid, k->pui32CommandSize, sizes, count * sizeof(uint32_t)) ||
          read_mem(pid, k->pui8FWCommand, cmds, count * sizeof(uint64_t)))
         return n;
      for (uint32_t i = 0; i < count; i++)
         add_blob(pid, blobs, &n, 0x100 + i, cmds[i], sizes[i], dropped);
   }
   return n;
}

struct pending {
   pid_t tid;
   uint8_t in_syscall; /* entry/exit toggle for this tracee */
   struct drm_srvkm_cmd cmd;
   uint8_t *in;
   struct blob blobs[MAX_PREPARES + 4]; /* read at entry: what the kernel is about to see */
   uint32_t nblobs;
   uint32_t truncated; /* prepares beyond MAX_PREPARES were not recorded */
   uint32_t dropped;   /* blobs that could not be read or were too large */
};

#define MAX_TRACEES 256
static struct pending pend[MAX_TRACEES];

static void free_blobs(struct pending *p);

static struct pending *slot(pid_t tid)
{
   for (int i = 0; i < MAX_TRACEES; i++)
      if (pend[i].tid == tid)
         return &pend[i];
   for (int i = 0; i < MAX_TRACEES; i++)
      if (!pend[i].tid) {
         pend[i].tid = tid;
         return &pend[i];
      }
   return NULL;
}

static void w32(uint32_t v) { fwrite(&v, 4, 1, out); }

static void on_entry(pid_t tid, struct pt_regs *r, struct pending *p)
{
   free(p->in);
   free_blobs(p);
   p->in = NULL;
   p->cmd.bridge_id = UINT32_MAX;
   if (r->ARM_r7 != NR_IOCTL || (uint32_t)r->ARM_r1 != DRM_IOCTL_SRVKM_CMD || !fd_is_pvr(tid, (int)r->ARM_r0))
      return;
   if (read_mem(tid, r->ARM_r2, &p->cmd, sizeof(p->cmd)))
      return;
   if (p->cmd.in_data_size > MAX_BLOB || p->cmd.out_data_size > MAX_BLOB) {
      p->cmd.bridge_id = UINT32_MAX;
      dropped_calls++;
      return;
   }
   p->in = calloc(1, p->cmd.in_data_size ? p->cmd.in_data_size : 1);
   if (p->in && read_mem(tid, p->cmd.in_data_ptr, p->in, p->cmd.in_data_size)) {
      free(p->in);
      p->in = NULL;
      p->cmd.bridge_id = UINT32_MAX;
      return;
   }
   if (p->in)
      p->nblobs = collect_blobs(tid, p->cmd.bridge_id, p->cmd.bridge_func_id, p->in,
                                p->cmd.in_data_size, p->blobs, &p->truncated, &p->dropped);
}

static void free_blobs(struct pending *p)
{
   for (uint32_t i = 0; i < p->nblobs; i++)
      free(p->blobs[i].data);
   p->nblobs = 0;
   p->truncated = 0;
   p->dropped = 0;
}

static void on_exit_(pid_t tid, struct pt_regs *r, struct pending *p)
{
   if (p->cmd.bridge_id == UINT32_MAX || !p->in)
      return;
   uint8_t *outbuf = calloc(1, p->cmd.out_data_size ? p->cmd.out_data_size : 1);
   if (!outbuf)
      goto done;
   /* Record flags: bit 0 OUT buffer unreadable (contents zero, not the kernel's), bit 1 transfer
    * prepares truncated at MAX_PREPARES. */
   uint32_t flags = 0;
   if (read_mem(tid, p->cmd.out_data_ptr, outbuf, p->cmd.out_data_size))
      flags |= 1;
   if (p->truncated)
      flags |= 2;
   if (p->dropped)
      flags |= 4;
   const uint32_t nblobs = p->nblobs;
   w32(0x52454332u);
   w32(flags);
   w32((uint32_t)tid);
   w32(p->cmd.bridge_id);
   w32(p->cmd.bridge_func_id);
   w32((uint32_t)(int32_t)r->ARM_r0);
   w32(p->cmd.in_data_size);
   w32(p->cmd.out_data_size);
   w32(nblobs);
   fwrite(p->in, 1, p->cmd.in_data_size, out);
   fwrite(outbuf, 1, p->cmd.out_data_size, out);
   for (uint32_t i = 0; i < nblobs; i++) {
      w32(p->blobs[i].tag);
      w32(p->blobs[i].size);
      fwrite(p->blobs[i].data, 1, p->blobs[i].size, out);
   }
   if (fflush(out) || ferror(out)) {
      fprintf(stderr, "pvrtrace: write failed, capture incomplete\n");
      exit(1);
   }
   records++;
   free(outbuf);
done:
   free(p->in);
   free_blobs(p);
   p->in = NULL;
   p->cmd.bridge_id = UINT32_MAX;
}

int main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: %s <out.bin> <program> [args...]\n", argv[0]);
      return 2;
   }
   /* Never overwrite an existing capture; owner-only, it holds vendor command streams. */
   int ofd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   out = ofd >= 0 ? fdopen(ofd, "wb") : NULL;
   if (!out) {
      perror(argv[1]);
      return 1;
   }
   fwrite("PVRT", 1, 4, out);
   w32(2);

   pid_t child = fork();
   if (child < 0) {
      perror("fork");
      return 1;
   }
   if (child == 0) {
      if (ptrace(PTRACE_TRACEME, 0, NULL, NULL))
         _exit(126);
      raise(SIGSTOP);
      execvp(argv[2], &argv[2]);
      perror("execvp");
      _exit(127);
   }

   int status;
   waitpid(child, &status, 0);
   ptrace(PTRACE_SETOPTIONS, child, NULL,
          (void *)(uintptr_t)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK |
                              PTRACE_O_TRACEVFORK | PTRACE_O_EXITKILL));
   ptrace(PTRACE_SYSCALL, child, NULL, NULL);

   int live = 1;
   int exit_code = 0;
   while (live > 0) {
      pid_t tid = waitpid(-1, &status, __WALL);
      if (tid < 0)
         break;
      if (WIFEXITED(status) || WIFSIGNALED(status)) {
         if (tid == child)
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
         struct pending *p = slot(tid);
         if (p) {
            free(p->in);
            free_blobs(p);
            memset(p, 0, sizeof(*p)); /* also clears the entry/exit toggle */
         }
         live--;
         continue;
      }
      int sig = 0;
      if (WIFSTOPPED(status)) {
         int s = WSTOPSIG(status);
         unsigned event = (unsigned)status >> 16;
         if (s == (SIGTRAP | 0x80)) {
            struct pt_regs regs;
            struct pending *p = slot(tid);
            if (!p) {
               fprintf(stderr, "pvrtrace: more than %d threads, giving up\n", MAX_TRACEES);
               exit(1);
            }
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
               if (!p->in_syscall)
                  on_entry(tid, &regs, p);
               else
                  on_exit_(tid, &regs, p);
            }
            p->in_syscall = !p->in_syscall;
         } else if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
                    event == PTRACE_EVENT_VFORK) {
            live++;
         } else if (s != SIGTRAP && s != SIGSTOP) {
            sig = s;
         }
      }
      ptrace(PTRACE_SYSCALL, tid, NULL, (void *)(uintptr_t)sig);
   }

   if (fclose(out)) {
      perror("pvrtrace: closing the capture");
      return 1;
   }
   fprintf(stderr, "pvrtrace: %lu bridge calls recorded, %lu dropped (buffers over %u bytes), program exit %d\n",
           records, dropped_calls, MAX_BLOB, exit_code);
   return exit_code;
}
