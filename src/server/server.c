#include <arpa/inet.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "demon.h"
#include "debug.h"
#include "inet.h"
#include "protocol.h"
#include "server.h"
#include "tcp.h"
#include "tunnel.h"

static char pkey_password[PASSWORD_SIZE];

static void init_ssl()
{
  SSL_load_error_strings();
  SSL_library_init();
  OpenSSL_add_all_algorithms();
}

static void close_ssl()
{
  ERR_free_strings();
}

static int open_socket(in_port_t port)
{
  struct sockaddr_in sin;
  bzero(&sin, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = hton_ip(INADDR_ANY);
  sin.sin_port = hton_port(port);
  return tcp_server(AF_INET, (struct sockaddr *)&sin, sizeof(sin));
}

typedef struct {
  tunnel *tun;
} in_band_arg;

static int in_band_loop(void *state)
{
  in_band_arg *arg = (in_band_arg *)state;
  tunnel_loop(arg->tun);
  return DAEMON_EXIT;
}

static int in_band_teardown(void *state)
{
  in_band_arg *arg = (in_band_arg *)state;
  tunnel_delete(arg->tun);
  free(arg);
  return DAEMON_OK;
}

typedef struct {
  tunnel *tun;
  SSL_CTX *ctx;
  SSL *ssl;
  demon *in_band_d;
} out_of_band_arg;

static int out_of_band_loop(void *state)
{
  out_of_band_arg *arg = (out_of_band_arg *)state;

  minivpn_packet pkt;
  debug("waiting for out-of-band message from client\n");
  int err = minivpn_recv(arg->ssl, MINIVPN_PKT_ANY, &pkt);
  if (err == MINIVPN_ERR_EOF) {
    debug("connection unexpectedly terminated, releasing client\n");
    return DAEMON_EXIT;
  } else if (err == MINIVPN_ERR_TIMEOUT) {
    debug("timed out waiting for client message\n");
    return DAEMON_OK;
  } else if (err != MINIVPN_OK) {
    debug("error receiving out-of-band packet\n");
    return DAEMON_OK;
  }

  switch (pkt.type) {
  case MINIVPN_PKT_DETACH:
    debug("client detached, beginning shutdown process\n");
    return DAEMON_EXIT;
  case MINIVPN_PKT_UPDATE_KEY:
    {
      debug("updating session key\n");
      minivpn_pkt_update_key *data = (minivpn_pkt_update_key *)pkt.data;
      if (!tunnel_set_key(arg->tun, data->key)) {
        minivpn_err(arg->ssl, MINIVPN_ERR_SERV);
      } else {
        minivpn_ack(arg->ssl);
      }
    }
    break;
  case MINIVPN_PKT_UPDATE_IV:
    {
      debug("updating session iv\n");
      minivpn_pkt_update_iv *data = (minivpn_pkt_update_iv *)pkt.data;
      if (!tunnel_set_iv(arg->tun, data->iv)) {
        minivpn_err(arg->ssl, MINIVPN_ERR_SERV);
      } else {
        minivpn_ack(arg->ssl);
      }
    }
    break;
  default:
    debug("received unsupported out-of-band packet type %" PRIu16 "\n", pkt.type);
    // TODO tell the client we can't do that
    // Backoff so we don't spam error messages
    sleep(1);
  }
  return DAEMON_OK;
}

static int out_of_band_teardown(void *state)
{
  out_of_band_arg *arg = (out_of_band_arg *)state;

  minivpn_detach(arg->ssl);

  tunnel_stop(arg->tun);
  demon_join(arg->in_band_d);

  SSL_shutdown(arg->ssl);
  SSL_free(arg->ssl);
  SSL_CTX_free(arg->ctx);
  free(arg);

  return DAEMON_OK;
}

static int pkey_password_cb(char *buf, int size, int rwflag, void *userdata)
{
  (void)rwflag;
  (void)userdata;

  bzero(buf, size);
  strncpy(buf, pkey_password, size - 1);
  return strlen(buf);
}

static void accept_client(const char *cert_file, const char *pkey_file, tunnel_server *tunserv,
                          passwd_db_conn *pwddb, int sockfd, in_port_t udp_port,
                          in_addr_t server_network, in_addr_t server_netmask, demon_pool *pool)
{
  // Use select so we can timeout
  fd_set set;
  FD_ZERO(&set);
  FD_SET(sockfd, &set);

  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;

  int ret = select(sockfd + 1, &set, NULL, NULL, &timeout);

  debug("waiting for connection\n");
  if (ret < 0 && (errno == EINTR)) {
    debug ("timed out waiting for connection\n");
    return;
  }
  if (ret < 0) {
    perror("select");
    return;
  }

  if (!FD_ISSET(sockfd, &set)) {
    debug("no connection available\n");
    return;
  }

  debug("connection available\n");
  struct sockaddr_in sin;
  socklen_t sinlen = sizeof(sin);
  int conn = accept(sockfd, (struct sockaddr *)&sin, &sinlen);
  if (conn < 0) {
    perror("accept");
    goto err_accept;
  }

#ifdef DEBUG
  char client_ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &sin.sin_addr, client_ip_str, INET_ADDRSTRLEN);
  uint16_t client_tcp_port = ntoh_port(sin.sin_port);
#endif
  debug("accepted connection from %s:%" PRIu16 "\n", client_ip_str, client_tcp_port);

  SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
  if (ctx == NULL) {
    ERR_print_errors_fp(stderr);
    goto err_ctx_new;
  }
  if (pkey_password[0] != '\0') {
    SSL_CTX_set_default_passwd_cb(ctx, pkey_password_cb);
  }

  if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) != 1) {
    ERR_print_errors_fp(stderr);
    goto err_use_cert;
  }

  if (SSL_CTX_use_PrivateKey_file(ctx, pkey_file, SSL_FILETYPE_PEM) != 1) {
    ERR_print_errors_fp(stderr);
    goto err_use_pkey;
  }

  SSL *ssl = SSL_new(ctx);
  if (ssl == NULL) {
    ERR_print_errors_fp(stderr);
    goto err_ssl_new;
  }

  if (SSL_set_fd(ssl, conn) != 1) {
    ERR_print_errors_fp(stderr);
    goto err_set_fd;
  }

  debug("beginning SSL handshake with %s:%" PRIu16 "\n", client_ip_str, client_tcp_port);
  int err = SSL_accept(ssl);
  if (err != 1) {
    ERR_print_errors_fp(stderr);
    goto err_ssl_accept;
  }
  debug("SSL handshake complete, beginning minivpn handshake with %s:%" PRIu16 "\n",
    client_ip_str, client_tcp_port);

  tunnel *tun = minivpn_server_handshake(
    ssl, tunserv, pwddb, udp_port, server_network, server_netmask);
  if (tun == NULL) {
    debug("minivpn handshake failed\n");
    goto err_minivpn_handshake;
  } else {
    debug("minivpn handshake succeeded\n");
  }

  demon *ibd;
  in_band_arg *ibarg = (in_band_arg *)malloc(sizeof(in_band_arg));
  ibarg->tun = tun;
  if ((ibd = demon_spawn(pool, NULL, in_band_loop, in_band_teardown, ibarg)) == NULL) {
    debug("error creating in-band demon\n");
    goto err_in_band_create;
  }

  out_of_band_arg *obarg = (out_of_band_arg *)malloc(sizeof(out_of_band_arg));
  obarg->ctx = ctx;
  obarg->ssl = ssl;
  obarg->tun = tun;
  obarg->in_band_d = ibd;
  if ((demon_spawn(pool, NULL, out_of_band_loop, out_of_band_teardown, obarg)) == NULL) {
    debug("error creating out-of-band demon\n");
    goto err_out_of_band_create;
  }

  debug("successfully launched client\n");

  return;

