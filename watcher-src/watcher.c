#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cJSON.h>
#include <uv.h>
#include <sys/queue.h>
#include <assert.h>
#include <errno.h>
#include <stdalign.h>

#define DEFAULT_UNIX_DOMAIN_SOCKET_PATH "/tmp/fswatcher.sock"
#define UNUSED __attribute__((unused))
#define UBSAN

#ifdef UBSAN
#define FREE(X) free(X); X = NULL
#else
#define FREE(X) free(X)
#endif

int safe_readlink(const char *pathname, char **buf)
{
    assert(buf != NULL);
    struct stat st;
    if (lstat(pathname, &st) == -1)
    {
        perror("lstat");
        return -1;
    }
    char *_buf = NULL;
    size_t size = st.st_size + 1;
    while (true)
    {
        FREE(_buf);
        _buf = (char *) malloc(size);
        assert(NULL != _buf);
        ssize_t linksize = readlink(pathname, _buf, size);
        if (linksize < 0)
        {
            perror("readlink");
            return -1;
        }
        if ((size_t) linksize < size)
        {
            _buf[linksize] = '\0';
            *buf = _buf;
            return linksize;
        }
        size *= 2;
    }
}

int bind_to_unix_domain_socket(char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd == -1)
    {
        perror("socket");
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
    {
        if (errno == EADDRINUSE)
        {
            int client_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
            if (client_fd == -1)
            {
                perror("socket");
                close(fd);
                return -1;
            }
            if (connect(client_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
            {
                if (unlink(path) == -1)
                {
                    printf("Socket already existed and we failed to unlink it\n");
                    perror("unlink");
                    close(fd);
                    return -1;
                }
                if (bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
                {
                    perror("bind after unlink");
                    close(fd);
                    return -1;
                }
            }
            else
            {
                close(client_fd);
                close(fd);
                printf("Socket is actively being used by another process\n");
                return -1;
            }
        }
        else
        {
            perror("bind");
            close(fd);
            return -1;
        }
    }
    if (listen(fd, 10) == -1)
    {
        perror("listen");
        close(fd);
        return -1;
    }
    if (chmod(path, 0777) == -1)
    {
        perror("chmod");
        close(fd);
        return -1;
    }
    return fd;
}

char *filepath_from_fanotify_even_metadata(struct fanotify_event_metadata *metadata, int mount_fd) 
{
#ifdef UBSAN
    struct fanotify_event_metadata _metadata;
    memcpy(&_metadata, metadata, sizeof(_metadata));
    uintptr_t next_metadata = (uintptr_t) metadata + _metadata.event_len;
#else
    uintptr_t next_metadata = (uintptr_t)metadata + metadata->event_len;
#endif
    struct fanotify_event_info_fid *info_fd = (struct fanotify_event_info_fid *)(metadata + 1);
    char *full_path = NULL;
    while ((uintptr_t) info_fd < next_metadata)
    {
        //printf("Info type: %d\n", info_fd->hdr.info_type);
        struct file_handle *file_handle = (struct file_handle *) (info_fd + 1);
        //printf("File handle size: %d\n", file_handle->handle_bytes);
        
        if (info_fd->hdr.info_type == FAN_EVENT_INFO_TYPE_DFID_NAME)
        {
            int fd = open_by_handle_at(mount_fd, file_handle, O_RDONLY);
            if (fd == -1)
            {
                printf("Failed to open file handle using fd: %d\n", mount_fd);
                perror("open_by_handle_at");
                return NULL;
            }
            char *path = NULL;
            char link_path[4096];
            sprintf(link_path, "/proc/self/fd/%d", fd);
            int linksize = safe_readlink(link_path, &path);
            if (linksize == -1)
            {
                printf("Failed to read link\n");
                path = strdup("???");
            }
            close(fd);

            char *name = (char *) (((uintptr_t) (file_handle + 1)) + file_handle->handle_bytes);
            //printf("DFID_NAME: %s\n", name);
            full_path = (char *) malloc(strlen(path) + strlen(name) + 2);
            sprintf(full_path, "%s/%s", path, name);
            FREE(path);
            //printf("Full path: %s\n", full_path);
        }
        else if (info_fd->hdr.info_type == FAN_EVENT_INFO_TYPE_FID)
        {
            //printf("FID\n");
        }
        

        info_fd = (struct fanotify_event_info_fid *) (((uintptr_t) info_fd) + info_fd->hdr.len);
    }

    return full_path;
}

typedef enum operation_type_t {
    OP_CLOSE_FOR_WRITE,
    OP_REMOVE,
    OP_MOVE_FROM,
    OP_MOVE_TO,
    OP_UNKNOWN,
} operation_type_t;

const char *operation_type_to_string[] = {
    [OP_CLOSE_FOR_WRITE] = "close_for_write",
    [OP_REMOVE] = "remove",
    [OP_MOVE_FROM] = "move_from",
    [OP_MOVE_TO] = "move_to",
    [OP_UNKNOWN] = "unknown",};


typedef struct json_entry_t {
    cJSON *json;
    STAILQ_ENTRY(json_entry_t) entries;
} json_entry_t;


//TODO: Need to have the ability to append to existing buffer if it hasn't been sent yet
//TODO: Need to keep track of how many bytes have been read
typedef struct refcounted_uv_buffer_t {
    int refcount;
    uv_buf_t buf;
} refcounted_uv_buffer_t;

refcounted_uv_buffer_t * refcounted_uv_buffer_from_string(char *str){
    refcounted_uv_buffer_t *buf = (refcounted_uv_buffer_t *) malloc(sizeof(refcounted_uv_buffer_t));
    assert(NULL != buf);
    buf->refcount = 1;
    buf->buf.base = str;
    buf->buf.len = strlen(str);
    return buf;
}

void refcounted_uv_buffer_ref(refcounted_uv_buffer_t *buf){
    buf->refcount++;
}

void refcounted_uv_buffer_unref(refcounted_uv_buffer_t *buf){
    buf->refcount--;
    if (buf->refcount == 0){
        FREE(buf->buf.base);
        FREE(buf);
    }
}



typedef struct loop_state_t {
    int exit_code;
    int fs_fd;
    STAILQ_HEAD(json_queue_t, json_entry_t) json_queue;
    uv_idle_t *send_jsons_handle;
    int *clients;
    size_t clients_count;
    size_t clients_capacity;
    char *compare_path;
} loop_state_t;


void loop_push_json(uv_loop_t *loop, cJSON *json)
{
    loop_state_t *state = (loop_state_t *) loop->data;
    json_entry_t *entry = (json_entry_t *) malloc(sizeof(json_entry_t));
    entry->json = json;
    STAILQ_INSERT_TAIL(&state->json_queue, entry, entries);
}

cJSON * loop_pop_json(uv_loop_t *loop)
{
    loop_state_t *state = (loop_state_t *) loop->data;
    json_entry_t *entry = STAILQ_FIRST(&state->json_queue);
    if (entry == NULL)
    {
        return NULL;
    }
    cJSON *json = entry->json;
    STAILQ_REMOVE_HEAD(&state->json_queue, entries);
    FREE(entry);
    return json;
}

bool loop_has_jsons(uv_loop_t *loop)
{
    loop_state_t *state = (loop_state_t *) loop->data;
    return STAILQ_FIRST(&state->json_queue) != NULL;
}

void loop_add_client(uv_loop_t *loop, int client)
{
    loop_state_t *state = (loop_state_t *) loop->data;
    if (state->clients_capacity == 0)
    {
        state->clients_capacity = 4;
        state->clients = (int *) malloc(sizeof(int) * state->clients_capacity);
        for (size_t i = 0; i < state->clients_capacity; i++)
        {
            state->clients[i] = -1;
        }
    }
    else if (state->clients_count == state->clients_capacity)
    {
        size_t old_capacity = state->clients_capacity;
        state->clients_capacity *= 2;
        state->clients = (int *) realloc(state->clients, sizeof(int) * state->clients_capacity);
        for (size_t i = old_capacity; i < state->clients_capacity; i++)
        {
            state->clients[i] = -1;
        }
    }
    for(size_t i = 0; i < state->clients_capacity; i++)
    {
        if (state->clients[i] == -1)
        {
            state->clients[i] = client;
            state->clients_count++;
            printf("Added %d as a client\n", client);
            return;
        }
    }
    printf("ERROR Should not happen\n");
 }

void loop_remove_client(uv_loop_t *loop, int client)
{
    loop_state_t *state = (loop_state_t *) loop->data;
    for (size_t i = 0; i < state->clients_capacity; i++)
    {
        if (state->clients[i] == client)
        {
            state->clients[i] = -1;
            state->clients_count--;
            break;
        }
    }
    printf("Clients %p\n", (void *) state->clients);

}

void free_handle(uv_handle_t *handle)
{
    FREE(handle);
}

#define UV_TERMINATE_LOOP(loop, reason) \
    ((loop_state_t *) loop->data)->exit_code = reason; \
    uv_stop(loop); \
    return;


void on_client_writeable(uv_poll_t *handle, UNUSED int status, UNUSED int events)
{
    int clientfd = handle->u.fd;
    refcounted_uv_buffer_t *buf = (refcounted_uv_buffer_t *) handle->data;
    printf("Sending to client %d\n", clientfd);
    if (send(clientfd, buf->buf.base, buf->buf.len, 0) == -1)
    {
        if (errno != EPIPE)
        {
            perror("send");
        }
        close(clientfd);
        loop_remove_client(handle->loop, clientfd);
    }
    else
    {
        printf("Sent to client %d\n", clientfd);
    }
    refcounted_uv_buffer_unref(buf);
    uv_poll_stop(handle);
    uv_close((uv_handle_t *) handle, free_handle);

}

void send_jsons(uv_idle_t *handle)
{
    uv_loop_t *loop = handle->loop;
    printf("Loop %p\n", (void *) loop);
    loop_state_t *loop_state = (loop->data);
    cJSON *json = loop_pop_json(loop);
    if (json == NULL)
    {
        return;
    }
    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);
    printf("%s\n", json_string);
    refcounted_uv_buffer_t *buf = refcounted_uv_buffer_from_string(json_string);

    printf("Sending to %zu clients\n", loop_state->clients_count);

    int *client = loop_state->clients;
    for (size_t i = 0; i < loop_state->clients_count; i++)
    {
        while (*client == -1)
        {
            assert(client < &loop_state->clients[loop_state->clients_capacity]);
            client++;
        }
        printf("Preparing Sending to client %d\n", *client);
        uv_poll_t *req = (uv_poll_t *) malloc(sizeof(uv_poll_t));
        printf("0 Loop: %p\n", (void *) loop);
        printf("1 Req: %p  Req->loop: %p\n", (void *) req, (void *) req->loop);
        int pollret = uv_poll_init(loop, req, *client);

        // TODO: Need to append to existing buffer if it hasn't been sent yet
        if (pollret != 0)
        {
            printf("Failed to init poll for FD: %d (already being monitored)\n", *client);
            printf("uv_poll_init: %s\n", uv_strerror(pollret));
            abort();
        }
        printf("2 Req: %p  Req->loop: %p\n", (void *) req, (void *) req->loop);
        req->u.fd  = *client;
        req->data = buf;
        printf("3 Req: %p  Req->loop: %p\n", (void *) req, (void *) req->loop);
        refcounted_uv_buffer_ref(buf);
        printf("4 Req: %p  Req->loop: %p\n", (void *) req, (void *) req->loop);

        uv_poll_start(req, UV_WRITABLE, on_client_writeable);
        client++;
    }

    refcounted_uv_buffer_unref(buf);
    if (!loop_has_jsons(loop))
    {
        uv_idle_stop(handle);
    }
}

void on_unix_domain_connection_attempt(uv_poll_t *handle, UNUSED int status, UNUSED int events)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);


    int fd = accept(handle->u.fd, (struct sockaddr *) &addr, &addrlen);
    if (fd == -1)
    {
        perror("accept");
        UV_TERMINATE_LOOP(handle->loop, 1);
    }
    printf("Accepted client connection\n");
    loop_add_client(handle->loop, fd);
}


