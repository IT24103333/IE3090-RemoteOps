CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

all: agent_333 controller_333

agent_333: agent_333.c
	$(CC) $(CFLAGS) -o agent_333 agent_333.c $(LDFLAGS)

controller_333: controller_333.c
	$(CC) $(CFLAGS) -o controller_333 controller_333.c $(LDFLAGS)

clean:
	rm -f agent_333 controller_333

.PHONY: all clean
