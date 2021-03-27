EXECBIN = httpserver
GCC=gcc -Wall -pthread -Wextra -Wpedantic -Wshadow -O2
OBJECTS= httpserver.o

###############################################

all: ${EXECBIN}

${EXECBIN}:${OBJECTS}
	${GCC} -o $@ $^

%.o : %.c
	${GCC} -c $<

clean:
	rm -f *.o

spotless: clean
	rm -f ${EXECBIN}
