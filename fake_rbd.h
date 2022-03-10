#ifndef __FAKE_RBD_H__
#define __FAKE_RBD_H__

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *rbd_image_t;
typedef void *rbd_image_options_t;
typedef void *rbd_pool_stats_t;

typedef void *rbd_completion_t;
typedef void (*rbd_callback_t)(rbd_completion_t cb, void *arg);

#define RBD_MAX_BLOCK_NAME_SIZE 24
#define RBD_MAX_IMAGE_NAME_SIZE 96

/* fio only looks at 'size' */
typedef struct {
  uint64_t size;
  uint64_t obj_size;
  uint64_t num_objs;
  int order;
  char block_name_prefix[RBD_MAX_BLOCK_NAME_SIZE]; /* deprecated */
  int64_t parent_pool;                             /* deprecated */
  char parent_name[RBD_MAX_IMAGE_NAME_SIZE];       /* deprecated */
} rbd_image_info_t;

#define CEPH_RBD_API          __attribute__ ((visibility ("default")))
typedef void *rados_ioctx_t;

int rbd_aio_create_completion(void *cb_arg,
                              rbd_callback_t complete_cb,
                              rbd_completion_t *c);
int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len,
                    rbd_completion_t c);

int rbd_aio_flush(rbd_image_t image, rbd_completion_t c);

void *rbd_aio_get_arg(rbd_completion_t c);

ssize_t rbd_aio_get_return_value(rbd_completion_t c);

int rbd_aio_read(rbd_image_t image, uint64_t off, size_t len,
                 char *buf, rbd_completion_t c);
void rbd_aio_release(rbd_completion_t c);
int rbd_aio_write(rbd_image_t image, uint64_t off, size_t len,
                  const char *buf, rbd_completion_t c);
int rbd_close(rbd_image_t image);
int rbd_stat(rbd_image_t image, rbd_image_info_t *info,
             size_t infosize);

int rbd_open(rados_ioctx_t io, const char *name,
             rbd_image_t *image, const char *snap_name);

int rbd_invalidate_cache(rbd_image_t image);
int rbd_poll_io_events(rbd_image_t image, rbd_completion_t *comps, int numcomp);

int rbd_set_image_notification(rbd_image_t image, int fd, int type);

typedef void *rados_t;
typedef void *rados_config_t;
int rados_conf_read_file(rados_t cluster, const char *path);
int rados_conf_set(rados_t cluster, const char *option,
                                  const char *value);
int rados_connect(rados_t cluster);
int rados_create(rados_t *cluster, const char * const id);
int rados_create2(rados_t *pcluster,
                  const char *const clustername,
                  const char * const name, uint64_t flags);

int rados_ioctx_create(rados_t cluster, const char *pool_name,
                                      rados_ioctx_t *ioctx);
void rados_ioctx_destroy(rados_ioctx_t io);

void rados_shutdown(rados_t cluster);

#ifdef __cplusplus
}
#endif

#endif // __FAKE_RBD_H__
