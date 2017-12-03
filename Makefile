# ENVIRONMENT VARIABLES
#
# SIMPLETUN_BINARY_DIR  -- Prefix for generated files (default build/)
# SIMPLETUN_LOGS_DIR 	-- Prefix for generated files (default logs/)
#
# SIMPLETUN_PEER 		-- IP address of the peer gateway (mandatory)
# SIMPLETUN_PEER_PORT 	-- UDP port of the peer gateway (default 55555)
# SIMPLETUN_LOCAL_PORT 	-- UDP port of outgoing packets (default 55555)
# SIMPLETUN_NET  		-- Private network IP (mandatory)

INCLUDE_DIRS = -Iinclude

CC = gcc
CC_FLAGS = -Wall -Wextra -Werror -std=gnu99 $(INCLUDE_DIRS)

ifdef SIMPLETUN_DEBUG
CC_FLAGS += -DDEBUG
endif

ifndef SIMPLETUN_BINARY_DIR
SIMPLETUN_BINARY_DIR = build
endif
ifndef SIMPLETUN_LOGS_DIR
SIMPLETUN_LOGS_DIR = logs
endif

ifndef SIMPLETUN_LOCAL_PORT
SIMPLETUN_LOCAL_PORT = 55555
endif
ifndef SIMPLETUN_PEER_PORT
SIMPLETUN_PEER_PORT = 55555
endif

ifndef SIMPLETUN_KEY
SIMPLETUN_KEY = $(SIMPLETUN_BINARY_DIR)/key
endif
ifndef SIMPLETUN_IV
SIMPLETUN_IV = $(SIMPLETUN_BINARY_DIR)/iv
endif

all: $(SIMPLETUN_BINARY_DIR) $(SIMPLETUN_LOGS_DIR) simpletun

clean:
	rm -f ./$(SIMPLETUN_BINARY_DIR)/simpletun
	rm -f ./$(SIMPLETUN_BINARY_DIR)/key
	rm -f ./$(SIMPLETUN_BINARY_DIR)/iv
	rm -f ./$(SIMPLETUN_BINARY_DIR)/*.o
	rm -f ./$(SIMPLETUN_LOGS_DIR)/*.out
	rm -f ./$(SIMPLETUN_LOGS_DIR)/*.err
	rmdir $(SIMPLETUN_BINARY_DIR)
	rmdir $(SIMPLETUN_LOGS_DIR)

$(SIMPLETUN_BINARY_DIR)/tunnel.o: src/tunnel.c include/tunnel.h $(SIMPLETUN_BINARY_DIR)
	$(CC) $(CC_FLAGS) -c -o $@ $< -lssl -lcrypto

simpletun: $(SIMPLETUN_BINARY_DIR)/simpletun
$(SIMPLETUN_BINARY_DIR)/simpletun: src/simpletun.c $(SIMPLETUN_BINARY_DIR)/tunnel.o
	$(CC) $(CC_FLAGS) -o $@ $^ -lssl -lcrypto

tunnel: simpletun simpletun_net simpletun_peer stop_tunnel $(SIMPLETUN_LOGS_DIR) $(SIMPLETUN_KEY) $(SIMPLETUN_IV)
	$(SIMPLETUN_BINARY_DIR)/simpletun --port $(SIMPLETUN_LOCAL_PORT) \
		--peer-ip $(SIMPLETUN_PEER) --peer-port $(SIMPLETUN_PEER_PORT) \
		--encryption-key $(SIMPLETUN_KEY) --encryption-iv $(SIMPLETUN_IV) \
		--network $(SIMPLETUN_NET) --netmask 255.255.255.0 \
		> $(SIMPLETUN_LOGS_DIR)/simpletun.out 2> $(SIMPLETUN_LOGS_DIR)/simpletun.err &

stop_tunnel:
	killall -q -9 simpletun || true # killall returns 1 if no processes were killed
	sleep 1
	! (ps -e | grep simpletun)

$(SIMPLETUN_BINARY_DIR):
	mkdir $@

$(SIMPLETUN_LOGS_DIR):
	mkdir $@

$(SIMPLETUN_KEY):
	head -c 32 /dev/urandom > $(SIMPLETUN_KEY)

$(SIMPLETUN_IV):
	head -c 16 /dev/urandom > $(SIMPLETUN_IV)

simpletun_peer:
ifndef SIMPLETUN_PEER
	$(error SIMPLETUN_PEER is not set)
endif

simpletun_net:
ifndef SIMPLETUN_NET
	$(error SIMPLETUN_NET is not set)
endif
