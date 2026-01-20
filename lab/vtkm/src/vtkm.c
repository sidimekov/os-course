#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/stdarg.h>

#define MODULE_NAME "vtkm"
#define PROC_FILE_NAME "vtkm"
#define BUFFER_SIZE 256

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("VTKM procfs interface for block device monitoring");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

static struct proc_dir_entry *vtkm_proc_entry;
static DEFINE_MUTEX(vtkm_lock);
static char vtkm_buffer[BUFFER_SIZE];
static size_t vtkm_buffer_len;

static void vtkm_set_response(const char *fmt, ...) {
  va_list args;

  mutex_lock(&vtkm_lock);
  va_start(args, fmt);
  vtkm_buffer_len = vscnprintf(vtkm_buffer, BUFFER_SIZE, fmt, args);
  va_end(args);
  mutex_unlock(&vtkm_lock);
}

static int vtkm_parse_major_minor(const char *query, dev_t *devt) {
  unsigned int major;
  unsigned int minor;

  if (sscanf(query, "%u:%u", &major, &minor) != 2) {
    return -EINVAL;
  }

  *devt = MKDEV(major, minor);
  return 0;
}

static int vtkm_lookup_by_name(const char *query, dev_t *devt) {
  const char *name = query;

  if (strncmp(query, "/dev/", 5) == 0) {
    name = query + 5;
  }

  *devt = blk_lookup_devt(name, 0);
  if (*devt == 0) {
    return -ENOENT;
  }

  return 0;
}

static void vtkm_format_device_info(const char *query) {
  struct block_device *bdev;
  const struct gendisk *disk;
  sector_t sectors;
  dev_t devt;
  int result;

  if (query[0] == '\0') {
    vtkm_set_response("error: empty query\nusage: echo sda1 > /proc/%s\n", PROC_FILE_NAME);
    return;
  }

  if (strchr(query, ':')) {
    result = vtkm_parse_major_minor(query, &devt);
  } else {
    result = vtkm_lookup_by_name(query, &devt);
  }

  if (result != 0) {
    vtkm_set_response("error: device not found for '%s'\n", query);
    return;
  }

  bdev = blkdev_get_by_dev(devt, FMODE_READ, NULL);
  if (IS_ERR(bdev)) {
    vtkm_set_response("error: cannot open %s\n", query);
    return;
  }

  disk = bdev->bd_disk;
  sectors = bdev_nr_sectors(bdev);
  vtkm_set_response(
    "device=%s\nmajor=%u\nminor=%u\nsize_bytes=%llu\nsize_sectors=%llu\n",
    disk ? disk->disk_name : "unknown",
    MAJOR(bdev->bd_dev),
    MINOR(bdev->bd_dev),
    (unsigned long long)(sectors << 9),
    (unsigned long long)sectors
  );

  blkdev_put(bdev, FMODE_READ);
}

static ssize_t vtkm_proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
  ssize_t ret;

  mutex_lock(&vtkm_lock);
  ret = simple_read_from_buffer(buf, count, ppos, vtkm_buffer, vtkm_buffer_len);
  mutex_unlock(&vtkm_lock);

  return ret;
}

static ssize_t vtkm_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
  char query[BUFFER_SIZE];
  size_t copy_len = min(count, (size_t)BUFFER_SIZE - 1);

  if (count == 0) {
    return 0;
  }

  if (copy_from_user(query, buf, copy_len)) {
    return -EFAULT;
  }
  query[copy_len] = '\0';
  strim(query);

  LOG("received query: %s\n", query);
  vtkm_format_device_info(query);

  return count;
}

static const struct proc_ops vtkm_proc_ops = {
  .proc_read = vtkm_proc_read,
  .proc_write = vtkm_proc_write,
};

static int __init vtkm_init(void) {
  vtkm_set_response("VTKM ready. Write device id to /proc/%s\n", PROC_FILE_NAME);

  vtkm_proc_entry = proc_create(PROC_FILE_NAME, 0666, NULL, &vtkm_proc_ops);
  if (!vtkm_proc_entry) {
    pr_err("failed to create /proc/%s\n", PROC_FILE_NAME);
    return -ENOMEM;
  }

  LOG("VTKM joined the kernel\n");
  return 0;
}

static void __exit vtkm_exit(void) {
  if (vtkm_proc_entry) {
    proc_remove(vtkm_proc_entry);
    vtkm_proc_entry = NULL;
  }

  LOG("VTKM left the kernel\n");
}

module_init(vtkm_init);
module_exit(vtkm_exit);
