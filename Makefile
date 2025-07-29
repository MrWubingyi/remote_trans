CC=gcc
CFLAGS=-Wall -O0 -g
TARGET=rdp_forwarder

$(TARGET): rdp_forwarder.c hybrid_transport.c
	$(CC) $(CFLAGS) -o $(TARGET) rdp_forwarder.c hybrid_transport.c

clean:
	rm -f $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/
	chmod +x /usr/local/bin/$(TARGET)

.PHONY: clean install