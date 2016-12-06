#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <signal.h>

#include "log.h"
#include "longhorn-rpc-client.h"
#include "longhorn-rpc-protocol.h"

int retry_interval = 5;
int retry_counts = 5;
int request_timeout_period = 15; //seconds

int send_request(struct client_connection *conn, struct Message *req) {
        int rc = 0;

        pthread_mutex_lock(&conn->mutex);
        rc = send_msg(conn->fd, req);
        pthread_mutex_unlock(&conn->mutex);
        return rc;
}

int receive_response(struct client_connection *conn, struct Message *resp) {
        int rc = 0;

        rc = receive_msg(conn->fd, resp);
        return rc;
}

void* response_process(void *arg) {
        struct client_connection *conn = arg;
        struct Message *req, *resp;
        struct itimerspec its;
        int ret = 0;

	resp = malloc(sizeof(struct Message));
        if (resp == NULL) {
            perror("cannot allocate memory for resp");
            return NULL;
        }

        while (1) {
                ret = receive_response(conn, resp);
                if (ret != 0) {
                        break;
                }

                switch (resp->Type) {
                case TypeRead:
                case TypeWrite:
                        eprintf("Wrong type for response %d of seq %d\n",
                                        resp->Type, resp->Seq);
                        continue;
                case TypeError:
                        eprintf("Receive error for response %d of seq %d: %s\n",
                                        resp->Type, resp->Seq, (char *)resp->Data);
                        /* fall through so we can response to caller */
                case TypeEOF:
                case TypeResponse:
                        break;
                default:
                        eprintf("Unknown message type %d\n", resp->Type);
                }

                pthread_mutex_lock(&conn->mutex);
                HASH_FIND_INT(conn->msg_hashtable, &resp->Seq, req);
                if (req != NULL) {
                        HASH_DEL(conn->msg_hashtable, req);
                }
                pthread_mutex_unlock(&conn->mutex);

                if (req == NULL) {
                        eprintf("Unknown response sequence %d\n", resp->Seq);
                        free(resp->Data);
                        continue;
                }

                pthread_mutex_lock(&req->mutex);

                //disarm timer
                its.it_value.tv_sec = 0;
                its.it_value.tv_nsec = 0;
                its.it_interval.tv_sec = 0;
                its.it_interval.tv_nsec = 0;
                ret = timer_settime(req->timer, 0, &its, NULL);
                if (ret < 0) {
                        perror("Fail to disarm the request timer");
                }
                eprintf("timer disarmed\n");

                if (resp->Type == TypeResponse || resp->Type == TypeEOF) {
                        req->DataLength = resp->DataLength;
                        memcpy(req->Data, resp->Data, req->DataLength);
                } else if (resp->Type == TypeError) {
                        req->Type = TypeError;
                }
                free(resp->Data);

                pthread_mutex_unlock(&req->mutex);

                pthread_cond_signal(&req->cond);
        }
        free(resp);
        if (ret != 0) {
                eprintf("Receive response returned error");
        }
        return NULL;
}

void start_response_processing(struct client_connection *conn) {
        int rc;

        rc = pthread_create(&conn->response_thread, NULL, &response_process, conn);
        if (rc < 0) {
                perror("Fail to create response thread");
                exit(-1);
        }
}

int new_seq(struct client_connection *conn) {
        return __sync_fetch_and_add(&conn->seq, 1);
}

void request_timeout_handler(union sigval val) {
        struct timeout_val *tv = val.sival_ptr;
        struct client_connection *conn = tv->conn;
        struct Message *req = tv->msg;

        if (req== NULL) {
                return;
        }
        pthread_mutex_lock(&conn->mutex);
        HASH_DEL(conn->msg_hashtable, req);
        pthread_mutex_unlock(&conn->mutex);

        pthread_mutex_lock(&req->mutex);
        req->Type = TypeError;
        eprintf("Timeout request %d due to disconnection", req->Seq);
        pthread_mutex_unlock(&req->mutex);
        pthread_cond_signal(&req->cond);
}