err_out_of_band_create:
  tunnel_stop(tun);
  demon_join(ibd);
  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  close(conn);
  debug("failed to launch client\n");
  return;

err_in_band_create:
  tunnel_delete(tun);
  free(ibarg);
err_minivpn_handshake:
  SSL_shutdown(ssl);
err_ssl_accept:
err_set_fd:
  SSL_free(ssl);
err_ssl_new:
err_use_pkey:
err_use_cert:
  SSL_CTX_free(ctx);
err_ctx_new:
  close(conn);
err_accept:
  debug("failed to launch client\n");
  return;
}

typedef struct {
  char cert_file[FILE_PATH_SIZE];
  char pkey_file[FILE_PATH_SIZE];
  char passwd_db[FILE_PATH_SIZE];
  in_port_t tcp_port;
  in_port_t udp_port;
  in_addr_t network;
  in_addr_t netmask;
  demon_pool *pool;

  // Fields instantiated by setup
  tunnel_server *tunserv;
  passwd_db_conn *pwddb;
  int sockfd;
} server_arg;

static int server_main_setup(void *state)
{
  server_arg *arg = (server_arg *)state;

  arg->sockfd = open_socket(arg->tcp_port);
  if (arg->sockfd < 0) {
    goto err_sock;
  }

  arg->pwddb = passwd_db_connect(arg->passwd_db);
  if (arg->pwddb == NULL) {
    debug("unable to open password database\n");
    goto err_pwddb;
  }

  arg->tunserv = tunnel_server_new(arg->udp_port);
  if (arg->tunserv == NULL) {
    debug("unable to create tunnel server\n");
    goto err_tunserv;
  }

  debug("listening on port %d\n", arg->tcp_port);
  return DAEMON_OK;

err_tunserv:
  passwd_db_close(arg->pwddb);
err_pwddb:
  close(arg->sockfd);
err_sock:
  return DAEMON_ABORT;
}

static int server_main_loop(void *state)
{
  server_arg *arg = (server_arg *)state;

  accept_client(arg->cert_file, arg->pkey_file, arg->tunserv, arg->pwddb, arg->sockfd,
                arg->udp_port, arg->network, arg->netmask, arg->pool);

  return DAEMON_OK;
}

