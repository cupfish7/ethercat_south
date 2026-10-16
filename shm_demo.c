// shm_demo.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SHM_NAME "/pc_demo"
#define SHM_SIZE 4096

// 共享内存布局
typedef struct {
    int  seq;          // 序列号，用于同步
    int  value;        // 实际数据
    char msg[64];      // 文本消息
} shm_data_t;

int main(void)
{
    // 1. 创建共享内存对象
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return 1;
    }

    // 2. 设置大小（新创建的共享内存默认 0 字节）
    if (ftruncate(fd, SHM_SIZE) == -1) {
        perror("ftruncate");
        return 1;
    }

    // 3. 映射到进程地址空间
    shm_data_t *shm = mmap(NULL, SHM_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // 4. 初始化（只有创建者需要初始化）
    shm->seq = 0;
    shm->value = 0;
    strcpy(shm->msg, "hello");

    // 5. 创建子进程模拟“另一个进程”
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：读共享内存
        for (int i = 0; i < 5; i++) {
            sleep(1);
            printf("[child] seq=%d value=%d msg=%s\n",
                   shm->seq, shm->value, shm->msg);
        }
        exit(0);
    }

    // 父进程：写共享内存
    for (int i = 0; i < 5; i++) {
        sleep(1);
        shm->seq++;
        shm->value += 10;
        snprintf(shm->msg, sizeof(shm->msg), "update %d", i);
        printf("[parent] wrote seq=%d value=%d\n", shm->seq, shm->value);
    }

    // 6. 等待子进程结束
    wait(NULL);

    // 7. 清理：解除映射 + 删除共享内存
    munmap(shm, SHM_SIZE);
    close(fd);
    shm_unlink(SHM_NAME);

    return 0;
}