all: dfc dfs

dfc: dfc.c
	gcc -Wall -Wextra -o dfc dfc.c -lssl -lcrypto

dfs: dfs.c
	gcc -Wall -Wextra -o dfs dfs.c -lpthread

clean:
	rm -f dfc dfs *.o