void __attribute__((no_sanitize("alignment", "undefined"))) on_fs_event(uv_poll_t *handle, UNUSED int status, UNUSED int events)
{
        loop_state_t *loop_state = (loop_state_t *) handle->loop->data;
        alignas(8) char buf[256];
        int buflen = read(handle->u.fd, buf, sizeof(buf));
        struct fanotify_event_metadata *metadata_itor = (struct fanotify_event_metadata *)  buf;
        //printf("Read %d bytes\n", buflen);
        cJSON *json = cJSON_CreateObject();
        cJSON *json_events = cJSON_CreateArray();
        cJSON_AddItemToObject(json, "events", json_events);
        printf("Buff Address: %p Bufflen: %d\n",buf,  buflen);
#ifdef UBSAN
        struct fanotify_event_metadata _metadata;
        while (memcpy(&_metadata, metadata_itor, sizeof(_metadata)) && FAN_EVENT_OK(&_metadata, buflen))
#else
        while (FAN_EVENT_OK(metadata_itor, buflen))
#endif
        {

#ifdef UBSAN
            struct fanotify_event_metadata *metadata = &_metadata;
#else
            struct fanotify_event_metadata *metadata = metadata_itor;
#endif
            if (metadata->vers != FANOTIFY_METADATA_VERSION)
            {
                printf("Wrong metadata version: %d\n", metadata->vers);
                UV_TERMINATE_LOOP(handle->loop, 1);
            }
            assert(metadata->fd == FAN_NOFD);

            char *filepath = filepath_from_fanotify_even_metadata(metadata_itor, ((loop_state_t *) handle->loop->data)->fs_fd);

            if (filepath == NULL)
            {
                printf("Failed to get filepath\n");
                goto loop_end;
            }
            if (strncmp(filepath, loop_state->compare_path, strlen(loop_state->compare_path)) != 0)
            {
                printf("Skipping %s\n", filepath);
                goto loop_end;
            }
            operation_type_t op = OP_UNKNOWN;
            if (metadata->mask & FAN_CLOSE_WRITE)
            {
                op = OP_CLOSE_FOR_WRITE;
            }


            if (metadata->mask & FAN_MOVED_TO)
            {
                op = OP_MOVE_TO;
            }

            if (metadata->mask & FAN_DELETE) 
            {
                op = OP_REMOVE;
            }
            if (metadata->mask & FAN_MOVED_FROM)
            {
                op = OP_MOVE_FROM;
            }

            cJSON *json_event = cJSON_CreateObject();
            cJSON_AddStringToObject(json_event, "path", filepath);
            cJSON_AddStringToObject(json_event, "operation", operation_type_to_string[op]);
            cJSON_AddItemToArray(json_events, json_event);
loop_end:
            FREE(filepath);
            metadata_itor = FAN_EVENT_NEXT(metadata_itor, buflen);
            printf("Attempting another loop\n");

        }
        if (cJSON_GetArraySize(json_events) == 0)
        {
            cJSON_Delete(json);
            return;
        }
        loop_push_json(handle->loop, json);
        uv_idle_start(((loop_state_t *) handle->loop->data)->send_jsons_handle, send_jsons);
}

