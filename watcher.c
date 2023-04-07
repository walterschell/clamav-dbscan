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
        free(_buf);
        _buf = (char *) malloc(size);
        assert(NULL != _buf);
        int linksize = readlink(pathname, _buf, size);
        if (linksize == -1)
        {
            perror("readlink");
            return -1;
        }
        if (linksize < size)
        {
            _buf[linksize] = '\0';
            *buf = _buf;
            return linksize;
        }
        size *= 2;
    }
}


char *filepath_from_fanotify_even_metadata(struct fanotify_event_metadata *metadata, int mount_fd) 
{

    uintptr_t next_metadata = (uintptr_t)metadata + metadata->event_len;
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


            char *name = (char *) (((uintptr_t) (file_handle + 1)) + file_handle->handle_bytes);
            //printf("DFID_NAME: %s\n", name);
            full_path = (char *) malloc(strlen(path) + strlen(name) + 2);
            sprintf(full_path, "%s/%s", path, name);
            free(path);
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
    OP_ADD,
    OP_REMOVE,
    OP_UNKNOWN
} operation_type_t;

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s <path>\n", argv[0]);
        return 1;
    }
    char *path = argv[1];
    int fs_fd = open(path, O_DIRECTORY | O_RDONLY);
    if (fs_fd == -1)
    {
        printf("Failed to open filesystem\n");
        perror("open");
        return 1;
    }
    int fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_FID | FAN_REPORT_DFID_NAME | FAN_UNLIMITED_QUEUE, O_RDONLY);
    if (fd == -1)
    {
        printf("Failed to initialize fanotify\n");
        perror("fanotify_init");
        return 1;
    }
    if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_CLOSE_WRITE | FAN_DELETE | FAN_MOVE, 0, path) != 0)
    {
        printf("Failed to add watch\n");
        perror("fanotify_mark");
        return 1;
    }
    char buf[4096];
    while (true)
    {
        int buflen = read(fd, buf, sizeof(buf));
        struct fanotify_event_metadata *metadata = (struct fanotify_event_metadata *)  buf;
        //printf("Read %d bytes\n", buflen);
        while (FAN_EVENT_OK(metadata, buflen))
        {
            if (metadata->vers != FANOTIFY_METADATA_VERSION)
            {
                printf("Wrong metadata version: %d\n", metadata->vers);
                return 1;
            }
            assert(metadata->fd == FAN_NOFD);

            char *filepath = filepath_from_fanotify_even_metadata(metadata, fs_fd);
            if (filepath == NULL)
            {
                printf("Failed to get filepath\n");
                return 1;
            }
            operation_type_t op = OP_UNKNOWN;
            if (metadata->mask & FAN_CLOSE_WRITE)
            {
                printf("A file was closed for writing: %s\n", filepath);
                op = OP_ADD;
            }

            if (metadata->mask & FAN_MOVED_TO)
            {
                printf("A file was moved to: %s\n", filepath);
                op = OP_ADD;
            }

            if (metadata->mask & FAN_DELETE) 
            {
                printf("A file was removed: %s\n", filepath);
                op = OP_REMOVE;
            }
            if (metadata->mask & FAN_MOVED_FROM)
            {
                printf("A file was moved from: %s\n", filepath);
                op = OP_REMOVE;
            }
            if (op == OP_UNKNOWN)
            {
                printf("Unknown event\n");
            }

            metadata = FAN_EVENT_NEXT(metadata, buflen);
        }
    }

    return 0;

}