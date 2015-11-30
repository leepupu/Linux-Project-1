#include <stdio.h>
#include <stdlib.h>

int main() {
	FILE* fd = fopen("/home/leepupu/Desktop/VMwareTools-9.9.3-2759765.tar.gz", "rb");
	if(!fd)
		printf("fopen err\n");
	int i=0;
	char c;
	c = fgetc(fd);
	while(!feof(fd)) {
		i++;
		c = getc(fd);
	}
	printf("i:%u\n", i);
	char *buf = (char*) malloc(i+5);
	fseek(fd, 0, SEEK_SET);
	i=0;
	c = fgetc(fd);
	while(!feof(fd)) {
		buf[i++] = getc(fd);
	}
	while(1);
}