int process_request(struct client_connection *conn, void *buf, size_t count, off_t offset,
                uint32_t type) {
        struct Message *req = malloc(sizeof(struct Message));
        struct sigevent sevp;
        struct itimerspec its;
        struct timeout_val *tv = malloc(sizeof(struct timeout_val));
        int rc = 0;

        pthread_mutex_lock(&conn->mutex);
        if (conn->state != CLIENT_CONN_STATE_OPEN) {
                eprintf("Cannot queue in more request. Connection is not open");
                return -EINVAL;
        }

        HASH_ADD_INT(conn->msg_hashtable, Seq, req);
        pthread_mutex_unlock(&conn->mutex);

        if (req == NULL) {
                perror("cannot allocate memory for req");
                return -EINVAL;
        }

        if (type != TypeRead && type != TypeWrite) {
                eprintf("BUG: Invalid type for process_request %d\n", type);
                rc = -EFAULT;
                goto free;
        }
        req->Seq = new_seq(conn);
        req->Type = type;
        req->Offset = offset;
        req->DataLength = count;
        req->Data = buf;

        if (req->Type == TypeRead) {
                bzero(req->Data, count);
        }

        rc = pthread_cond_init(&req->cond, NULL);
        if (rc < 0) {
                perror("Fail to init phread_cond");
                rc = -EFAULT;
                goto free;
        }
        rc = pthread_mutex_init(&req->mutex, NULL);
        if (rc < 0) {
                perror("Fail to init phread_mutex");
                rc = -EFAULT;
                goto free;
        }

        tv->conn = conn;
        tv->msg = req;

        sevp.sigev_notify = SIGEV_THREAD;
        sevp.sigev_notify_function = request_timeout_handler;
        sevp.sigev_notify_attributes = NULL;
        sevp.sigev_value.sival_ptr = tv;
        rc = timer_create(CLOCK_MONOTONIC, &sevp, &req->timer);
        if (rc < 0) {
                perror("Fail to init timer for request");
                rc = -EFAULT;
                goto free;
        }

        pthread_mutex_lock(&req->mutex);
        rc = send_request(conn, req);
        if (rc < 0) {
                goto out;
        }
        //arm timer
        its.it_value.tv_sec = request_timeout_period;
        its.it_value.tv_nsec = 0;
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
        rc = timer_settime(req->timer, 0, &its, NULL);
        if (rc < 0) {
                perror("Fail to arm the request timer");
                goto out;
        }
        eprintf("timer armed\n");

        pthread_cond_wait(&req->cond, &req->mutex);

        if (req->Type == TypeError) {
                rc = -EFAULT;
        }
out:
        pthread_mutex_unlock(&req->mutex);
free:
        free(tv);
        free(req);
        return rc;
}

int read_at(struct client_connection *conn, void *buf, size_t count, off_t offset) {
        return process_request(conn, buf, count, offset, TypeRead);
}

int write_at(struct client_connection *conn, void *buf, size_t count, off_t offset) {
        return process_request(conn, buf, count, offset, TypeWrite);
}

struct client_connection *new_client_connection(char *socket_path) {
        struct sockaddr_un addr;
        int fd, rc = 0;
        struct client_connection *conn = NULL;
        int i, connected = 0;

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == -1) {
                perror("socket error");
                exit(-1);
        }

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (strlen(socket_path) >= 108) {
                eprintf("socket path is too long, more than 108 characters");
                exit(-EINVAL);
        }

        strncpy(addr.sun_path, socket_path, strlen(socket_path));

        for (i = 0; i < retry_counts; i ++) {
                if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                        connected = 1;
                        break;
		}

                perror("Cannot connect, retrying");
                sleep(retry_interval);
        }
        if (!connected) {
                perror("connection error");
                exit(-EFAULT);
        }

        conn = malloc(sizeof(struct client_connection));
        if (conn == NULL) {
            perror("cannot allocate memory for conn");
            return NULL;
        }

        conn->fd = fd;
        conn->seq = 0;
        conn->msg_hashtable = NULL;

        rc = pthread_mutex_init(&conn->mutex, NULL);
        if (rc < 0) {
                perror("fail to init conn->mutex");
                exit(-EFAULT);
        }

        conn->state = CLIENT_CONN_STATE_OPEN;
        return conn;
}

int shutdown_client_connection(struct client_connection *conn) {
        struct Message *req, *tmp;
        pthread_mutex_lock(&conn->mutex);
        // Prevent future requests
        conn->state = CLIENT_CONN_STATE_CLOSE;

        // Clean up and fail all pending requests
        HASH_ITER(hh, conn->msg_hashtable, req, tmp) {
                HASH_DEL(conn->msg_hashtable, req);

                pthread_mutex_lock(&req->mutex);
                req->Type = TypeError;
                eprintf("Cancel request %d due to disconnection", req->Seq);
                pthread_mutex_unlock(&req->mutex);
                pthread_cond_signal(&req->cond);
        }
        pthread_mutex_unlock(&conn->mutex);

        close(conn->fd);
        free(conn);
        return 0;
}
