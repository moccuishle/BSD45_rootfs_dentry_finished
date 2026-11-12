/* minimal freestanding user program: write(1,...) / exit(0) via int 0x80 */
static int sys_write(int fd, const void *buf, unsigned len){
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(4), "b"(fd), "c"(buf), "d"(len) : "memory");
    return r;
}
static void sys_exit(int code){
    __asm__ volatile("int $0x80" :: "a"(1), "b"(code));
    for(;;);
}
void _start(void){
    const char *m = "hello from user mode (ELF32 i386)\r\n";
    sys_write(1, m, 35);
    sys_exit(0);
}
