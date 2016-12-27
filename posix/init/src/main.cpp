
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h> // FIXME: for testing
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h> // FIXME: for testing
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <vector>

//FIXME:
#include <string.h>
#include <hel.h>
#include <hel-syscalls.h>

int main() {
	int fd = open("/dev/helout", O_WRONLY);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	printf("Starting posix-init\n");

	std::vector<char *> args;
	args.push_back(const_cast<char *>("acpi"));
	args.push_back(nullptr);

	char *envp[] = { nullptr };

	auto child = fork();
	if(!child) {
//		execve("/initrd/ata", args.data(), envp);
		execve("/initrd/virtio-block", args.data(), envp);
	}else assert(child != -1);

	// Spin until /dev/sda0 becomes available.
	int sda = -1;
	while((sda = open("/dev/sda0", O_RDONLY)) == -1)
		assert(errno == ENOENT);

/*	
	// TODO: this is a very ugly hack to wait until the fs is ready
	for(int i = 0; i < 10000; i++)
		sched_yield();

	printf("Second fork, here we go!\n");

	pid_t terminal_child = fork();
	assert(terminal_child != -1);
	if(!terminal_child) {
//		execve("/usr/bin/kbd", args.data(), envp);
		execve("/usr/bin/uhci", args.data(), envp);
//		execve("/usr/bin/virtio-net", args.data(), envp);
//		execve("/usr/bin/bochs_vga", args.data(), envp);
//		execve("/usr/bin/zisa", args.data(), envp);
	}
*/	
	// TODO: this is a very ugly hack to wait until the fs is ready
/*	for(int i = 0; i < 10000; i++)
		sched_yield();
	
	printf("Testing network API!\n");
	
	int socket = open("/dev/network/ip+udp", O_RDWR);
	
	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_port = 7;
	address.sin_addr.s_addr = (10 << 24) | (85 << 16) | (1 << 8) | 1;
	connect(socket, (struct sockaddr *)&address, sizeof(struct sockaddr_in));
	write(socket, "hello", 5);*/
}