void open_handles_cb(uv_handle_t *handle, UNUSED void *arg)
{
    if (!uv_is_closing(handle))
    {
        switch (handle->type)
        {
        case UV_POLL:
            printf("Closing poll handle\n");
            uv_poll_stop((uv_poll_t *) handle);
            if (((uv_poll_t *) handle)->poll_cb == on_client_writeable)
            {
                if (((uv_poll_t *) handle)->data != NULL)
                {
                    refcounted_uv_buffer_unref(((uv_poll_t *) handle)->data);
                }
                uv_close(handle, free_handle);
            }
            else
            {
                uv_close(handle, NULL);
            }
        break;
        case UV_IDLE:
            printf("Closing idle handle\n");
            uv_idle_stop((uv_idle_t *) handle);
            uv_close(handle, NULL);
        break;
        default:
            printf("Unknown handle of type %d\n", handle->type);
        break;
        }
    }
}


void sigint_handler(uv_signal_t *handle, UNUSED int signum)
{
    uv_loop_t *loop = handle->loop;
    printf("SIGINT received\n");
    uv_signal_stop(handle);
    uv_close((uv_handle_t *) handle, NULL);
    uv_walk(loop, open_handles_cb, NULL);
}





int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s <path>\n", argv[0]);
        return 1;
    }
    char *path = argv[1];
    char *canonical_path = realpath(path, NULL);
    if (canonical_path == NULL)
    {
        printf("Failed to get canonical path\n");
        perror("realpath");
        return 1;
    }

    char *compare_path = calloc(1, strlen(canonical_path) + 2);
    strcpy(compare_path, canonical_path);
    if (strlen(compare_path) > 1)
    {
        strcat(compare_path, "/");
    }
    FREE(canonical_path);

    int fs_fd = open(path, O_DIRECTORY | O_RDONLY);
    if (fs_fd == -1)
    {
        FREE(compare_path);
        printf("Failed to open filesystem\n");
        perror("open");
        return 1;
    }
    int fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_FID | FAN_REPORT_DFID_NAME | FAN_UNLIMITED_QUEUE, O_RDONLY);
    if (fd == -1)
    {
        FREE(compare_path);
        printf("Failed to initialize fanotify\n");
        perror("fanotify_init");
        return 1;
    }
    if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_CLOSE_WRITE | FAN_DELETE | FAN_MOVE, 0, path) != 0)
    {
        FREE(compare_path);
        printf("Failed to add watch\n");
        perror("fanotify_mark");
        return 1;
    }

    uv_idle_t idle_handle;

    uv_loop_t *loop = uv_loop_new();
    loop_state_t loop_state = {
        .exit_code = 0,
        .send_jsons_handle = &idle_handle,
        .fs_fd = fs_fd,
        .compare_path = compare_path,
    };
    STAILQ_INIT(&loop_state.json_queue);
    loop->data = &loop_state;
    uv_idle_init(loop, &idle_handle);

    uv_poll_t poll_handle;
    poll_handle.data = NULL;
    uv_poll_init(loop, &poll_handle, fd);
    poll_handle.u.fd = fd;
    uv_poll_start(&poll_handle, UV_READABLE, on_fs_event);



    int listenfd = bind_to_unix_domain_socket(DEFAULT_UNIX_DOMAIN_SOCKET_PATH);
    if (listenfd == -1)
    {
        printf("Failed to bind to unix domain socket\n");
        return 1;
    }

    uv_poll_t unix_domain_poll_handle;
    unix_domain_poll_handle.data = NULL;
    unix_domain_poll_handle.u.fd = listenfd;
    uv_poll_init(loop, &unix_domain_poll_handle, listenfd);
    uv_poll_start(&unix_domain_poll_handle, UV_READABLE, on_unix_domain_connection_attempt);

    uv_signal_t sigint_handle;
    uv_signal_init(loop, &sigint_handle);
    uv_signal_start(&sigint_handle, sigint_handler, SIGINT);


    uv_run(loop, UV_RUN_DEFAULT);

    unlink(DEFAULT_UNIX_DOMAIN_SOCKET_PATH);

    printf("Cliends at end: %p\n", (void *) loop_state.clients);
    for (size_t i = 0; i < loop_state.clients_capacity; i++)
    {
        if (loop_state.clients[i] != -1)
        {
            close(loop_state.clients[i]);
        }
    }

    FREE(loop_state.clients);
    loop_state.clients = NULL;

    while (loop_has_jsons(loop))
    {
        cJSON *json = loop_pop_json(loop);
        cJSON_Delete(json);
    }
    FREE(loop_state.compare_path);

    close(listenfd);
    close(fd);
    close(fs_fd);

    if (uv_loop_close(loop) != 0)
    {
        printf("Failed to close loop\n");
        return 1;
    }

    FREE(loop);

    return 0;

}