static int server_main_teardown(void *state)
{
  server_arg *arg = (server_arg *)state;
  tunnel_server_delete(arg->tunserv);
  passwd_db_close(arg->pwddb);
  close(arg->sockfd);
  return DAEMON_OK;
}

typedef struct {
  bool halt;
} session;

typedef struct {
  int type;
  char data[100];
} cli_command;
#define CLI_COMMAND_PING 0
#define CLI_COMMAND_STOP 1

typedef struct {
  int type;
  char data[100];
} cli_response;
#define CLI_RESPONSE_OK               0
#define CLI_RESPONSE_INVALID_COMMAND  1

static bool eval_command(session *s, int conn, const cli_command *command)
{
  cli_response res;

  switch (command->type) {
  case CLI_COMMAND_PING:
    debug("responding to ping request\n");
    res.type = CLI_RESPONSE_OK;
    break;
  case CLI_COMMAND_STOP:
    debug("beginning shutdown process\n");
    s->halt = true;
    res.type = CLI_RESPONSE_OK;
    break;
  default:
    debug("invalid CLI command %d\n", command->type);
    res.type = CLI_RESPONSE_INVALID_COMMAND;
  }

  return write_n(conn, &res, sizeof(cli_response));
}

int server_start(const char *cert_file, const char *pkey_file, const char *passwd_db,
                 const char *_pkey_password, const char *cli_socket,
                 in_port_t tcp_port, in_port_t udp_port, in_addr_t network, in_addr_t netmask)
{
  strncpy(pkey_password, _pkey_password, PASSWORD_SIZE);

  // Set up a TCP server to handle CLI commands
  struct sockaddr_un sa;
  bzero(&sa, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path + 1, cli_socket, 106);
  int sockfd = tcp_server(AF_UNIX, (struct sockaddr *)&sa, sizeof(sa));
  if (sockfd < 1) {
    fprintf(stderr, "unable to start cli server\n");
    return 1;
  }

  init_ssl();

  demon_pool *pool = demon_pool_new();
  if (pool == NULL) {
    debug("unable to start demon pool\n");
    return 1;
  }

  server_arg arg;
  bzero(&arg, sizeof(arg));
  strncpy(arg.cert_file, cert_file, FILE_PATH_SIZE);
  strncpy(arg.pkey_file, pkey_file, FILE_PATH_SIZE);
  strncpy(arg.passwd_db, passwd_db, FILE_PATH_SIZE);
  arg.tcp_port = tcp_port;
  arg.udp_port = udp_port;
  arg.network = network;
  arg.netmask = netmask;
  arg.pool = pool;
  if (demon_spawn(pool, server_main_setup, server_main_loop, server_main_teardown, &arg) == NULL) {
    debug("unable to start server demon\n");
    return 1;
  }

  session s;
  s.halt = false;

  while (!s.halt) {
    int conn = accept(sockfd, NULL, NULL);
    if (conn < 0) {
      perror("accept");
      // Back off a little so we don't spam error messages
      sleep(1);
      continue;
    }
    debug("accepted CLI connection\n");

    cli_command command;
    if (!read_n(conn, &command, sizeof(cli_command))) {
      debug("error reading cli command\n");
      goto next_client;
    }

    if (!eval_command(&s, conn, &command)) {
      debug("error evaluating cli command\n");
      goto next_client;
    }

next_client:
    debug("closing CLI connection\n");
    close(conn);
  }

  demon_pool_delete(pool);
  close(sockfd);
  close_ssl();
  return 0;
}

static bool send_cli_command(const char *sock, const cli_command *command, cli_response *response)
{
  bool result;

  struct sockaddr_un sa;
  bzero(&sa, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path + 1, sock, 106);
  int sockfd;
  if ((sockfd = tcp_client(AF_UNIX, (struct sockaddr *)&sa, sizeof(sa))) < 0) {
    return false;
  }

  if (!write_n(sockfd, command, sizeof(cli_command))) {
    result = false;
    goto close_socket;
  }

  if (!read_n(sockfd, response, sizeof(cli_response))) {
    result = false;
    goto close_socket;
  }

  result = true;

close_socket:
  close(sockfd);
  return result;
}

bool server_ping(const char *sock)
{
  cli_command comm;
  cli_response res;

  comm.type = CLI_COMMAND_PING;

  if (!send_cli_command(sock, &comm, &res)) {
    fprintf(stderr, "unable to reach server\n");
    return false;
  }
  return res.type == CLI_RESPONSE_OK;
}

bool server_stop(const char *sock)
{
  cli_command comm;
  cli_response res;

  comm.type = CLI_COMMAND_STOP;

  if (!send_cli_command(sock, &comm, &res)) {
    fprintf(stderr, "unable to reach server\n");
    return false;
  }
  return res.type == CLI_RESPONSE_OK;
}

