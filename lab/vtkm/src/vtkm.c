#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

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

static ssize_t vtkm_proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
  ssize_t ret;

  mutex_lock(&vtkm_lock);
  ret = simple_read_from_buffer(buf, count, ppos, vtkm_buffer, vtkm_buffer_len);
  mutex_unlock(&vtkm_lock);

  return ret;
}

static ssize_t vtkm_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
  size_t copy_len = min(count, (size_t)BUFFER_SIZE - 1);
  ssize_t not_copied;

  if (count == 0) {
    return 0;
  }

  mutex_lock(&vtkm_lock);
  memset(vtkm_buffer, 0, BUFFER_SIZE);
  not_copied = copy_from_user(vtkm_buffer, buf, copy_len);
  vtkm_buffer_len = copy_len - not_copied;
  vtkm_buffer[vtkm_buffer_len] = '\0';
  mutex_unlock(&vtkm_lock);

  if (not_copied != 0) {
    return -EFAULT;
  }

  LOG("received query: %s\n", vtkm_buffer);
  return count;
}

static const struct proc_ops vtkm_proc_ops = {
  .proc_read = vtkm_proc_read,
  .proc_write = vtkm_proc_write,
};

static int __init vtkm_init(void) {
  mutex_lock(&vtkm_lock);
  memset(vtkm_buffer, 0, BUFFER_SIZE);
  vtkm_buffer_len = scnprintf(vtkm_buffer, BUFFER_SIZE, "VTKM ready. Write device id to /proc/%s\n", PROC_FILE_NAME);
  mutex_unlock(&vtkm_lock);

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
